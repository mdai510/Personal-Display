#pragma once

#include "esp_err.h"
#include <stdint.h>

esp_err_t time_sync_once_with_utc_offset(int32_t utc_offset_seconds, const char *timezone_hint);
