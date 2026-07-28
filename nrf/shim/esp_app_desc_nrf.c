#include "esp_app_desc.h"

#ifndef BRAMBLE_GIT_DESCRIBE
#define BRAMBLE_GIT_DESCRIBE "unknown"
#endif

static const esp_app_desc_t s_app_desc = {
    .version = BRAMBLE_GIT_DESCRIBE,
    .secure_version = 0,
};

const esp_app_desc_t* esp_app_get_description(void) { return &s_app_desc; }
