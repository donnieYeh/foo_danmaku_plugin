/* music_client.cpp  —  Layer-2 static-library implementation
 *
 * Responsibilities:
 *   - Load / unload provider DLLs at runtime (LoadLibraryW / GetProcAddress)
 *   - Dispatch search + comment calls to the active provider
 *   - Own atomic cover-art fetching (provider metadata lookup + HTTP download)
 *   - Forward log messages from providers to the caller's log sink
 */

#include "../include/music_provider.h"
#include "../include/music_client.h"
#include "deepseek_client.h"


#include <windows.h>
#include <urlmon.h>     /* URLOpenBlockingStreamW */
#include <objidl.h>     /* IStream                */

#include <new>
#include <string>
#include <vector>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <cwctype>

/* Pull in urlmon at link time (propagates to the consuming DLL via .obj). */
#pragma comment(lib, "urlmon.lib")

/* ── internal types ──────────────────────────────────── */

struct ProviderSlot {
    HMODULE                    dll    = nullptr;
    MusicProviderHandle        handle = nullptr;
    const MusicProviderVTable* vtable = nullptr;
    std::wstring               path;           /* full DLL path, for introspection */
};

struct MusicClientCtx {
    std::vector<ProviderSlot> providers;
    int                       search_provider_idx = 0; /* set by last successful search */
    std::wstring              last_error;
    MusicClientLogCallback    log_cb       = nullptr;
    void*                     log_userdata = nullptr;

    /* Forward a message to the caller's log sink (if any). */
    void log(const wchar_t* msg) const {
        if (log_cb) log_cb(msg, log_userdata);
    }
    void log(const std::wstring& msg) const { log(msg.c_str()); }
};

struct TrackProviderState {
    bool         tried_resolve    = false;
    bool         resolved         = false;
    bool         comments_failed  = false;
    bool         cover_failed     = false;
    std::wstring song_id;
    std::wstring cover_url;
    int          comment_offset   = 0;
    int          delivered_total  = 0;
};

struct TrackSessionCtx {
    MusicClientCtx*                  client = nullptr;
    std::wstring                     title;
    std::wstring                     artist;
    std::wstring                     album;
    int                              duration_ms = 0;
    std::wstring                     keyword;
    std::vector<TrackProviderState>  provider_states;
    int                              active_comment_provider = -1;
    bool                             deepseek_tried = false;
    bool                             deepseek_success = false;
    DeepSeekCleanResult              deepseek_result;
    std::mutex                       mutex;
};

/* ── helpers ─────────────────────────────────────────── */

static MusicClientCtx* ctx(MusicClientHandle h) {
    return reinterpret_cast<MusicClientCtx*>(h);
}

static TrackSessionCtx* session_ctx(MusicTrackSessionHandle s) {
    return reinterpret_cast<TrackSessionCtx*>(s);
}

static std::wstring clean_artist_part(const std::wstring& part) {
    size_t start = part.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return L"";
    size_t end = part.find_last_not_of(L" \t\r\n");
    std::wstring s = part.substr(start, end - start + 1);

    std::wstring lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower.find(L"作詞") != std::wstring::npos ||
        lower.find(L"作词") != std::wstring::npos ||
        lower.find(L"lyric") != std::wstring::npos ||
        lower.find(L"作曲") != std::wstring::npos ||
        lower.find(L"composer") != std::wstring::npos ||
        lower.find(L"music") != std::wstring::npos ||
        lower.find(L"編曲") != std::wstring::npos ||
        lower.find(L"编曲") != std::wstring::npos ||
        lower.find(L"arrange") != std::wstring::npos ||
        lower.find(L"illustration") != std::wstring::npos ||
        lower.find(L"插画") != std::wstring::npos) {
        return L"";
    }

    const wchar_t* prefixes[] = { L"歌:", L"歌：", L"vocal:", L"vocal：", L"cv:", L"cv：", L"c.v.：", L"c.v.:" };
    for (const wchar_t* prefix : prefixes) {
        std::wstring p(prefix);
        if (lower.compare(0, p.length(), p) == 0) {
            s = s.substr(p.length());
            start = s.find_first_not_of(L" \t\r\n");
            if (start == std::wstring::npos) return L"";
            end = s.find_last_not_of(L" \t\r\n");
            s = s.substr(start, end - start + 1);
            break;
        }
    }

    lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    const wchar_t* suffixes[] = { L"(歌)", L"（歌）", L"(cv)", L"（cv）", L"(vocal)", L"（vocal）" };
    for (const wchar_t* suffix : suffixes) {
        std::wstring suf(suffix);
        if (suf.length() <= lower.length() && 
            lower.compare(lower.length() - suf.length(), suf.length(), suf) == 0) {
            s = s.substr(0, s.length() - suf.length());
            start = s.find_first_not_of(L" \t\r\n");
            if (start == std::wstring::npos) return L"";
            end = s.find_last_not_of(L" \t\r\n");
            s = s.substr(start, end - start + 1);
            break;
        }
    }

    return s;
}

