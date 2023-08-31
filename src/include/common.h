#ifndef dotk_common_h
#define dotk_common_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEBUG_PRINT_CODE 0
#define DEBUG_TRACE_EXEC 0

#define DEBUG_PRINT_VERBOSE_CUSTOM_OBJECTS 0

#define DEBUG_STRESS_GC 0
#define DEBUG_LOG_GC
#undef DEBUG_LOG_GC
#define UINT8_COUNT (UINT8_MAX + 1)
#define UINT16_COUNT (UINT16_MAX + 1)

#endif