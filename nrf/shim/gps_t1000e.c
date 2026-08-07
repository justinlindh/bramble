/*
 * AG3335 GNSS driver for the T1000-E, satisfying components/gps/include/gps.h
 * over UARTE1. Event-driven (unlike console_uart.c's blocking UARTE0): an
 * nrfx event handler feeds raw bytes into a stream buffer, a dedicated task
 * drains it into gps_feed, and the feed's fix callback (gps_events.c) emits
 * the RPC event.
 *
 * Every power transition (the initial power-on and every later
 * gps_set_enabled() toggle) runs on the gnss task, never on the calling
 * task: gps_init() and gps_set_enabled() only set s_user_enabled and wake
 * the task, then return immediately, so neither the boot path nor the RPC
 * handler that services setGpsEnabled ever blocks on the AG3335's ~1.55s
 * power-on sequence. This also makes s_powered task-private (only the gnss
 * task ever reads or writes it), which is what rules out a check-then-act
 * race on it entirely, rather than needing a lock around it.
 *
 * The power decision itself is not just s_user_enabled: gnss_task's
 * once-a-second duty-cycle evaluation runs it through
 * gps_duty_should_power() (Task 9) against the location-share policy, so
 * an idle mesh node with a long share interval powers the module down
 * between fixes instead of holding it on continuously. s_user_enabled
 * still gates it outright (off overrides everything), and the single
 * power-transition call site above is unchanged.
 *
 * gnss_gpio_idle_init() establishes every AG3335 control line's safe idle
 * state unconditionally from gps_init(), regardless of gps_pref: without
 * it, a pref-off boot would leave EN/VRTC_EN/SLEEP_INT/RTC_INT/RESET
 * floating in power-on-reset state (input, disconnected), so the module's
 * power would end up decided by board leakage instead of by this driver.
 * gnss_power_on() then only changes what differs from that idle state.
 *
 * Every blocking delay inside gnss_power_on() goes through
 * gnss_delay_draining() instead of a bare vTaskDelay(): the sequence
 * blocks the gnss task for close to two seconds while the module is
 * already emitting NMEA and $PAIR ACKs, and the 512B stream buffer holds
 * only ~44ms of bytes at 115200 baud, so a bare sleep would overrun on
 * every single power-on.
 *
 * gps_deinit() tears down in one fixed order, and the order is the whole
 * correctness argument: gate the ISR off (clear s_task), then settle the
 * task's fate (wait for its own confirmation, or force-delete it), then
 * nrfx_uarte_uninit(). Gating first means the ISR can never notify a
 * mid-deletion or freed TCB; uninit last means a still-live task can never
 * reach a de-initialized nrfx driver, whose NRFX_ASSERT is a hard lockup in
 * this build. See the comments in gps_deinit() for the per-step reasoning.
 *
 * P1.06 (Meshtastic's PIN_3V3_EN, "Power to Sensors") is deliberately NOT
 * driven here. Bench testing on the physical T1000-E confirmed GNSS runs
 * without it: no BOARD_PIN_GNSS_RAIL or extra power-on step is needed.
 */
#include "gps.h"

#include <string.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include <stream_buffer.h>
#include "freertos/task.h" /* the IDF-style path: task.h's shim wrapper gives
                            * xTaskCreate byte-semantics for the stack size
                            * below (see nrf/shim/include/freertos/task.h);
                            * a bare #include <task.h> would silently take
                            * the stack size in words instead. */

#include <hal/nrf_gpio.h>
#include <nrfx_uarte.h>

#include "bramble_board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gps_duty.h"
#include "gps_events.h"
#include "gps_feed.h"
#include "gps_pref.h"
#include "mesh_task.h"

static const char* TAG = "gps_t1000e";

/* Statics */
static nrfx_uarte_t s_uarte = NRFX_UARTE_INSTANCE(1);
static uint8_t s_rx_buf[2][64]; /* alternating EasyDMA targets */
/* Index of the static buffer most recently handed to nrfx_uarte_rx(), seeded
 * by the initial queueing in gps_init() and advanced only by the
 * RX_BUF_REQUEST handler, which is the sole supply path in steady state.
 * NRFX_UARTE_EVT_RX_BUF_REQUEST carries no buffer pointer, so this is how
 * that handler knows which of the two is free. */
static uint8_t s_rx_last_idx;
static StreamBufferHandle_t s_stream; /* 512B, ISR -> task */
/* "gnss", prio 5. volatile because uarte_handler() reads it in IRQ context to
 * decide whether to notify, and gps_deinit() clears it to gate that notify
 * off before the task is deleted: the read must be a real load every time,
 * and the store must not be sunk past the teardown calls that follow it. */
static TaskHandle_t volatile s_task;
/* Guards s_feed. volatile and snapshotted at every use site for the same
 * reason as s_task: gps_deinit() clears it while other tasks may be between
 * their null-guard and their take. */
static SemaphoreHandle_t volatile s_mu;
static SemaphoreHandle_t s_tx_done; /* UARTE TX completion */
/* gps_deinit's exit-confirmation from gnss_task. Statically allocated so
 * creating it cannot fail and cannot block: a NULL here, or a reschedule
 * inside pvPortMalloc, would let gnss_task observe s_shutdown with s_stopped
 * not yet published, skip its give, and self-delete behind gps_deinit's
 * back. See gps_deinit() for the full argument. */
