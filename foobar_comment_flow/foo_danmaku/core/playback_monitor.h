#ifndef PLAYBACK_MONITOR_H
#define PLAYBACK_MONITOR_H

#include <foobar2000/SDK/foobar2000.h>
#include <string>

class PlaybackMonitor : public play_callback_impl_base {
public:
    typedef void (*OnNewTrackCallback)(const wchar_t* title, const wchar_t* artist, const wchar_t* album, void* userdata);
    typedef void (*OnPlayStateCallback)(bool playing, void* userdata);

    PlaybackMonitor();
    ~PlaybackMonitor();

    void init();
    void shutdown();

    void setOnNewTrack(OnNewTrackCallback cb, void* userdata);
    void setOnPlayState(OnPlayStateCallback cb, void* userdata);

    // Latest metadb handle reported by foobar2000; used by the UI to fetch
    // album art on a worker thread. Returns an empty handle when nothing
    // is currently playing.
    metadb_handle_ptr getCurrentTrack() const { return m_currentTrack; }

private:
    void on_playback_starting(play_control::t_track_command p_command, bool p_paused) override;
    void on_playback_new_track(metadb_handle_ptr p_track) override;
    void on_playback_stop(play_control::t_stop_reason p_reason) override;
    void on_playback_seek(double p_time) override;
    void on_playback_pause(bool p_state) override;
    void on_playback_edited(metadb_handle_ptr p_track) override;
    void on_playback_dynamic_info(const file_info& p_info) override;
    void on_playback_dynamic_info_track(const file_info& p_info) override;
    void on_playback_time(double p_time) override;
    void on_volume_change(float p_new_val) override;

    static std::wstring utf8ToWide(const char* text);
    static std::wstring formatTrackField(metadb_handle_ptr track, const char* format);

    OnNewTrackCallback m_onNewTrack;
    OnPlayStateCallback m_onPlayState;
    void* m_userdata;
    bool m_registered;
    metadb_handle_ptr m_currentTrack;
};

#endif // PLAYBACK_MONITOR_H