static std::wstring clean_artist(const std::wstring& artist) {
    if (artist.empty()) return L"";

    std::vector<std::wstring> parts;
    std::wstring current;
    for (wchar_t c : artist) {
        if (c == L'/' || c == L';' || c == L',' || c == L'，' || c == L'、' || c == L'|') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }

    std::wstring cleaned;
    for (const auto& part : parts) {
        std::wstring cleaned_part = clean_artist_part(part);
        if (!cleaned_part.empty()) {
            if (!cleaned.empty()) cleaned += L" ";
            cleaned += cleaned_part;
        }
    }

    if (cleaned.empty()) {
        return clean_artist_part(artist);
    }
    return cleaned;
}

static std::wstring clean_title(const std::wstring& title) {
    if (title.empty()) return L"";
    int parens = 0;
    for (wchar_t c : title) {
        if (c == L'(' || c == L'（') parens++;
        else if (c == L')' || c == L'）') parens--;
    }
    std::wstring t = title;
    if (parens > 0) {
        size_t pos = t.find_last_of(L"(（");
        if (pos != std::wstring::npos) {
            t = t.substr(0, pos);
        }
    }
    size_t start = t.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return L"";
    size_t end = t.find_last_not_of(L" \t\r\n");
    t = t.substr(start, end - start + 1);

    /* Fix-0: strip leading track-number prefix, e.g. "3.", "01 ", "02-", "3)"
     * Pattern: one or more digits followed by one or more separator chars
     * (.  )  -  space/tab), only when real content follows. */
    {
        size_t ti = 0;
        while (ti < t.size() && iswdigit(t[ti])) ti++;
        if (ti > 0 && ti < t.size()) {
            wchar_t sep = t[ti];
            if (sep == L'.' || sep == L')' || sep == L'-' ||
                sep == L' ' || sep == L'\t') {
                size_t after = ti;
                while (after < t.size() &&
                       (t[after] == L'.' || t[after] == L')' || t[after] == L'-' ||
                        t[after] == L' '  || t[after] == L'\t')) {
                    after++;
                }
                if (after < t.size()) {  /* guard: ensure actual content remains */
                    t = t.substr(after);
                }
            }
        }
    }

    return t;
}

static std::wstring clean_album(const std::wstring& album) {
    if (album.empty()) return L"";
    std::wstring a = album;
    int parens = 0;
    for (wchar_t c : a) {
        if (c == L'(' || c == L'（') parens++;
        else if (c == L')' || c == L'）') parens--;
    }
    if (parens > 0) {
        size_t pos = a.find_last_of(L"(（");
        if (pos != std::wstring::npos) {
            a = a.substr(0, pos);
        }
    }
    size_t start = a.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return L"";
    size_t end = a.find_last_not_of(L" \t\r\n");
    return a.substr(start, end - start + 1);
}

static std::wstring build_keyword(const std::wstring& title,
                                  const std::wstring& artist,
                                  const std::wstring& album)
{
    std::wstring kw = title;
    if (!artist.empty()) {
        if (!kw.empty()) kw += L" ";
        kw += artist;
    }
    if (!album.empty()) {
        if (!kw.empty()) kw += L" ";
        kw += album;
    }
    return kw;
}

