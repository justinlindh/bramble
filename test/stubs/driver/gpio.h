#ifndef DRIVER_GPIO_H_STUB
#define DRIVER_GPIO_H_STUB

#include <stdint.h>
#include "esp_stubs.h"

typedef int gpio_num_t;

typedef struct {
    uint64_t pin_bit_mask;
    int mode;
} gpio_config_t;

#define GPIO_MODE_OUTPUT 1

esp_err_t gpio_config(const gpio_config_t* pGPIOConfig);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);

#endif
