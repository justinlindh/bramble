// IDF-style include path wrapper; the kernel header is the real thing.
#pragma once
#include <task.h>

// IDF SMP helper used in log lines; this target is single-core.
#ifndef xPortGetCoreID
#define xPortGetCoreID() 0
#endif
