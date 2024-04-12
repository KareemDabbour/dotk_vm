#ifndef dotk_common_h
#define dotk_common_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define DEBUG_PRINT_CODE 0
#define DEBUG_TRACE_EXEC 0

#define PRINT_VERBOSE_OBJECTS_DEPTH 6

#define DEBUG_PRINT_VERBOSE_CUSTOM_OBJECTS 1

#define DEBUG_STRESS_GC 0
#define DEBUG_LOG_GC
#undef DEBUG_LOG_GC
#define UINT4_MAX 15
#define UINT4_COUNT (UINT4_MAX + 1)
#define UINT8_COUNT (UINT8_MAX + 1)
#define UINT16_COUNT (UINT16_MAX + 1)
#define LOCALS_MAX UINT8_COUNT * 4

#ifdef _WIN32
#include <Windows.h>
#include <conio.h>
#else
#define clrscr() printf("\e[1;1H\e[2J")
#endif

#if defined(__GNUC__) || defined(__clang__)
#define unlikely(x) __builtin_expect((x), 0)
#define likely(x)   __builtin_expect((x), 1)
#endif

#endif