static StaticSemaphore_t s_stopped_buf;
/* volatile: the publish-before-request ordering against s_shutdown (also
 * volatile) in gps_deinit() must be structural, not empirical. C only
 * orders volatile accesses against each other, not a volatile against a
 * plain store, so without this the compiler (or gnss_task's read of
 * s_shutdown racing this write) has no guarantee of seeing s_stopped set
 * before s_shutdown goes true. */
static SemaphoreHandle_t volatile s_stopped;
static gps_feed_t s_feed;
static volatile uint32_t s_rx_overruns;   /* stream-buffer-full drops */
static volatile uint32_t s_rx_errors;     /* NRFX_UARTE_EVT_ERROR occurrences */
static volatile uint32_t s_rx_disabled;   /* NRFX_UARTE_EVT_RX_DISABLED recoveries */
static volatile uint32_t s_rx_rearm_fail; /* failed nrfx_uarte_rx from any site */
/* Set when an RX restart attempt fails, cleared when one succeeds. Sticky on
 * purpose: without it a single failed recovery leaves reception dead forever
 * with nothing but a counter to show for it, which is the same silent-death
 * shape this whole path exists to prevent. gnss_task retries on its tick. */
static volatile bool s_rx_needs_restart;
static volatile bool s_shutdown;     /* gps_deinit's request to gnss_task */
static bool s_powered;               /* EN state; task-private, see file header */
static volatile bool s_user_enabled; /* mirrors gps_pref/setGpsEnabled; the one
                                      * cross-task input into the gnss task */

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000ULL); }

/* ------------------------------------------------------------------ */
/*  UARTE event handler (IRQ context): only FromISR APIs here.         */
/* ------------------------------------------------------------------ */

/* Restart reception from scratch. Safe from both the RX_DISABLED handler (IRQ
 * context, where nrfx has already finished its own teardown) and the gnss
 * task's retry tick. Only the first buffer is supplied: nrfx_uarte_rx_enable()
 * inside nrfx_uarte_rx() re-enables the peripheral, re-arms rx_int_mask and
 * triggers STARTRX, whose RXSTARTED raises RX_BUF_REQUEST for the second, so
 * there stays exactly one buffer-supply path. */
static void gnss_rx_restart(void) {
    s_rx_last_idx = 0;
    if (nrfx_uarte_rx(&s_uarte, s_rx_buf[0], sizeof(s_rx_buf[0])) == NRFX_SUCCESS) {
        s_rx_needs_restart = false;
    } else {
        s_rx_rearm_fail++;
        s_rx_needs_restart = true;
    }
}

