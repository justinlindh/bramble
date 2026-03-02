#ifndef TEST_STUB_ESP_APP_DESC_H
#define TEST_STUB_ESP_APP_DESC_H

typedef struct {
    char version[32];
    char project_name[32];
    char time[16];
    char date[16];
    char idf_ver[32];
} esp_app_desc_t;

static inline const esp_app_desc_t *esp_app_get_description(void) {
    static esp_app_desc_t desc = {
        .version = "0.0.0-test",
        .project_name = "bramble",
    };
    return &desc;
}

#endif
