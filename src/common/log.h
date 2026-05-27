#ifndef LOOPY_COMMON_LOG_H
#define LOOPY_COMMON_LOG_H

#include <stdio.h>

#ifdef LOOPY_DEBUG_LOGGING
#define LOOPY_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
#define LOOPY_DEBUG_PRINTF(...) do { if (0) printf(__VA_ARGS__); } while (0)
#endif

#endif