static void uarte_handler(nrfx_uarte_event_t const* event, void* context) {
    (void)context;
    BaseType_t woken = pdFALSE;

    switch (event->type) {
    case NRFX_UARTE_EVT_RX_DONE: {
        uint8_t* done_buf = event->data.rx.p_buffer;
        size_t len = event->data.rx.length;
        if (len > 0) {
            size_t sent = xStreamBufferSendFromISR(s_stream, done_buf, len, &woken);
            if (sent < len) {
                /* Counts dropped BYTES (matches the api/openapi.yaml
                 * "gps_rx_overruns" description), not drop events. */
                s_rx_overruns += (uint32_t)(len - sent);
            }
        }
        /* Deliberately NO re-arm here: consume the bytes and nothing else.
         * Supplying a buffer from this event is what killed reception on the
         * bench. nrfx calls us from inside endrx_irq_handler(), and the very
         * next thing that function does after we return is
         * nrfy_uarte_shorts_disable(NRF_UARTE_SHORT_ENDRX_STARTRX), which
         * undoes the ENDRX->STARTRX short that rx_buffer_set() had just
         * enabled on our behalf. Every cycle then had to take the manual
         * STARTRX fallback further down that same function, and once a
         * buffer landed late enough for RXTO to already be set, nrfx
         * triggered STOPRX and the resulting RXTO ran on_rx_disabled():
         * that masks rx_int_mask, which includes NRF_UARTE_INT_ERROR_MASK,
         * and powers the receiver down. Reception then stopped permanently
         * and even the error counter froze, which is exactly the signature
         * the bench saw. Buffers are supplied only from RX_BUF_REQUEST
         * below, which nrfx raises from rxstarted_irq_handler(), the
         * ordering the driver is actually written for. */

        /* Snapshot the volatile handle once: test and notify must use the
         * same value, and gps_deinit() clears it precisely so this notify
         * stops happening before the task is deleted. */
        TaskHandle_t task = s_task;
        if (task) {
            vTaskNotifyGiveFromISR(task, &woken);
        }
        break;
    }
    case NRFX_UARTE_EVT_RX_BUF_REQUEST: {
        /* The ONLY place buffers are supplied during normal operation. nrfx
         * raises this from rxstarted_irq_handler() once the receiver has
         * actually started on the previous buffer, so the short it enables
         * survives (nothing disables it afterwards on this path) and the
         * driver stays on its continuous-reception fast path.
         *
         * The event carries no pointer saying which buffer is free. With
         * two buffers alternating between "being filled" and "just
         * supplied", the free one is always the other of the two relative
         * to whichever went out last, so s_rx_last_idx ^ 1 names it.
         * s_rx_last_idx now has exactly one writer in steady state (this
         * handler) plus the seed in gps_init(), and it is only advanced on
         * success: recording a supply that did not happen would let the
         * next request hand nrfx the buffer the receiver is filling. */
        uint8_t idx = (uint8_t)(s_rx_last_idx ^ 1);
        if (nrfx_uarte_rx(&s_uarte, s_rx_buf[idx], sizeof(s_rx_buf[0])) == NRFX_SUCCESS) {
            s_rx_last_idx = idx;
        } else {
            s_rx_rearm_fail++;
        }
        break;
    }
    case NRFX_UARTE_EVT_RX_DISABLED:
        /* Safety net, and the thing whose absence made the bench failure
         * permanent and silent. nrfx reaches on_rx_disabled() from an RXTO
         * after a STOPRX, which masks every RX interrupt (ERROR included)
         * and NULLs both buffer slots; without this case the driver simply
         * stopped talking to us forever. Chain A above should mean this
         * never fires, so it is instrumented rather than trusted:
         * gps_rx_disabled is a permanent diagnostic, and a nonzero value on
         * a bench node means the fast path lost a race somewhere.
         *
         * Recovery is the init sequence, which is legal from here: nrfx has
         * already finished its own teardown before invoking this handler
         * (curr/next NULL, RX flags released), and nrfx_uarte_rx_enable()
         * inside nrfx_uarte_rx() re-enables the peripheral, re-arms
         * rx_int_mask and manually triggers STARTRX. Supplying only the
         * first buffer is deliberate: rx_enable's own STARTRX raises
         * RXSTARTED, whose RX_BUF_REQUEST supplies the second through the
         * case above, which keeps one supply path rather than two. */
        s_rx_disabled++;
        gnss_rx_restart();
        break;
    case NRFX_UARTE_EVT_ERROR:
        /* No re-arm here. nrfx keeps the receiver running through an ERROR
         * event on its own: irq_handler() processes ERROR and then
         * independently processes ENDRX/RXSTARTED/RX_BUF_REQUEST off the
         * same interrupt; error_irq_handler() only invokes the user
         * handler, it never touches the driver's curr/next buffer state.
         * This event's data.error.rx.p_buffer is also never populated by
         * this nrfx version (user_handler_on_error() only fills
         * error_mask, confirmed against drivers/src/nrfx_uarte.c), so
         * re-arming with it here would always fall back to a hardcoded
         * buffer and could alias one physical buffer as both curr and
         * next. Just count it; RX_DONE and RX_BUF_REQUEST above are the
         * driver's actual supported re-arm paths. */
        s_rx_errors++;
        break;
    case NRFX_UARTE_EVT_TX_DONE:
        xSemaphoreGiveFromISR(s_tx_done, &woken);
        break;
    default:
        break;
    }

    portYIELD_FROM_ISR(woken);
}

/* ------------------------------------------------------------------ */
/*  $PAIR command TX                                                   */
/* ------------------------------------------------------------------ */

/* EasyDMA needs a RAM source; the $PAIR string literals below live in
 * flash, so every send copies through this static buffer first. */
static void gnss_tx(const char* cmd, size_t len) {
    static uint8_t s_tx_buf[40];
    if (len == 0 || len > sizeof(s_tx_buf)) {
        ESP_LOGE(TAG, "gnss_tx: bad length %u", (unsigned)len);
        return;
    }
    /* Drain any stale give left by a prior call's timed-out (and possibly
     * still in-flight) transfer, so a late TX_DONE can never be mistaken
     * for this call's completion. */
    xSemaphoreTake(s_tx_done, 0);
    memcpy(s_tx_buf, cmd, len);
    if (nrfx_uarte_tx(&s_uarte, s_tx_buf, len, 0) != NRFX_SUCCESS) {
        ESP_LOGW(TAG, "gnss_tx: nrfx_uarte_tx failed");
        return;
    }
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "gnss_tx: TX_DONE timeout, aborting");
        /* Synchronous abort: guarantees the transfer, and any DMA read of
         * s_tx_buf, has actually stopped before this returns. Without it
         * the next call's memcpy could race a still-in-flight DMA read of
         * the same static buffer. */
        nrfx_uarte_tx_abort(&s_uarte, true);
    }
}

#define GNSS_TX_STR(s) gnss_tx((s), sizeof(s) - 1)

/* ------------------------------------------------------------------ */
/*  Power sequencing (gnss task only; see file header)                 */
/* ------------------------------------------------------------------ */

/* Drains the RX stream buffer for approximately total_ms instead of
 * sleeping through it: gnss_power_on() blocks the gnss task for close to
 * two seconds while the AG3335 is actively talking (NMEA plus $PAIR ACKs),
 * and the 512B stream buffer is only about 44ms deep at 115200 baud, so a
 * bare vTaskDelay() across that span would overrun on every power cycle.
 * xStreamBufferReceive() returns as soon as any byte arrives (trigger
 * level 1), so during active traffic this drains in a tight loop; the 20ms
 * per-iteration cap just bounds how long a quiet stretch can run past the
 * requested total before the loop notices. */
