#ifndef ESP_EVENT_H_STUB
#define ESP_EVENT_H_STUB

#include "esp_stubs.h"
#include <stdint.h>

typedef const char* esp_event_base_t;
typedef int esp_event_handler_instance_t;

#define ESP_EVENT_ANY_ID (-1)
#define IP_EVENT_STA_GOT_IP 200

extern const char* WIFI_EVENT;
extern const char* IP_EVENT;

esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t event_base, int32_t event_id,
                                              void (*event_handler)(void*, esp_event_base_t,
                                                                    int32_t, void*),
                                              void* event_handler_arg,
                                              esp_event_handler_instance_t* instance);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t event_base, int32_t event_id,
                                                esp_event_handler_instance_t instance);

#endif
