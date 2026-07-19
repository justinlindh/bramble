#ifndef FREERTOS_FREERTOS_H_STUB
#define FREERTOS_FREERTOS_H_STUB
#define pdMS_TO_TICKS(x) (x)
#ifndef pdPASS
#define pdPASS 1
#endif
#ifndef pdFAIL
#define pdFAIL 0
#endif

/* Spinlock shim. Host tests are single-threaded, so the critical sections
 * are no-ops; they exist only so code that guards shared state with
 * portMUX compiles unchanged. */
#ifndef portMUX_TYPE
#define portMUX_TYPE int
#endif
#ifndef portMUX_INITIALIZER_UNLOCKED
#define portMUX_INITIALIZER_UNLOCKED 0
#endif
#ifndef taskENTER_CRITICAL
#define taskENTER_CRITICAL(mux) ((void)(mux))
#endif
#ifndef taskEXIT_CRITICAL
#define taskEXIT_CRITICAL(mux) ((void)(mux))
#endif
#endif
