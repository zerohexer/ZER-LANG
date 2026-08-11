/* Test helper for BUG-787. The bodyless-extern destructor heuristic has TWO
 * arms and the .zer exercises both for real (they link and run):
 *   - a VOID-returning extern taking *T  -> dev_close
 *   - a NON-VOID one matched by NAME     -> res_release (returns int)
 * Plain `unsigned char *` because the ZER side declares `*u8`: `*opaque` emits
 * as the fat `_zer_opaque` struct, which a hand-written prototype cannot spell. */
#include <stdlib.h>
static unsigned char *dev_open(void)                { return (unsigned char *)calloc(1, 16); }
static void           dev_close(unsigned char *d)   { free(d); }
static unsigned       dev_read(unsigned char *d)    { return d ? 0u : 1u; }
static unsigned char *res_acquire(void)             { return (unsigned char *)calloc(1, 8); }
static int            res_release(unsigned char *p) { free(p); return 0; }