static int resolve_provider_for_session(
    TrackSessionCtx* sess,
    int              provider_idx)
{
    if (!sess || !sess->client) return MUSIC_ERR_PARAM;
    MusicClientCtx* c = sess->client;
    if (provider_idx < 0 || provider_idx >= (int)c->providers.size())
        return MUSIC_ERR_PARAM;

    DWORD tid = GetCurrentThreadId();
    c->log(std::wstring(L"[music_client] [TID:") + std::to_wstring(tid) + L"] resolve_provider enter for provider=" + std::to_wstring(provider_idx));

    std::lock_guard<std::mutex> lock(sess->mutex);

    c->log(std::wstring(L"[music_client] [TID:") + std::to_wstring(tid) + L"] resolve_provider acquired lock for provider=" + std::to_wstring(provider_idx));

    TrackProviderState& st = sess->provider_states[provider_idx];
    if (st.resolved) {
        c->log(std::wstring(L"[music_client] [TID:") + std::to_wstring(tid) + L"] resolve_provider already resolved, returning OK");
        return MUSIC_OK;
    }
    if (st.tried_resolve) {
        c->log(std::wstring(L"[music_client] [TID:") + std::to_wstring(tid) + L"] resolve_provider already tried, returning NOTFOUND");
        return MUSIC_ERR_NOTFOUND;
    }
    st.tried_resolve = true;

    ProviderSlot& p = c->providers[provider_idx];

    std::vector<std::wstring> keywords;
    std::wstring cleaned_title = clean_title(sess->title);
    std::wstring cleaned_artist = clean_artist(sess->artist);
    std::wstring cleaned_album = clean_album(sess->album);

    // Attempt 1: Title + Artist + Album
    std::wstring kw1 = build_keyword(cleaned_title, cleaned_artist, cleaned_album);
    if (!kw1.empty()) keywords.push_back(kw1);

    // Attempt 2: Title + Artist
    std::wstring kw2 = build_keyword(cleaned_title, cleaned_artist, L"");
    if (!kw2.empty() && kw2 != kw1) keywords.push_back(kw2);

    // Attempt 3: Title
    std::wstring kw3 = cleaned_title;
    if (!kw3.empty() && kw3 != kw2 && kw3 != kw1) keywords.push_back(kw3);

    if (keywords.empty()) {
        keywords.push_back(sess->keyword);
    }

    int rc = MUSIC_ERR_NOTFOUND;
    wchar_t song_id[128] = {};
    wchar_t cover_url[2048] = {};

    for (const auto& kw : keywords) {
        c->log(L"[music_client] attempting search for provider=" + std::to_wstring(provider_idx) + L" with keyword: " + kw);
        song_id[0] = L'\0';
        cover_url[0] = L'\0';
        rc = p.vtable->search_song(
            p.handle,
            kw.c_str(),
            song_id, (int)_countof(song_id),
            cover_url, (int)_countof(cover_url));
        if (rc == MUSIC_OK && song_id[0] != L'\0') {
            break;
        }
    }

    if (rc != MUSIC_OK || song_id[0] == L'\0') {
        if (deepseek_has_api_key()) {
            c->log(L"[music_client] First-pass keyword search failed. Triggering DeepSeek fallback...");
            if (!sess->deepseek_tried) {
                sess->deepseek_tried = true;
                std::wstring ds_err;
                sess->deepseek_success = deepseek_clean_metadata(
                    sess->title, sess->artist, sess->album,
                    sess->deepseek_result, ds_err);
                if (!sess->deepseek_success) {
                    c->log(L"[music_client] DeepSeek fallback failed: " + ds_err);
                }
            }

            if (sess->deepseek_success) {
                // Sort/prioritize CJK/Japanese variants to the front so that Japanese/CJK keywords are searched first
                std::stable_sort(sess->deepseek_result.variants.begin(), sess->deepseek_result.variants.end(),
                    [](const DeepSeekSongVariant& a, const DeepSeekSongVariant& b) {
                        auto has_cjk = [](const std::wstring& s) {
                            for (wchar_t c : s) {
                                if (c >= 0x2E80) return true;
                            }
                            return false;
                        };
                        bool a_cjk = has_cjk(a.title);
                        bool b_cjk = has_cjk(b.title);
                        if (a_cjk != b_cjk) {
                            return a_cjk; // True (a has CJK, b doesn't) comes first
                        }
                        return false;
                    });

                bool found = false;
                for (const auto& var : sess->deepseek_result.variants) {
                    std::vector<std::wstring> ds_keywords;
                    std::wstring ds_title = var.title;
                    std::wstring ds_artist = var.artist;
                    std::wstring ds_album = var.album;

                    /* Fix-1 (revised): strip the words that title and album share as a
                     * leading word-prefix from the album part, to avoid keyword duplication.
                     *
                     * e.g. title="Ichigo Complete (Jelly mix)"
                     *      album="Ichigo Complete - Ichigo Mashimaro OP Single"
                     * Shared leading words (case-insensitive): "ichigo", "complete"  (2 words)
                     * Album unique suffix: "Ichigo Mashimaro OP Single"
                     * → kw1 = "Ichigo Complete (Jelly mix) Ichigo Mashimaro OP Single" */
                    std::wstring ds_album_part = ds_album;
                    if (!ds_title.empty() && !ds_album.empty()) {
                        /* Simple word splitter: splits on whitespace */
                        auto split_words = [](const std::wstring& s) {
                            std::vector<std::wstring> toks;
                            size_t i = 0;
                            while (i < s.size()) {
                                while (i < s.size() && iswspace(s[i])) i++;
                                size_t start = i;
                                while (i < s.size() && !iswspace(s[i])) i++;
                                if (i > start) {
                                    std::wstring w = s.substr(start, i - start);
                                    std::transform(w.begin(), w.end(), w.begin(), ::towlower);
                                    toks.push_back(std::move(w));
                                }
                            }
                            return toks;
                        };

                        auto title_toks = split_words(ds_title);
                        auto album_toks = split_words(ds_album);

                        /* Count how many leading words are shared */
                        size_t shared = 0;
                        while (shared < title_toks.size() && shared < album_toks.size() &&
                               title_toks[shared] == album_toks[shared]) {
                            shared++;
                        }

                        if (shared > 0) {
                            /* Find the position in ds_album after skipping `shared` words */
                            size_t pos = 0, skipped = 0;
                            while (skipped < shared && pos < ds_album.size()) {
                                while (pos < ds_album.size() && iswspace(ds_album[pos])) pos++;
                                while (pos < ds_album.size() && !iswspace(ds_album[pos])) pos++;
                                skipped++;
                            }
                            /* Skip separator chars (space, dash, em-dash…) */
                            static const std::wstring sep_chars = L" \t-\u2013\u2014\u3000";
                            size_t si = ds_album.find_first_not_of(sep_chars, pos);
                            ds_album_part = (si != std::wstring::npos) ? ds_album.substr(si) : L"";
                        }
                    }


                    // Attempt 1: Title + Artist + (de-duplicated) Album
                    std::wstring kw1 = build_keyword(ds_title, ds_artist, ds_album_part);
                    if (!kw1.empty()) ds_keywords.push_back(kw1);

                    // Attempt 2: Title + Artist
                    std::wstring kw2 = build_keyword(ds_title, ds_artist, L"");
                    if (!kw2.empty() && kw2 != kw1) ds_keywords.push_back(kw2);

                    // Attempt 3: Title
                    std::wstring kw3 = ds_title;
                    if (!kw3.empty() && kw3 != kw2 && kw3 != kw1) ds_keywords.push_back(kw3);

                    for (const auto& kw_ds : ds_keywords) {
                        c->log(L"[music_client] attempting search for provider=" + std::to_wstring(provider_idx) + L" with DeepSeek cleaned keyword: " + kw_ds);
                        song_id[0] = L'\0';
                        cover_url[0] = L'\0';
                        rc = p.vtable->search_song(
                            p.handle,
                            kw_ds.c_str(),
                            song_id, (int)_countof(song_id),
                            cover_url, (int)_countof(cover_url));
                        if (rc == MUSIC_OK && song_id[0] != L'\0') {
                            found = true;
                            break;
                        }
                    }
                    if (found) {
                        break;
                    }
                }
            }
        } else {
            c->log(L"[music_client] First-pass keyword search failed. DeepSeek API key is not configured, skipping fallback.");
        }
    }

    if (rc != MUSIC_OK || song_id[0] == L'\0') {
        c->last_error = p.vtable->last_error(p.handle);
        c->log(std::wstring(L"[music_client] [TID:") + std::to_wstring(tid) + L"] provider[" + p.path
               + L"] track resolve failed (rc=" + std::to_wstring(rc)
               + L")");
        return (rc == MUSIC_OK) ? MUSIC_ERR_NOTFOUND : rc;
    }

    st.song_id = song_id;
    st.cover_url = cover_url;
    st.resolved = !st.song_id.empty();
    if (!st.resolved) {
        c->last_error = L"Provider resolved empty song id";
        c->log(std::wstring(L"[music_client] [TID:") + std::to_wstring(tid) + L"] resolve_provider returned empty song ID");
        return MUSIC_ERR_NOTFOUND;
    }

    c->log(std::wstring(L"[music_client] [TID:") + std::to_wstring(tid) + L"] track resolved by provider["
           + p.path + L"]");
    return MUSIC_OK;
}

