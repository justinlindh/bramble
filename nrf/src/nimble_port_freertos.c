/*
 * NimBLE's FreeRTOS task creation, owned by this port.
 *
 * This replaces upstream's porting/npl/freertos/src/nimble_port_freertos.c
 * (which nrf/CMakeLists.txt therefore does not compile). Upstream's version
 * is a reasonable default for a board whose only job is BLE, but two of its
 * choices are wrong for a node that also runs a LoRa mesh, and both are task
 * parameters rather than logic, so a port-owned copy is the honest place to
 * fix them. The alternative, correcting the priority from inside the host
 * task body in components/ble/ble_server.c, put a number from this
 * scheduler's namespace into code the ESP fleet also compiles.
 *
 * Host task priority. Upstream uses tskIDLE_PRIORITY + 1, which on this
 * target sits below every Bramble task including the mesh and the radio. The
 * pairing state machine advances on this task and LE Secure Connections puts
 * about a second of P-256 work on it, so at priority 1 the exchange was
 * starved and outran the link supervision timeout: pairing never completed.
 * Priority 6 sits above the mesh and radio tasks (5) and below the FreeRTOS
 * timer task (7) and the link layer (8), which must stay the highest
 * priority thing on the chip. esp-nimble makes the same choice, running its
 * host well above application work.
 *
 * Host task stack. Upstream's configMINIMAL_STACK_SIZE + 400 words does not
 * account for a software P-256 keypair, which mbedtls builds on the caller's
 * stack during pairing.
 */
#include <stddef.h>

#include "FreeRTOS.h"
#include "task.h"

#include "nimble/nimble_port.h"

/* Above the mesh and radio tasks (5), below the timer task (7) and the link
 * layer (8). See the file comment. */
#define BRAMBLE_BLE_HOST_TASK_PRIO 6

/* Words. Sized for the LE Secure Connections pairing path, which runs
 * mbedtls P-256 on this task. */
#define BRAMBLE_BLE_HOST_TASK_STACK (configMINIMAL_STACK_SIZE + 1024)

#if NIMBLE_CFG_CONTROLLER
static TaskHandle_t ll_task_h;
#endif
static TaskHandle_t host_task_h;

void nimble_port_freertos_init(TaskFunction_t host_task_fn) {
#if NIMBLE_CFG_CONTROLLER
    /* The link layer has its own event queue and must outrank everything
     * else on the chip; missing its scheduled radio events is fatal to a
     * connection. Upstream's priority is already correct here. */
    xTaskCreate(nimble_port_ll_task_func, "ll", configMINIMAL_STACK_SIZE + 400, NULL,
                configMAX_PRIORITIES - 1, &ll_task_h);
#endif

    xTaskCreate(host_task_fn, "ble", BRAMBLE_BLE_HOST_TASK_STACK, NULL, BRAMBLE_BLE_HOST_TASK_PRIO,
                &host_task_h);
}
