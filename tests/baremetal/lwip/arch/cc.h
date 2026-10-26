/* lwIP compiler/platform glue for arm-none-eabi with newlib. */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

/* newlib supplies struct timeval and errno; lwIP must report errors with
 * newlib's codes so EAGAIN and friends compare equal in libttak. */
#define LWIP_TIMEVAL_PRIVATE 0
#define LWIP_ERRNO_STDINCLUDE 1

#define LWIP_RAND() ((u32_t)rand())

#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    printf("[lwIP] assert \"%s\" at %s:%d\n", x, __FILE__, __LINE__); \
    abort(); \
} while (0)

#endif /* LWIP_ARCH_CC_H */
