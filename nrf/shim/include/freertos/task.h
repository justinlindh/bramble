// IDF-style include path wrapper presenting ESP-IDF task-API semantics on
// the upstream kernel, for the shared main/ and components/ code that
// expects them. nrf-native code includes <task.h> bare and is unaffected.
#pragma once
#include <task.h>

// IDF SMP helper used in log lines; this target is single-core.
#ifndef xPortGetCoreID
#define xPortGetCoreID() 0
#endif

// ESP-IDF xTaskCreate takes the stack size in BYTES; the upstream kernel
// takes WORDS. Shared code passes IDF byte constants, so convert here. A
// function-like macro invoking the real function of the same name does not
// recurse (the name is painted blue during its own expansion).
#define xTaskCreate(fn, name, stack_bytes, arg, prio, handle)                                      \
    xTaskCreate(fn, name, (stack_bytes) / sizeof(StackType_t), arg, prio, handle)

// Single core: the affinity argument drops; stack conversion applies via the
// xTaskCreate macro above.
#define xTaskCreatePinnedToCore(fn, name, stack_bytes, arg, prio, handle, core)                    \
    xTaskCreate(fn, name, stack_bytes, arg, prio, handle)
