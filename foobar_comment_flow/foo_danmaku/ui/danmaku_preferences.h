#ifndef DANMAKU_PREFERENCES_H
#define DANMAKU_PREFERENCES_H

// Global user setting: minimum time between two spawned danmaku items.
// Larger value = sparser danmaku. Stored in foobar2000 configuration.
int danmaku_get_spawn_interval_ms();
void danmaku_set_spawn_interval_ms(int valueMs);
int danmaku_default_spawn_interval_ms();

// Vertical lane count. Fewer lanes means larger visual line spacing.
int danmaku_get_track_count();
void danmaku_set_track_count(int value);
int danmaku_default_track_count();

// Playback speed as percent of the historical base speed (2.0 px/frame).
int danmaku_get_speed_percent();
void danmaku_set_speed_percent(int value);
int danmaku_default_speed_percent();
float danmaku_get_base_speed();

int danmaku_get_turntable_speed_percent();
void danmaku_set_turntable_speed_percent(int value);
int danmaku_default_turntable_speed_percent();
float danmaku_get_turntable_speed();


int danmaku_get_bk_aspect_min_tenths();
int danmaku_get_bk_aspect_max_tenths();
void danmaku_set_bk_aspect_min_tenths(int value);
void danmaku_set_bk_aspect_max_tenths(int value);
int danmaku_default_bk_aspect_min_tenths();
int danmaku_default_bk_aspect_max_tenths();
float danmaku_get_bk_aspect_min();
float danmaku_get_bk_aspect_max();

#include <string>

std::string danmaku_get_deepseek_api_key();
void danmaku_set_deepseek_api_key(const char* val);

#endif // DANMAKU_PREFERENCES_H