static void gnss_delay_draining(uint32_t total_ms) {
    uint8_t chunk[64];
    TickType_t start = xTaskGetTickCount();
    TickType_t total_ticks = pdMS_TO_TICKS(total_ms);

    for (;;) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= total_ticks) {
            return;
        }
        TickType_t remaining = total_ticks - elapsed;
        TickType_t slice = remaining < pdMS_TO_TICKS(20) ? remaining : pdMS_TO_TICKS(20);

        size_t n = xStreamBufferReceive(s_stream, chunk, sizeof(chunk), slice);
        if (n > 0) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            gps_feed_bytes(&s_feed, chunk, n, now_ms());
            xSemaphoreGive(s_mu);
        }
    }
}

/* Safe-idle configuration for every AG3335 control line, run exactly once
 * and unconditionally from gps_init() regardless of gps_pref_get(); see
 * the file header for why this cannot be skipped on a pref-off boot.
 * gnss_power_on() only changes what differs from this (EN, plus the
 * reset/wake/config sequence). */
static void gnss_gpio_idle_init(void) {
    /* VRTC_EN: set once, NEVER cleared anywhere in this file (grep-able
     * invariant). Buys warm start across power cycles; established here,
     * unconditionally, so a later gps_set_enabled(true) still gets the
     * warm-start benefit even when the pref was off at boot. */
    nrf_gpio_cfg_output(BOARD_PIN_GNSS_VRTC_EN);
    nrf_gpio_pin_set(BOARD_PIN_GNSS_VRTC_EN);

    nrf_gpio_cfg_output(BOARD_PIN_GNSS_SLEEP_INT);
    nrf_gpio_pin_set(BOARD_PIN_GNSS_SLEEP_INT);

    nrf_gpio_cfg_output(BOARD_PIN_GNSS_RTC_INT);
    nrf_gpio_pin_clear(BOARD_PIN_GNSS_RTC_INT);

    nrf_gpio_cfg_input(BOARD_PIN_GNSS_RESETB, NRF_GPIO_PIN_PULLUP);

    nrf_gpio_cfg_output(BOARD_PIN_GNSS_RESET);
    nrf_gpio_pin_clear(BOARD_PIN_GNSS_RESET);

    /* EN idle LOW: gnss_power_on() is the only place this goes HIGH. */
    nrf_gpio_cfg_output(BOARD_PIN_GNSS_EN);
    nrf_gpio_pin_clear(BOARD_PIN_GNSS_EN);
}

static const char* const k_init_cmds[] = {
    "$PAIR066,1,1,1,1,0,0*3A\r\n", /* GPS + GLONASS + GALILEO + BDS */
    "$PAIR062,0,1*3F\r\n",         /* GGA on  */
    "$PAIR062,1,0*3F\r\n",         /* GLL off */
    "$PAIR062,2,0*3C\r\n",         /* GSA off */
    "$PAIR062,3,1*3C\r\n", /* GSV ON: sats_in_view feeds the UI (deviation from Meshtastic) */
    "$PAIR062,4,1*3B\r\n", /* RMC on  */
    "$PAIR062,5,0*3B\r\n", /* VTG off */
    "$PAIR062,6,0*38\r\n", /* ZDA off */
};

/* Re-running the sequence on every unpark is deliberate: deterministic
 * beats optimal, and the commands are idempotent. Only ever called from
 * gnss_task(). */
static void gnss_power_on(void) {
    /* Declared powered up front: this function only ever runs on gnss_task,
     * which is the sole reader too, so there is no window where another
     * task could observe a stale s_powered mid-sequence. */
    s_powered = true;

    /* EN HIGH: the only pin state gnss_gpio_idle_init() (called once,
     * unconditionally, from gps_init()) left at its "off" value.
     * VRTC_EN/SLEEP_INT/RTC_INT/RESET/RESETB already sit where that idle
     * configuration left them; re-driving them here on every power-on
     * would be redundant (idempotent, but pointless), so only EN changes. */
    nrf_gpio_pin_set(BOARD_PIN_GNSS_EN);

    /* Reset pulse. */
    nrf_gpio_pin_set(BOARD_PIN_GNSS_RESET);
    gnss_delay_draining(10);
    nrf_gpio_pin_clear(BOARD_PIN_GNSS_RESET);
    gnss_delay_draining(100);

    /* Wake: RTC_INT pulse, then $PAIR382 spam for 1000ms (25 sends at
     * 40ms). Meshtastic treats the spam as required on this board; bench
     * Task 11 confirms. */
    nrf_gpio_pin_set(BOARD_PIN_GNSS_RTC_INT);
    gnss_delay_draining(3);
    nrf_gpio_pin_clear(BOARD_PIN_GNSS_RTC_INT);
    gnss_delay_draining(50);
    for (int i = 0; i < 25; i++) {
        GNSS_TX_STR("$PAIR382,1*2E\r\n");
        gnss_delay_draining(40);
    }

    /* Config burst, each line followed by 40ms, then save to flash. */
    for (size_t i = 0; i < sizeof(k_init_cmds) / sizeof(k_init_cmds[0]); i++) {
        gnss_tx(k_init_cmds[i], strlen(k_init_cmds[i]));
        gnss_delay_draining(40);
    }
    gnss_delay_draining(250);
    GNSS_TX_STR("$PAIR513*3D\r\n");

    ESP_LOGI(TAG, "GNSS powered on");
}

