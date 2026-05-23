/* music_client.cpp  —  Layer-2 static-library implementation
 *
 * Responsibilities:
 *   - Load / unload provider DLLs at runtime (LoadLibraryW / GetProcAddress)
 *   - Dispatch search + comment calls to the active provider
 *   - Own the cover-art HTTP download (URLOpenBlockingStreamW)
 *   - Forward log messages from providers to the caller's log sink
 */

#include "../include/music_provider.h"
#include "../include/music_client.h"

#include <windows.h>
#include <urlmon.h>     /* URLOpenBlockingStreamW */
#include <objidl.h>     /* IStream                */

#include <new>
#include <string>
#include <vector>
#include <cstdlib>

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

/* ── helpers ─────────────────────────────────────────── */

static MusicClientCtx* ctx(MusicClientHandle h) {
    return reinterpret_cast<MusicClientCtx*>(h);
}

/* Trampoline: provider calls __stdcall MusicLogCallback;
   we forward to the non-__stdcall MusicClientLogCallback stored in the ctx.
   We pass the ctx* as userdata when registering this trampoline. */
static void __stdcall provider_log_trampoline(const wchar_t* msg, void* ud) {
    const MusicClientCtx* c = reinterpret_cast<const MusicClientCtx*>(ud);
    if (c) c->log(msg);
}

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
                music_client_download_bytes(h, cover_url, out_cover_data, out_cover_size);
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

/* ── cover download ──────────────────────────────────── */

int music_client_download_bytes(
    MusicClientHandle h,
    const wchar_t*    url,
    void**            out_data,
    int*              out_size)
{
    if (!h || !url || !*url || !out_data || !out_size) return MUSIC_ERR_PARAM;
    *out_data = nullptr;
    *out_size = 0;

    MusicClientCtx* c = ctx(h);

    IStream* stream = nullptr;
    HRESULT hr = URLOpenBlockingStreamW(nullptr, url, &stream, 0, nullptr);
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