/* Trampoline: provider calls __stdcall MusicLogCallback;
   we forward to the non-__stdcall MusicClientLogCallback stored in the ctx.
   We pass the ctx* as userdata when registering this trampoline. */
static void __stdcall provider_log_trampoline(const wchar_t* msg, void* ud) {
    const MusicClientCtx* c = reinterpret_cast<const MusicClientCtx*>(ud);
    if (c) c->log(msg);
}

/* Internal URL downloader used only to complete Layer-2 atomic cover fetches.
 * The public music_client_download_bytes() wrapper remains for compatibility,
 * but cover-related code paths should call this helper instead. */
static int download_bytes_from_url(
    MusicClientCtx* c,
    const wchar_t*  url,
    void**          out_data,
    int*            out_size);

/* ── lifecycle ───────────────────────────────────────── */

MusicClientHandle music_client_create(void) {
    return new (std::nothrow) MusicClientCtx();
}

void music_client_destroy(MusicClientHandle h) {
    MusicClientCtx* c = ctx(h);
    if (!c) return;
    for (auto& slot : c->providers) {
        if (slot.handle && slot.vtable && slot.vtable->destroy)
            slot.vtable->destroy(slot.handle);
        if (slot.dll) FreeLibrary(slot.dll);
    }
    delete c;
}

/* ── provider loading ────────────────────────────────── */

