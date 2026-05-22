/* logger.cpp — one-definition-rule anchor for the global log callback */
#include "logger.h"

namespace netease {
NeteaseLogCallback g_log_cb       = nullptr;
void*              g_log_userdata = nullptr;
} // namespace netease
