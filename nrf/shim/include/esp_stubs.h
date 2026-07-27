// Some portable headers (packet.h) include esp_stubs.h when ESP_PLATFORM is
// undefined; on this target the real shims provide those names.
#pragma once

#include "esp_err.h"
#include "esp_log.h"