/* Only ever called from gnss_task(). */
static void gnss_power_off(void) {
    s_powered = false;

    /* EN LOW; VRTC stays HIGH (never cleared). */
    nrf_gpio_pin_clear(BOARD_PIN_GNSS_EN);

    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool had = gps_feed_has_fix(&s_feed, now_ms());
    gps_feed_reset(&s_feed);
    xSemaphoreGive(s_mu);

    if (had) {
        nrf_gps_emit_fix_lost();
    }
    ESP_LOGI(TAG, "GNSS powered off");
}

/* ------------------------------------------------------------------ */
/*  gnss task                                                           */
/* ------------------------------------------------------------------ */

static void gnss_task(void* arg) {
    (void)arg;
    uint8_t chunk[64];
    TickType_t last_duty_tick = xTaskGetTickCount();

    for (;;) {
        /* Woken by the ISR on RX_DONE (near-instant byte reaction) or by
         * gps_set_enabled()/gps_deinit() on a state change; the 100ms bound
         * is a safety net, not the primary wake path. The duty-cycle
         * evaluation below is gated to once a second regardless of how
         * often this wakes, so a gps_set_enabled() toggle takes effect
         * within 1s, not within this 100ms bound. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        if (s_shutdown) {
            break;
        }

        size_t n;
        while ((n = xStreamBufferReceive(s_stream, chunk, sizeof(chunk), 0)) > 0) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            gps_feed_bytes(&s_feed, chunk, n, now_ms());
            xSemaphoreGive(s_mu);
        }

        TickType_t now_tick = xTaskGetTickCount();
        if ((now_tick - last_duty_tick) >= pdMS_TO_TICKS(1000)) {
            last_duty_tick = now_tick;
            /* Retry a failed RX restart. Costs one nrfx call a second only
             * while reception is actually down. */
            if (s_rx_needs_restart && s_powered) {
                gnss_rx_restart();
            }

            /* The only power-transition call site: see the file header.
             * gps_duty_should_power's rule 1 (!user_enabled -> off) fully
             * subsumes the plain on/off-by-preference check this replaced,
             * so there is no separate "want" path left: every power
             * decision, preference or policy driven, goes through here,
             * evaluated once a second. */
            mesh_location_share_state_t share;
            mesh_location_get_share_state(&share);
            gps_duty_inputs_t duty_in = {
                .user_enabled = s_user_enabled,
                .sharing_active = share.sharing_active,
                .interval_s = share.interval_s,
                .now_ms = (uint32_t)(esp_timer_get_time() / 1000),
                .last_send_ms = share.last_send_ms,
            };
            bool want = gps_duty_should_power(&duty_in);
            if (want && !s_powered) {
                gnss_power_on();
            } else if (!want && s_powered) {
                gnss_power_off();
            }
        }
    }

    /* Confirm exit to gps_deinit() before this task is gone: at this point
     * it holds no lock (the loop body above never leaves gnss_power_off()
     * or the byte-drain section mid-way), so gps_deinit() can safely reuse
     * s_mu once it sees this. */
    if (s_stopped) {
        xSemaphoreGive(s_stopped);
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  gps.h                                                               */
/* ------------------------------------------------------------------ */

int gps_init(gps_fix_cb_t cb, void* ctx) {
    /* Unconditional, before anything below can fail and bail out early:
     * the GNSS control lines must never be left floating regardless of
     * what the rest of this function does. */
    gnss_gpio_idle_init();

    s_shutdown = false;

    s_mu = xSemaphoreCreateMutex();
    if (!s_mu) {
        ESP_LOGE(TAG, "gps_init: mutex create failed");
        return -1;
    }
    s_tx_done = xSemaphoreCreateBinary();
    if (!s_tx_done) {
        ESP_LOGE(TAG, "gps_init: tx semaphore create failed");
        vSemaphoreDelete(s_mu);
        s_mu = NULL;
        return -1;
    }
    s_stream = xStreamBufferCreate(512, 1);
    if (!s_stream) {
        ESP_LOGE(TAG, "gps_init: stream buffer create failed");
        vSemaphoreDelete(s_tx_done);
        s_tx_done = NULL;
        vSemaphoreDelete(s_mu);
        s_mu = NULL;
        return -1;
    }

    gps_feed_init(&s_feed, cb, ctx);

    nrfx_uarte_config_t cfg = NRFX_UARTE_DEFAULT_CONFIG(BOARD_PIN_GNSS_TX, BOARD_PIN_GNSS_RX);
    cfg.baudrate = NRF_UARTE_BAUDRATE_115200;
    if (nrfx_uarte_init(&s_uarte, &cfg, uarte_handler) != NRFX_SUCCESS) {
        ESP_LOGE(TAG, "gps_init: nrfx_uarte_init failed");
        vStreamBufferDelete(s_stream);
        s_stream = NULL;
        vSemaphoreDelete(s_tx_done);
        s_tx_done = NULL;
        vSemaphoreDelete(s_mu);
        s_mu = NULL;
        return -1;
    }

    /* Queue both RX buffers: nrfx keeps one active plus one pending, which is
     * what makes the re-arm gap in uarte_handler survivable. Our own ISR
     * usually beats us to the second one, and that is fine.
     *
     * The first nrfx_uarte_rx() triggers STARTRX from inside rx_buffer_set(),
     * RXSTARTED fires straight away, and nrfx's rxstarted_irq_handler()
     * raises NRFX_UARTE_EVT_RX_BUF_REQUEST off it, so uarte_handler() can
     * supply the second buffer before the second call below even runs. Two
     * consequences, and getting either wrong breaks init outright:
     *
     * - s_rx_last_idx is SEEDED BEFORE the first call, never assigned after
     *   it. Assigning after would clobber whatever the ISR just set, leaving
     *   it claiming s_rx_buf[0] was supplied last when s_rx_buf[1] actually
     *   was, and the next RX_BUF_REQUEST would then hand nrfx the buffer it
     *   is currently filling. Seeding is also what makes a second gps_init()
     *   after a gps_deinit() start from a known value rather than inheriting
     *   the previous session's.
     * - NRFX_ERROR_BUSY from the SECOND call is success, not failure. It
     *   means rx_buffer_set() found both of nrfx's slots already occupied
     *   (drivers/src/nrfx_uarte.c:1408), i.e. the ISR queued the buffer we
     *   were about to queue. This is the normal outcome on real hardware:
     *   the T1000-E bench hit it on every boot, and treating it as an error
     *   failed gps_init() with -1 every time, so GNSS never started at all.
     *   No aliasing results, because the handler supplies s_rx_last_idx ^ 1
     *   and the seed above makes that s_rx_buf[1]: precisely the buffer this
     *   second call would have supplied.
     *
     * Everything else stays fatal, including BUSY from the FIRST call, which
     * would mean something other than this driver had already armed the
     * receiver rather than a benign lost race. The post-success assignment
     * below is safe for the same reason the seed is needed: the module is
     * still unpowered here (EN is low until gnss_power_on() runs on the gnss
     * task), so no bytes can arrive and RX_BUF_REQUEST is the only ISR path
     * reachable, and once both slots are full it can only return BUSY and
     * leave s_rx_last_idx alone. */
    s_rx_last_idx = 0;
    nrfx_err_t rx0 = nrfx_uarte_rx(&s_uarte, s_rx_buf[0], sizeof(s_rx_buf[0]));
    nrfx_err_t rx1 = NRFX_SUCCESS;
    if (rx0 == NRFX_SUCCESS) {
        rx1 = nrfx_uarte_rx(&s_uarte, s_rx_buf[1], sizeof(s_rx_buf[1]));
        if (rx1 == NRFX_SUCCESS) {
            s_rx_last_idx = 1;
        }
    }
    if (rx0 != NRFX_SUCCESS || (rx1 != NRFX_SUCCESS && rx1 != NRFX_ERROR_BUSY)) {
        s_rx_rearm_fail++;
        ESP_LOGE(TAG, "gps_init: nrfx_uarte_rx queueing failed (rx0=0x%08x rx1=0x%08x)",
                 (unsigned)rx0, (unsigned)rx1);
        nrfx_uarte_uninit(&s_uarte);
        vStreamBufferDelete(s_stream);
        s_stream = NULL;
        vSemaphoreDelete(s_tx_done);
        s_tx_done = NULL;
        vSemaphoreDelete(s_mu);
        s_mu = NULL;
        return -1;
    }

    /* Read the persisted preference here, inside the nRF driver only: the
     * ESP32 gps_init() does not read gps_pref, and gps.h's cross-platform
     * contract is not changing to match, so this stays a T1000-E-specific
     * detail. gps_init() still registers the fix callback either way (ESP
     * boot parity: a later gps_set_enabled(true) works without a second
     * init), it just skips the first power-on when the pref is off. The
     * gnss task below does that skip itself (see gnss_task()'s
     * want/s_powered check), so gps_init() never blocks on the AG3335's
     * power-on sequence regardless of the pref. */
    s_user_enabled = gps_pref_get();

    /* Created into a local, then published: xTaskCreate() wants a plain
     * TaskHandle_t*, and s_task is volatile (see its declaration). The gap
     * where the task runs but the ISR still sees s_task == NULL costs
     * nothing: the notify is only a wake accelerator, and gnss_task()'s
     * 100ms ulTaskNotifyTake() bound covers it. */
    TaskHandle_t task = NULL;
    if (xTaskCreate(gnss_task, "gnss", 3072, NULL, 5, &task) != pdPASS) {
        ESP_LOGE(TAG, "gps_init: task create failed");
        nrfx_uarte_uninit(&s_uarte);
        vStreamBufferDelete(s_stream);
        s_stream = NULL;
        vSemaphoreDelete(s_tx_done);
        s_tx_done = NULL;
        vSemaphoreDelete(s_mu);
        s_mu = NULL;
        return -1;
    }
    s_task = task;

    ESP_LOGI(TAG, "gps_init complete (pref=%s)", s_user_enabled ? "on" : "off");
    return 0;
}

int gps_set_enabled(bool enabled) {
    /* Snapshot once. Guarding on s_task and then notifying through it as two
     * separate volatile loads lets gps_deinit() NULL it in between, and
     * xTaskNotifyGive(NULL) trips configASSERT, i.e. bramble_assert_failed
     * and a reset. What remains after the snapshot is a much narrower and
     * strictly better-behaved window: this task would have to stay preempted
     * across all of gps_deinit's teardown (up to the 5500ms wait plus the
     * delete) for `task` to go stale, and notifying a task that is merely
     * mid-teardown is harmless. Closing even that would mean holding a
     * critical section across xTaskNotifyGive(), which is not legal. */
    TaskHandle_t task = s_task;
    if (task == NULL) {
        return -1;
    }
    s_user_enabled = enabled;
    xTaskNotifyGive(task);
    return 0;
}

bool gps_has_fix(void) {
    /* Snapshot s_mu once, here and in every query below: gps_deinit() clears
     * it, so re-reading it between the guard and the take/give could take a
     * live mutex and then give a NULL one. */
    SemaphoreHandle_t mu = s_mu;
    if (!mu) {
        return false;
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    bool has_fix = gps_feed_has_fix(&s_feed, now_ms());
    xSemaphoreGive(mu);
    return has_fix;
}

bool gps_get_position(bramble_position_t* out) {
    SemaphoreHandle_t mu = s_mu;
    if (!mu) {
        return false;
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    bool ok = gps_feed_get_position(&s_feed, now_ms(), out);
    xSemaphoreGive(mu);
    return ok;
}

bool gps_get_utc_hm(uint8_t* hour, uint8_t* min) {
    SemaphoreHandle_t mu = s_mu;
    if (!mu) {
        return false;
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    bool ok = gps_feed_get_utc_hm(&s_feed, now_ms(), hour, min);
    xSemaphoreGive(mu);
    return ok;
}

bool gps_get_utc_date(uint16_t* year, uint8_t* month, uint8_t* day) {
    SemaphoreHandle_t mu = s_mu;
    if (!mu) {
        return false;
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    bool ok = gps_feed_get_utc_date(&s_feed, now_ms(), year, month, day);
    xSemaphoreGive(mu);
    return ok;
}

void gps_get_stats(gps_stats_t* out) {
    if (out == NULL) {
        return;
    }
    SemaphoreHandle_t mu = s_mu;
    if (!mu) {
        memset(out, 0, sizeof(*out));
        /* Zero would read as "the feed started this instant"; with no driver
         * running, nothing has arrived, which is what the sentinel says. */
        out->nmea_age_s = GPS_STATS_NMEA_NEVER;
        return;
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    gps_feed_get_stats(&s_feed, now_ms(), out);
    xSemaphoreGive(mu);
}

void gps_get_debug(gps_debug_t* out) {
    if (out == NULL) {
        return;
    }
    SemaphoreHandle_t mu = s_mu;
    if (!mu) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    out->rx_bytes_total = s_feed.rx_bytes_total;
    out->rx_lines_total = s_feed.rx_lines_total;
    strncpy(out->chip, s_feed.chip_banner, sizeof(out->chip) - 1);
    out->chip[sizeof(out->chip) - 1] = '\0';
    xSemaphoreGive(mu);
    /* ISR-incremented counters, not gps_feed state: s_mu guards s_feed, not
     * these, so they are read outside the critical section above. Each is a
     * single word (atomic enough on Cortex-M) and merely diagnostic. */
    out->rx_overruns = s_rx_overruns;
    out->rx_errors = s_rx_errors;
    out->rx_disabled = s_rx_disabled;
    out->rx_rearm_fail = s_rx_rearm_fail;
}

void gps_deinit(void) {
    /* Capture and clear together, under one critical section. Two effects:
     *
     * 1. It gates the ISR off FIRST, before anything can make `task` stale.
     *    Past this store uarte_handler() reads s_task == NULL and skips its
     *    vTaskNotifyGiveFromISR() forever, so neither the confirmed path
     *    (gnss_task self-deletes via vTaskDelete(NULL), and the idle task
     *    frees its TCB some time after that) nor the forced path below can
     *    leave the ISR notifying a task that is mid-deletion or already
     *    freed. Doing it here rather than after the wait is what makes that
     *    unconditional: s_shutdown is still false, so gnss_task provably has
     *    not exited yet, and any notify that lands before this store
     *    therefore targets a live, undeleted task.
     * 2. Capturing inside the same section makes gps_deinit idempotent and
     *    safe against two callers: exactly one of them comes out with a
     *    non-NULL `task` and owns the teardown below; the other observes
     *    NULL and early-returns after forcing EN low, touching nothing
     *    else, since s_mu/s_tx_done/s_stream may still be in live use by
     *    the owning caller's still-running gnss_task.
     *
     * The critical section is the ordering guarantee. On this port
     * (GCC_ARM_CM4F) taskENTER_CRITICAL() raises BASEPRI to
     * configMAX_SYSCALL_INTERRUPT_PRIORITY (2 << (8 - 3) = 0x40), which masks
     * every interrupt whose priority value is numerically >= 0x40; UARTE1
     * runs at NRFX_UARTE_DEFAULT_CONFIG_IRQ_PRIORITY = 7 (0xE0), so it is
     * masked here. It has to be: uarte_handler() calls ...FromISR APIs, which
     * are only legal at or below that ceiling. s_task's volatile qualifier
     * does the rest, keeping the compiler from sinking the store past the
     * teardown calls that follow. */
    taskENTER_CRITICAL();
    TaskHandle_t task = s_task;
    s_task = NULL;
    taskEXIT_CRITICAL();

    if (task == NULL) {
        /* No task to tear down: either GPS was never initialized, or a
         * concurrent second caller lost the race above to a first caller
         * that is still tearing down (see the critical section comment).
         * Force EN low so a never-initialized call still leaves the module
         * off, then stop: the resources below may still be in live use by
         * the first caller's still-running gnss_task, and deleting them
         * here would pull them out from under it. */
        nrf_gpio_pin_clear(BOARD_PIN_GNSS_EN);
        return;
    }

    /* Publish the confirmation channel BEFORE requesting the shutdown,
     * never the other way round. gnss_task's exit path is "see s_shutdown,
     * give s_stopped, self-delete", so if it observes s_shutdown while
     * s_stopped is still NULL it skips the give and self-deletes silently;
     * this function would then wait out the full budget and force-delete an
     * already-reclaimed TCB, double-decrement uxCurrentNumberOfTasks and run
     * prvDeleteTCB on freed memory that is still on the termination list.
     * That window is not hypothetical with a dynamically created semaphore:
     * gnss_task can wake on its own 100ms timeout inside it, and
     * pvPortMalloc's vTaskSuspendAll / xTaskResumeAll pair is itself a
     * reschedule point. The static variant removes both halves of the
     * problem, since it neither allocates nor can return NULL. */
    SemaphoreHandle_t stopped = xSemaphoreCreateBinaryStatic(&s_stopped_buf);
    s_stopped = stopped;
    s_shutdown = true;

    /* Losing the ISR wake for the duration of the wait below is deliberate
     * and harmless: gnss_task still polls the stream buffer every 100ms,
     * gnss_delay_draining() blocks on the stream buffer rather than on
     * notifications, and the task is on its way out. */
    xTaskNotifyGive(task);

    /* Budget for the worst case, not the nominal case: gnss_power_on()
     * schedules ~1733ms of delay (10+100+3+50+1000+320+250) plus real UART
     * TX time for its 34 gnss_tx() sends (25 wake + 8 config + 1 save), and
     * each of those 34 sends can independently block up to 100ms on its own
     * TX_DONE timeout before giving up (3400ms on top in the worst case).
     * 5500ms covers that with margin. */
    bool confirmed = xSemaphoreTake(stopped, pdMS_TO_TICKS(5500)) == pdTRUE;

    if (!confirmed) {
        ESP_LOGW(TAG, "gps_deinit: gnss task did not confirm exit in time, forcing delete");
        /* The task is genuinely still live here (it simply has not
         * confirmed within the budget), so it has to be excised from the
         * scheduler before the driver goes away: vTaskDelete() takes effect
         * immediately, even called from another task and before it returns,
         * so only past this call is it guaranteed the task can never issue
         * another nrfx_uarte_tx()/rx(). The reverse order would leave a
         * window where a still-live task's next UARTE call hits nrfx's
         * NRFX_ASSERT(state == INITIALIZED), and this build's assert
         * handler is __disable_irq(); for (;;) {}: a permanent hard lockup,
         * not a benign race. */
        vTaskDelete(task);
    }
    /* Safe in both branches now. Confirmed: gnss_task gave `stopped` and
     * from there only runs vTaskDelete(NULL), never touching the UARTE
     * driver again. Forced: the task is already gone. Either way no
     * nrfx_uarte_*() call can still be in flight or yet to come, and
     * uninit's own first act is NVIC_DisableIRQ on UARTE1
     * (nrfy_uarte_int_uninit), so uarte_handler() cannot run afterwards
     * either. */
    nrfx_uarte_uninit(&s_uarte);

    /* Only past both calls above is gnss_task guaranteed to be unable to
     * execute another instruction, so retiring `stopped` here cannot race a
     * genuine late-but-real xSemaphoreGive(s_stopped). */
    s_stopped = NULL;
    vSemaphoreDelete(stopped);

    nrf_gpio_pin_clear(BOARD_PIN_GNSS_EN);

    SemaphoreHandle_t mu = s_mu;
    if (mu != NULL) {
        /* Bounded, never portMAX_DELAY: the forced delete above can strand
         * s_mu, because FreeRTOS does not release a deleted task's mutexes
         * and gnss_task holds s_mu across gps_feed_bytes() in both its drain
         * loop and gnss_delay_draining(). An unbounded take would then hang
         * the caller forever. */
        bool locked = xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE;
        s_mu = NULL;
        gps_feed_reset(&s_feed);
        if (locked) {
            xSemaphoreGive(mu);
            vSemaphoreDelete(mu);
        } else {
            /* Deliberately leaked, not deleted. Failing the take means the
             * mutex is held by a task that no longer exists, and the query
             * functions block on it with portMAX_DELAY, so there may be
             * waiters parked on this queue right now. Freeing it would leave
             * them blocked on released memory and giving/taking a recycled
             * allocation later. Something has already gone wrong on this
             * path; stranding one mutex control block is strictly cheaper
             * than corrupting the heap. New callers are unaffected: s_mu is
             * NULL above, so every query now takes its null-guard. */
            ESP_LOGW(TAG, "gps_deinit: s_mu still held, leaking it rather than freeing waiters");
        }
    } else {
        gps_feed_reset(&s_feed);
    }
    if (s_tx_done) {
        vSemaphoreDelete(s_tx_done);
        s_tx_done = NULL;
    }
    if (s_stream) {
        vStreamBufferDelete(s_stream);
        s_stream = NULL;
    }
    s_powered = false;
    s_user_enabled = false;
}
