/* simple_check.c — minimal C function for VST verification demo.
 *
 * The function `is_alive` returns 1 iff the input state equals
 * HS_ALIVE (= 1). This mirrors a specific check from zercheck.c
 * but extracted to a standalone file for a first-pass verification.
 *
 * VST will prove this C implementation matches the Coq predicate
 * `is_alive(state) = (state =? 1)`.
 */

#define HS_ALIVE 1

int is_alive(int state) {
    if (state == HS_ALIVE) {
        return 1;
    }
    return 0;
}
