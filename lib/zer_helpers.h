/* ZER stdlib C helpers — tiny wrappers for C macros that ZER can't access directly */
#ifndef ZER_HELPERS_H
#define ZER_HELPERS_H
#include <stdio.h>
static inline FILE *zer_get_stderr(void) { return stderr; }
static inline FILE *zer_get_stdout(void) { return stdout; }
#endif
