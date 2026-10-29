/**
 * @file no_malloc.h
 * @brief Compile-time guard that traps every dynamic allocation when
 *        TTAK_NO_MALLOC is defined.
 *
 * Defining TTAK_NO_MALLOC (and including this header, e.g. via
 * -include ttak/compat/no_malloc.h) redefines malloc/calloc/realloc/free so
 * that any call becomes a hard compile-time error on GCC/Clang (via the
 * error function attribute) or an unresolved external symbol on other
 * toolchains (link-time error).
 *
 * The library's own translation units additionally compile out their dynamic
 * constructors under TTAK_NO_MALLOC, so the zero-allocation modules can be
 * built with:
 *
 *   cc -DTTAK_NO_MALLOC -include ttak/compat/no_malloc.h ...
 *
 * and any accidental heap use fails the build instead of silently linking.
 * Use the ttak_<module>_init_static() APIs with caller-provided storage.
 */

#ifndef TTAK_COMPAT_NO_MALLOC_H
#define TTAK_COMPAT_NO_MALLOC_H

#if defined(TTAK_NO_MALLOC)

/*
 * Pull in the real allocator declarations first so the macros below do not
 * clash with prototypes in headers included later (include guards make the
 * second inclusion a no-op).
 */
#include <stdlib.h>

#if defined(__GNUC__)
/*
 * Returns void * (never defined) so that unrelated system-header expansions
 * still type-check; the error attribute turns any real call in the
 * translation unit into a hard compile-time error.
 */
__attribute__((error("dynamic allocation is disabled: build without -DTTAK_NO_MALLOC "
                     "or use the ttak_*_init_static APIs with caller-provided storage")))
void *ttak_no_malloc_trap(void);
#else
/* Intentionally undefined: using it fails at link time. */
void *ttak_no_malloc_trap(void);
#endif

#ifndef malloc
#define malloc(size)      ttak_no_malloc_trap()
#endif
#ifndef calloc
#define calloc(n, size)   ttak_no_malloc_trap()
#endif
#ifndef realloc
#define realloc(ptr, size) ttak_no_malloc_trap()
#endif
#ifndef free
#define free(ptr)         ttak_no_malloc_trap()
#endif

#endif /* TTAK_NO_MALLOC */

#endif /* TTAK_COMPAT_NO_MALLOC_H */
