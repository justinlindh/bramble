/*
 * Priority flattening for the IDF linux target: every FreeRTOS task the
 * firmware (or IDF itself) creates runs at ONE priority, so the scheduler's
 * round-robin time slicing always reschedules a preempted task.
 *
 * WHY (the freeze class this kills): on the linux port only one task pthread
 * executes at a time, and glibc's internal locks (stdio during a log write,
 * a malloc arena, any library mutex) are invisible to the FreeRTOS
 * scheduler. With UNEQUAL priorities this combination deadlocks the whole
 * node: a low-priority task gets preempted while holding such a lock, a
 * higher-priority task then blocks on the futex while FreeRTOS still
 * considers it the running/ready task, and strict priority scheduling never
 * runs the low-priority holder again. The node freezes silently: process
 * alive, every task stopped. Observed repeatedly on CI pods (mute nodes with
 * zero or one transmission, UI paint stopped mid-boot-burst) where load
 * jitter widens the preemption windows; the per-lock fixes (emu_link's
 * send-lock yield-wait) cannot cover glibc-internal locks. With EQUAL
 * priorities the preempted holder stays in the round-robin rotation, so the
 * worst case is a bounded stall instead of a permanent freeze.
 *
 * Real-time fidelity cost, STATED LOUDLY: a formerly high-priority task
 * (radio, mesh) can now wait up to a time slice behind any other ready
 * task, and, more importantly, THE EMULATOR CANNOT SURFACE GENUINE
 * PRIORITY-STARVATION BUGS in bramble's task design: a firmware change that
 * would starve a low-priority task on real hardware runs happily here,
 * because here every task shares one priority. That bug class is
 * hardware-only detection now (bench devices), by choice: the priority
 * semantics the port offered were already unfaithful, since the freeze
 * class above does not exist on target (no glibc locks there), so this
 * trades fake fidelity for guaranteed liveness. The same caveat lives in
 * emulator/README.md. The emulator already runs on a wall clock with
 * approximate timing and every scenario assertion is event-driven, so the
 * latency cost is sound here. Device builds never compile this file and
 * keep their true priorities. The longer-term alternative (FreeRTOS-aware
 * lock shims, or a port-level fix upstream) is recorded in the follow-up
 * issue for this bug family; flattening is a chosen tradeoff, not a hidden
 * one.
 *
 * Mechanism: the emulator link wraps the task-creation APIs (see
 * null_drivers/CMakeLists.txt's --wrap options) and clamps every requested
 * priority to EMU_FLAT_TASK_PRIO. The idle task (priority 0) keeps priority
 * 0: it never blocks, so putting it in the shared rotation would burn a full
 * slice per rotation doing nothing. vTaskPrioritySet is wrapped to a no-op
 * (nothing in the firmware depends on runtime priority changes for
 * correctness; honoring one would silently reintroduce the freeze).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define EMU_FLAT_TASK_PRIO 5

static UBaseType_t flatten(UBaseType_t prio) {
    return (prio == tskIDLE_PRIORITY) ? tskIDLE_PRIORITY : (UBaseType_t)EMU_FLAT_TASK_PRIO;
}

/* __real_ prototypes resolved by the linker's --wrap machinery. */
BaseType_t __real_xTaskCreate(TaskFunction_t fn, const char* name, configSTACK_DEPTH_TYPE stack,
                              void* arg, UBaseType_t prio, TaskHandle_t* out);
BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack,
                                          void* arg, UBaseType_t prio, TaskHandle_t* out,
                                          BaseType_t core);
TaskHandle_t __real_xTaskCreateStatic(TaskFunction_t fn, const char* name, uint32_t stack,
                                      void* arg, UBaseType_t prio, StackType_t* stack_buf,
                                      StaticTask_t* task_buf);
TaskHandle_t __real_xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char* name,
                                                  uint32_t stack, void* arg, UBaseType_t prio,
                                                  StackType_t* stack_buf, StaticTask_t* task_buf,
                                                  BaseType_t core);

BaseType_t __wrap_xTaskCreate(TaskFunction_t fn, const char* name, configSTACK_DEPTH_TYPE stack,
                              void* arg, UBaseType_t prio, TaskHandle_t* out) {
    return __real_xTaskCreate(fn, name, stack, arg, flatten(prio), out);
}

BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack,
                                          void* arg, UBaseType_t prio, TaskHandle_t* out,
                                          BaseType_t core) {
    return __real_xTaskCreatePinnedToCore(fn, name, stack, arg, flatten(prio), out, core);
}

TaskHandle_t __wrap_xTaskCreateStatic(TaskFunction_t fn, const char* name, uint32_t stack,
                                      void* arg, UBaseType_t prio, StackType_t* stack_buf,
                                      StaticTask_t* task_buf) {
    return __real_xTaskCreateStatic(fn, name, stack, arg, flatten(prio), stack_buf, task_buf);
}

TaskHandle_t __wrap_xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char* name,
                                                  uint32_t stack, void* arg, UBaseType_t prio,
                                                  StackType_t* stack_buf, StaticTask_t* task_buf,
                                                  BaseType_t core) {
    return __real_xTaskCreateStaticPinnedToCore(fn, name, stack, arg, flatten(prio), stack_buf,
                                                task_buf, core);
}

void __wrap_vTaskPrioritySet(TaskHandle_t task, UBaseType_t prio) {
    (void)task;
    (void)prio; /* flattened world: runtime priority changes are ignored */
}