int music_client_load_provider(
    MusicClientHandle h,
    const wchar_t*    dll_path,
    const wchar_t*    cookie)
{
    if (!h || !dll_path || !*dll_path) return MUSIC_ERR_PARAM;
    MusicClientCtx* c = ctx(h);

    HMODULE dll = LoadLibraryW(dll_path);
    if (!dll) {
        c->last_error  = L"LoadLibraryW failed for: ";
        c->last_error += dll_path;
        c->last_error += L" (error ";
        c->last_error += std::to_wstring(GetLastError());
        c->last_error += L")";
        c->log(c->last_error);
        return MUSIC_ERR_INTERNAL;
    }

    auto get_vtable = reinterpret_cast<music_provider_vtable_fn>(
        GetProcAddress(dll, MUSIC_PROVIDER_VTABLE_EXPORT));
    if (!get_vtable) {
        c->last_error  = L"DLL missing export 'music_provider_vtable': ";
        c->last_error += dll_path;
        c->log(c->last_error);
        FreeLibrary(dll);
        return MUSIC_ERR_INTERNAL;
    }

    const MusicProviderVTable* vtable = get_vtable();
    if (!vtable || !vtable->create || !vtable->destroy ||
        !vtable->search_song || !vtable->get_comments_paged || !vtable->last_error) {
        c->last_error = L"Provider returned an incomplete vtable";
        c->log(c->last_error);
        FreeLibrary(dll);
        return MUSIC_ERR_INTERNAL;
    }

    MusicProviderHandle ph = vtable->create(cookie);
    if (!ph) {
        c->last_error = L"Provider create() returned null handle";
        c->log(c->last_error);
        FreeLibrary(dll);
        return MUSIC_ERR_INTERNAL;
    }

    /* Wire the log trampoline if the provider supports per-instance logging. */
    if (vtable->set_log) {
        vtable->set_log(ph, provider_log_trampoline, c);
    }

    ProviderSlot slot;
    slot.dll    = dll;
    slot.handle = ph;
    slot.vtable = vtable;
    slot.path   = dll_path;
    c->providers.push_back(std::move(slot));

    c->log(std::wstring(L"[music_client] provider loaded: ") + dll_path);
    return MUSIC_OK;
}

/* ── search ──────────────────────────────────────────── */

int music_client_search_song(
    MusicClientHandle h,
    const wchar_t*    keyword,
    wchar_t*          out_song_id,
    int               song_id_buf_wchars,
    void**            out_cover_data,
    int*              out_cover_size)
{
    if (out_cover_data) *out_cover_data = nullptr;
    if (out_cover_size) *out_cover_size = 0;
    if (!h || !keyword || !out_song_id || song_id_buf_wchars < 2)
        return MUSIC_ERR_PARAM;

    MusicClientCtx* c = ctx(h);
    if (c->providers.empty()) {
        c->last_error = L"No provider loaded";
        return MUSIC_ERR_NO_PROVIDER;
    }

    const bool want_cover = (out_cover_data && out_cover_size);
    wchar_t cover_url[2048] = {}; /* internal buffer; never exposed to caller */

    /* Fallback: try each provider in priority order, stop at first success. */
    int last_rc = MUSIC_ERR_NO_PROVIDER;
    for (int i = 0; i < (int)c->providers.size(); i++) {
        ProviderSlot& s = c->providers[i];
        int rc = s.vtable->search_song(
            s.handle,
            keyword,
            out_song_id, song_id_buf_wchars,
            want_cover ? cover_url : nullptr,
            want_cover ? (int)(_countof(cover_url)) : 0);
        if (rc == MUSIC_OK) {
            c->search_provider_idx = i;
            /* Download cover art if the provider returned a URL */
            if (want_cover && cover_url[0]) {
                download_bytes_from_url(c, cover_url, out_cover_data, out_cover_size);
                /* Non-fatal: song_id is valid even if cover download fails */
            }
            return MUSIC_OK;
        }
        c->last_error = s.vtable->last_error(s.handle);
        c->log(std::wstring(L"[music_client] provider[") + s.path
               + L"] search failed (rc=" + std::to_wstring(rc)
               + L"), trying next");
        last_rc = rc;
    }
    return last_rc;
}

/* ── comments ────────────────────────────────────────── */

int music_client_get_comments_paged(
    MusicClientHandle          h,
    const wchar_t*             song_id,
    int                        offset,
    int                        page_limit,
    MusicClientCommentCallback callback,
    void*                      userdata,
    int*                       out_delivered)
{
    if (out_delivered) *out_delivered = 0;
    if (!h || !song_id || !callback || page_limit <= 0 || offset < 0)
        return MUSIC_ERR_PARAM;

    MusicClientCtx* c = ctx(h);
    if (c->providers.empty()) {
        c->last_error = L"No provider loaded";
        return MUSIC_ERR_NO_PROVIDER;
    }
    /* Comments must come from the same provider that resolved the song_id. */
    int idx = c->search_provider_idx;
    if (idx < 0 || idx >= (int)c->providers.size()) idx = 0;
    ProviderSlot& slot_ref = c->providers[idx];
    ProviderSlot* s = &slot_ref;

    /* MusicClientCommentCallback and MusicCommentCallback share the same
     * signature and calling convention (__stdcall), so a direct cast is safe
     * on both x86 and x64. */
    int delivered = 0;
    int rc = s->vtable->get_comments_paged(
        s->handle,
        song_id,
        offset,
        page_limit,
        reinterpret_cast<MusicCommentCallback>(callback),
        userdata,
        &delivered);

    if (out_delivered) *out_delivered = delivered;
    if (rc != MUSIC_OK)
        c->last_error = s->vtable->last_error(s->handle);

    return rc;
}

