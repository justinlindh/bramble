// nrfx integration glue for the Bramble bare-metal nRF52840 target.
// Implements the macro set from nrfx templates/nrfx_glue.h on CMSIS
// intrinsics and GCC atomic builtins. FreeRTOS constraints: peripheral IRQ
// priorities must stay numerically >= configMAX_SYSCALL_INTERRUPT_PRIORITY
// (set per driver at init time, not here).
#ifndef NRFX_GLUE_H__
#define NRFX_GLUE_H__

#ifdef __cplusplus
extern "C" {
#endif

// Bind peripheral IRQ handlers to the MDK startup vector names at link time.
#include <soc/nrfx_irqs.h>

#define NRFX_ASSERT(expression)                                                \
    do {                                                                       \
        if (!(expression)) {                                                   \
            __disable_irq();                                                   \
            for (;;) {                                                         \
            }                                                                  \
        }                                                                      \
    } while (0)

#define NRFX_STATIC_ASSERT(expression) _Static_assert(expression, "nrfx static assert")

#define NRFX_IRQ_PRIORITY_SET(irq_number, priority)                            \
    NVIC_SetPriority((IRQn_Type)(irq_number), (priority))
#define NRFX_IRQ_ENABLE(irq_number) NVIC_EnableIRQ((IRQn_Type)(irq_number))
#define NRFX_IRQ_IS_ENABLED(irq_number) (0 != NVIC_GetEnableIRQ((IRQn_Type)(irq_number)))
#define NRFX_IRQ_DISABLE(irq_number) NVIC_DisableIRQ((IRQn_Type)(irq_number))
#define NRFX_IRQ_PENDING_SET(irq_number) NVIC_SetPendingIRQ((IRQn_Type)(irq_number))
#define NRFX_IRQ_PENDING_CLEAR(irq_number) NVIC_ClearPendingIRQ((IRQn_Type)(irq_number))
#define NRFX_IRQ_IS_PENDING(irq_number) (0 != NVIC_GetPendingIRQ((IRQn_Type)(irq_number)))

// PRIMASK save/restore pair; the unbalanced braces are closed by EXIT, which
// is how the nrfx glue contract expects scoped critical sections to work.
#define NRFX_CRITICAL_SECTION_ENTER()                                          \
    {                                                                          \
        uint32_t nrfx_crit_primask = __get_PRIMASK();                          \
        __disable_irq();
#define NRFX_CRITICAL_SECTION_EXIT()                                           \
        __set_PRIMASK(nrfx_crit_primask);                                      \
    }

#define NRFX_DELAY_DWT_BASED 0
#include <soc/nrfx_coredep.h>
#define NRFX_DELAY_US(us_time) nrfx_coredep_delay_us(us_time)

#define nrfx_atomic_t uint32_t
#define NRFX_ATOMIC_FETCH_STORE(p_data, value) __atomic_exchange_n(p_data, value, __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_OR(p_data, value) __atomic_fetch_or(p_data, value, __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_AND(p_data, value) __atomic_fetch_and(p_data, value, __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_XOR(p_data, value) __atomic_fetch_xor(p_data, value, __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_ADD(p_data, value) __atomic_fetch_add(p_data, value, __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_SUB(p_data, value) __atomic_fetch_sub(p_data, value, __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_CAS(p_data, old_value, new_value)                          \
    __extension__({                                                            \
        typeof(*(p_data)) nrfx_cas_expected = (old_value);                     \
        __atomic_compare_exchange_n(p_data, &nrfx_cas_expected, (new_value),   \
                                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
    })
#define NRFX_CLZ(value) __builtin_clz(value)
#define NRFX_CTZ(value) __builtin_ctz(value)

#define NRFX_CUSTOM_ERROR_CODES 0
#define NRFX_EVENT_READBACK_ENABLED 1

// No cache on nRF52840
#define NRFY_CACHE_WB(p_buffer, size)
#define NRFY_CACHE_INV(p_buffer, size)
#define NRFY_CACHE_WBINV(p_buffer, size)

#define NRFX_DPPI_CHANNELS_USED 0
#define NRFX_DPPI_GROUPS_USED 0
#define NRFX_PPI_CHANNELS_USED 0
#define NRFX_PPI_GROUPS_USED 0
#define NRFX_GPIOTE_CHANNELS_USED 0
#define NRFX_EGUS_USED 0
#define NRFX_TIMERS_USED 0

#ifdef __cplusplus
}
#endif

#endif // NRFX_GLUE_H__
