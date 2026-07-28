#include "esp_system.h"

#include <nrfx.h>

void esp_restart(void) { NVIC_SystemReset(); }