/* ── track session ───────────────────────────────────── */

int music_client_open_track_session(
    MusicClientHandle        h,
    const MusicTrackQuery*   query,
    MusicTrackSessionHandle* out_session)
{
    if (out_session) *out_session = nullptr;
    if (!h || !query || !query->title || !*query->title || !out_session)
        return MUSIC_ERR_PARAM;

    MusicClientCtx* c = ctx(h);
    if (c->providers.empty()) {
        c->last_error = L"No provider loaded";
        return MUSIC_ERR_NO_PROVIDER;
    }

    TrackSessionCtx* s = new (std::nothrow) TrackSessionCtx();
    if (!s) {
        c->last_error = L"Out of memory creating track session";
        return MUSIC_ERR_INTERNAL;
    }

    s->client      = c;
    s->title       = query->title  ? query->title  : L"";
    s->artist      = query->artist ? query->artist : L"";
    s->album       = query->album  ? query->album  : L"";
    s->duration_ms = query->duration_ms;
    s->keyword     = build_keyword(s->title, s->artist, s->album);
    s->provider_states.resize(c->providers.size());

    *out_session = s;
    c->log(std::wstring(L"[music_client] track session opened: ") + s->keyword);
    return MUSIC_OK;
}

void music_client_close_track_session(MusicTrackSessionHandle session) {
    delete session_ctx(session);
}

int music_client_track_next_comments(
    MusicTrackSessionHandle     session,
    int                         page_limit,
    MusicClientCommentCallback  callback,
    void*                       userdata,
    int*                        out_delivered)
{
    if (out_delivered) *out_delivered = 0;
    if (!session || page_limit <= 0 || !callback) return MUSIC_ERR_PARAM;

    TrackSessionCtx* sess = session_ctx(session);
    MusicClientCtx* c = sess->client;
    if (!c || c->providers.empty()) return MUSIC_ERR_NO_PROVIDER;

    int last_rc = MUSIC_ERR_NO_PROVIDER;
    const int active = sess->active_comment_provider;
    const bool can_retry_active =
        (active >= 0 && active < (int)c->providers.size() &&
         !sess->provider_states[active].comments_failed);

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < (int)c->providers.size(); i++) {
            int provider_idx = -1;
            if (pass == 0) {
                if (!can_retry_active || i > 0) break;
                provider_idx = active;
            } else {
                if (i == active) continue;
                provider_idx = i;
            }

            TrackProviderState& st = sess->provider_states[provider_idx];
            if (st.comments_failed) continue;

            int rc = resolve_provider_for_session(sess, provider_idx);
            if (rc != MUSIC_OK) {
                last_rc = rc;
                continue;
            }

            ProviderSlot& p = c->providers[provider_idx];
            int delivered = 0;
            rc = p.vtable->get_comments_paged(
                p.handle,
                st.song_id.c_str(),
                st.comment_offset,
                page_limit,
                reinterpret_cast<MusicCommentCallback>(callback),
                userdata,
                &delivered);

            if (out_delivered) *out_delivered = delivered;

            if (rc == MUSIC_OK) {
                sess->active_comment_provider = provider_idx;
                c->search_provider_idx = provider_idx;
                st.comment_offset += delivered;
                st.delivered_total += delivered;
                return MUSIC_OK;
            }

            c->last_error = p.vtable->last_error(p.handle);
            c->log(std::wstring(L"[music_client] provider[") + p.path
                   + L"] track comments failed (rc=" + std::to_wstring(rc)
                   + L")");
            last_rc = rc;

            /* Cross-provider fallback is safe before any comments have been
             * delivered.  Once a provider's stream has started, do not mix
             * another platform's comment set into the same track session. */
            if (st.delivered_total > 0 || st.comment_offset > 0) {
                return rc;
            }

            st.comments_failed = true;
            if (out_delivered) *out_delivered = 0;
        }
    }

    return last_rc;
}

