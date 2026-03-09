#ifndef DRIVER_GPIO_H_STUB
#define DRIVER_GPIO_H_STUB

#include <stdint.h>
#include "esp_stubs.h"

typedef int gpio_num_t;

typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;

#define GPIO_MODE_OUTPUT 1
#define GPIO_MODE_INPUT 2

#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLUP_ENABLE 1

#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1

#define GPIO_INTR_DISABLE 0

esp_err_t gpio_config(const gpio_config_t* pGPIOConfig);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);
int gpio_get_level(gpio_num_t gpio_num);

#endif
