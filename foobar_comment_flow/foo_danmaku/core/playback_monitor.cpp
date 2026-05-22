#include "playback_monitor.h"
#include <windows.h>

PlaybackMonitor::PlaybackMonitor()
    : play_callback_impl_base(0)
    , m_onNewTrack(nullptr)
    , m_onPlayState(nullptr)
    , m_userdata(nullptr)
    , m_registered(false) {
}

PlaybackMonitor::~PlaybackMonitor() {
    shutdown();
}

void PlaybackMonitor::init() {
    if (!m_registered) {
        play_callback_reregister(
            play_callback::flag_on_playback_starting |
            play_callback::flag_on_playback_new_track |
            play_callback::flag_on_playback_stop |
            play_callback::flag_on_playback_pause,
            true);
        m_registered = true;
    }
}

void PlaybackMonitor::shutdown() {
    if (m_registered) {
        play_callback_reregister(0, false);
        m_registered = false;
    }
}

void PlaybackMonitor::setOnNewTrack(OnNewTrackCallback cb, void* userdata) {
    m_onNewTrack = cb;
    m_userdata = userdata;
}

void PlaybackMonitor::setOnPlayState(OnPlayStateCallback cb, void* userdata) {
    m_onPlayState = cb;
    m_userdata = userdata;
}

void PlaybackMonitor::on_playback_starting(play_control::t_track_command p_command, bool p_paused) {
    (void)p_command;
    if (m_onPlayState) {
        m_onPlayState(!p_paused, m_userdata);
    }
}

void PlaybackMonitor::on_playback_new_track(metadb_handle_ptr p_track) {
    if (!m_onNewTrack || p_track.is_empty()) return;

    std::wstring title = formatTrackField(p_track, "[%title%]");
    if (title.empty()) {
        title = formatTrackField(p_track, "[%filename%]");
    }

    std::wstring artist = formatTrackField(p_track, "[%artist%]");
    if (artist.empty()) {
        artist = formatTrackField(p_track, "[%album artist%]");
    }

    m_onNewTrack(title.c_str(), artist.c_str(), m_userdata);
}

void PlaybackMonitor::on_playback_stop(play_control::t_stop_reason p_reason) {
    (void)p_reason;
    if (m_onPlayState) {
        m_onPlayState(false, m_userdata);
    }
}

void PlaybackMonitor::on_playback_pause(bool p_state) {
    if (m_onPlayState) {
        m_onPlayState(!p_state, m_userdata);
    }
}

void PlaybackMonitor::on_playback_seek(double p_time) { (void)p_time; }
void PlaybackMonitor::on_playback_edited(metadb_handle_ptr p_track) { (void)p_track; }
void PlaybackMonitor::on_playback_dynamic_info(const file_info& p_info) { (void)p_info; }
void PlaybackMonitor::on_playback_dynamic_info_track(const file_info& p_info) { (void)p_info; }
void PlaybackMonitor::on_playback_time(double p_time) { (void)p_time; }
void PlaybackMonitor::on_volume_change(float p_new_val) { (void)p_new_val; }

std::wstring PlaybackMonitor::utf8ToWide(const char* text) {
    if (!text || !*text) return std::wstring();

    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (len <= 1) return std::wstring();

    std::wstring result(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &result[0], len);
    return result;
}

std::wstring PlaybackMonitor::formatTrackField(metadb_handle_ptr track, const char* format) {
    if (track.is_empty()) return std::wstring();

    pfc::string8 out;
    if (!track->format_title_legacy(nullptr, out, format, nullptr)) {
        return std::wstring();
    }
    return utf8ToWide(out.c_str());
}