int music_client_track_fetch_cover(
    MusicTrackSessionHandle session,
    void**                  out_data,
    int*                    out_size)
{
    if (out_data) *out_data = nullptr;
    if (out_size) *out_size = 0;
    if (!session || !out_data || !out_size) return MUSIC_ERR_PARAM;

    TrackSessionCtx* sess = session_ctx(session);
    MusicClientCtx* c = sess->client;
    if (!c || c->providers.empty()) return MUSIC_ERR_NO_PROVIDER;

    int last_rc = MUSIC_ERR_NO_PROVIDER;
    const int active = sess->active_comment_provider;

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < (int)c->providers.size(); i++) {
            int provider_idx = -1;
            if (pass == 0) {
                if (active < 0 || active >= (int)c->providers.size() || i > 0)
                    break;
                provider_idx = active;
            } else {
                if (i == active) continue;
                provider_idx = i;
            }

            TrackProviderState& st = sess->provider_states[provider_idx];
            if (st.cover_failed) continue;

            int rc = resolve_provider_for_session(sess, provider_idx);
            if (rc != MUSIC_OK) {
                last_rc = rc;
                continue;
            }

            if (st.cover_url.empty()) {
                c->last_error = L"Provider returned no cover URL";
                c->log(std::wstring(L"[music_client] provider[")
                       + c->providers[provider_idx].path
                       + L"] session cover URL empty, trying next");
                st.cover_failed = true;
                last_rc = MUSIC_ERR_NOTFOUND;
                continue;
            }

            rc = download_bytes_from_url(c, st.cover_url.c_str(), out_data, out_size);
            if (rc == MUSIC_OK && *out_data && *out_size > 0) {
                c->search_provider_idx = provider_idx;
                return MUSIC_OK;
            }

            c->log(std::wstring(L"[music_client] provider[")
                   + c->providers[provider_idx].path
                   + L"] session cover download failed (rc="
                   + std::to_wstring(rc) + L"), trying next");
            music_client_free(*out_data);
            *out_data = nullptr;
            *out_size = 0;
            st.cover_failed = true;
            last_rc = rc;
        }
    }

    if (c->last_error.empty())
        c->last_error = L"No provider could fetch cover art for track session";
    return last_rc;
}

/* ── provider introspection ──────────────────────────────── */

int music_client_get_provider_count(MusicClientHandle h) {
    if (!h) return 0;
    return (int)ctx(h)->providers.size();
}

const wchar_t* music_client_get_provider_path(MusicClientHandle h, int index) {
    if (!h) return nullptr;
    auto* c = ctx(h);
    if (index < 0 || index >= (int)c->providers.size()) return nullptr;
    return c->providers[index].path.c_str();
}

int music_client_reorder_providers(
    MusicClientHandle h,
    const int*        new_order,
    int               count)
{
    if (!h || !new_order || count <= 0) return MUSIC_ERR_PARAM;
    auto* c = ctx(h);
    if (count != (int)c->providers.size()) return MUSIC_ERR_PARAM;
    /* Validate indices. */
    for (int i = 0; i < count; i++)
        if (new_order[i] < 0 || new_order[i] >= count) return MUSIC_ERR_PARAM;

    std::vector<ProviderSlot> reordered;
    reordered.reserve(count);
    for (int i = 0; i < count; i++)
        reordered.push_back(std::move(c->providers[new_order[i]]));
    c->providers = std::move(reordered);
    c->search_provider_idx = 0;
    c->log(L"[music_client] provider order updated");
    return MUSIC_OK;
}

/* ── cover fetch / download ──────────────────────────── */

int music_client_fetch_cover(
    MusicClientHandle h,
    const wchar_t*    keyword,
    void**            out_data,
    int*              out_size)
{
    if (out_data) *out_data = nullptr;
    if (out_size) *out_size = 0;
    if (!h || !keyword || !out_data || !out_size) return MUSIC_ERR_PARAM;

    MusicClientCtx* c = ctx(h);
    if (c->providers.empty()) {
        c->last_error = L"No provider loaded";
        return MUSIC_ERR_NO_PROVIDER;
    }

    /* Cover-only operation: unlike music_client_search_song(), a provider is
     * considered successful only if it produces downloadable, non-empty image
     * bytes.  If QQ Music resolves the song but its CDN cover URL fails, keep
     * trying the next provider (e.g. NetEase) instead of returning MUSIC_OK
     * with a null cover. */
    int last_rc = MUSIC_ERR_NO_PROVIDER;
    for (int i = 0; i < (int)c->providers.size(); i++) {
        ProviderSlot& s = c->providers[i];
        wchar_t ignored_song_id[128] = {};
        wchar_t cover_url[2048]      = {};

        int rc = s.vtable->search_song(
            s.handle,
            keyword,
            ignored_song_id, (int)_countof(ignored_song_id),
            cover_url,      (int)_countof(cover_url));

        if (rc != MUSIC_OK) {
            c->last_error = s.vtable->last_error(s.handle);
            c->log(std::wstring(L"[music_client] provider[") + s.path
                   + L"] cover metadata failed (rc=" + std::to_wstring(rc)
                   + L"), trying next");
            last_rc = rc;
            continue;
        }

        if (!cover_url[0]) {
            c->last_error = L"Provider returned no cover URL";
            c->log(std::wstring(L"[music_client] provider[") + s.path
                   + L"] returned no cover URL, trying next");
            last_rc = MUSIC_ERR_NOTFOUND;
            continue;
        }

        rc = download_bytes_from_url(c, cover_url, out_data, out_size);
        if (rc == MUSIC_OK && *out_data && *out_size > 0) {
            c->search_provider_idx = i;
            return MUSIC_OK;
        }

        c->log(std::wstring(L"[music_client] provider[") + s.path
               + L"] cover download failed (rc=" + std::to_wstring(rc)
               + L"), trying next");
        music_client_free(*out_data);
        *out_data = nullptr;
        *out_size = 0;
        last_rc = rc;
    }

    if (c->last_error.empty())
        c->last_error = L"No provider could fetch cover art";
    return last_rc;
}

