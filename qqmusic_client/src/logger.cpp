/* logger.cpp — one-definition-rule anchor for the global log callback */
#include "logger.h"

namespace qqmusic {
QQMusicLogCallback g_log_cb       = nullptr;
void*              g_log_userdata = nullptr;
} // namespace qqmusic
