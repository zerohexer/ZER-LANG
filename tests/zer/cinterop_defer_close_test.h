/* Test helper for cinterop_defer_close_ok.zer — a minimal C "device" library
 * with the open/read/close shape ZER's cinclude interop targets.
 *
 * Two things worth knowing if you copy this pattern:
 *   - `struct Dev` is DECLARED IN ZER and emitted before this include, so the
 *     header must use the tag, never redefine it.
 *   - a concrete `*Dev` is used rather than `*opaque`, because `*opaque` lowers
 *     to the tracked `_zer_opaque` wrapper struct, which hand-written C cannot
 *     spell. */
#include <stdlib.h>
static int _dev_open_count = 0;
static int _dev_close_count = 0;
static struct Dev *dev_open(void) {
    struct Dev *h = (struct Dev *)malloc(sizeof(struct Dev));
    if (h) { h->v = 7u; _dev_open_count++; }
    return h;
}
static unsigned dev_read(struct Dev *d) { return d ? d->v : 0u; }
static void dev_close(struct Dev *d) { if (d) { _dev_close_count++; free(d); } }
static unsigned dev_balanced(void) {
    return (_dev_open_count == _dev_close_count) ? 1u : 0u;
}