static int download_bytes_from_url(
    MusicClientCtx* c,
    const wchar_t*  url,
    void**          out_data,
    int*            out_size)
{
    if (!c || !url || !*url || !out_data || !out_size) return MUSIC_ERR_PARAM;
    *out_data = nullptr;
    *out_size = 0;

    IStream* stream = nullptr;
    auto started = std::chrono::steady_clock::now();
    HRESULT hr = URLOpenBlockingStreamW(nullptr, url, &stream, 0, nullptr);
    auto opened = std::chrono::steady_clock::now();
    auto open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(opened - started).count();
    if (open_ms >= 30000) {
        c->log(std::wstring(L"[music_client] cover open took ")
               + std::to_wstring(open_ms) + L"ms (>=30000ms): " + url);
    }
    if (FAILED(hr) || !stream) {
        c->last_error = L"URLOpenBlockingStreamW failed for cover URL";
        c->log(c->last_error);
        return MUSIC_ERR_NETWORK;
    }

    std::vector<BYTE> buf;
    BYTE  block[8192];
    for (;;) {
        ULONG read = 0;
        hr = stream->Read(block, sizeof(block), &read);
        if (read > 0) buf.insert(buf.end(), block, block + read);
        if (FAILED(hr) || read == 0) break;
        if (buf.size() > 20u * 1024 * 1024) break; /* 20 MB sanity cap */
    }
    auto finished = std::chrono::steady_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();
    if (total_ms >= 30000) {
        c->log(std::wstring(L"[music_client] cover download took ")
               + std::to_wstring(total_ms) + L"ms (>=30000ms): " + url);
    }
    stream->Release();

    if (buf.empty()) {
        c->last_error = L"Cover download returned empty response";
        c->log(c->last_error);
        return MUSIC_ERR_NETWORK;
    }

    void* data = std::malloc(buf.size());
    if (!data) {
        c->last_error = L"Out of memory allocating cover buffer";
        return MUSIC_ERR_INTERNAL;
    }
    std::memcpy(data, buf.data(), buf.size());
    *out_data = data;
    *out_size = static_cast<int>(buf.size());
    return MUSIC_OK;
}

int music_client_download_bytes(
    MusicClientHandle h,
    const wchar_t*    url,
    void**            out_data,
    int*              out_size)
{
    if (!h) return MUSIC_ERR_PARAM;
    return download_bytes_from_url(ctx(h), url, out_data, out_size);
}

void music_client_free(void* ptr) {
    std::free(ptr);
}

/* ── diagnostics ─────────────────────────────────────── */

const wchar_t* music_client_last_error(MusicClientHandle h) {
    if (!h) return L"(null handle)";
    const MusicClientCtx* c = ctx(h);
    return c->last_error.empty() ? L"(no error)" : c->last_error.c_str();
}

void music_client_set_log(
    MusicClientHandle      h,
    MusicClientLogCallback cb,
    void*                  userdata)
{
    if (!h) return;
    MusicClientCtx* c   = ctx(h);
    c->log_cb       = cb;
    c->log_userdata = userdata;

    /* Re-wire the trampoline on all already-loaded providers so they also
     * route through the new sink. */
    for (auto& slot : c->providers) {
        if (slot.vtable && slot.vtable->set_log) {
            if (cb)
                slot.vtable->set_log(slot.handle, provider_log_trampoline, c);
            else
                slot.vtable->set_log(slot.handle, nullptr, nullptr);
        }
    }
}

void music_client_set_deepseek_api_key(
    MusicClientHandle h,
    const char*       api_key)
{
    (void)h;
    deepseek_set_api_key(api_key);
}

void music_client_clear_deepseek_cache(MusicClientHandle h)
{
    (void)h;
    deepseek_clear_cache();
}

