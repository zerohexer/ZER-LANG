/* Test helper: C library that calls a ZER callback from a pthread */
#include <pthread.h>
typedef void (*_test_cb_fn)(void);
static _test_cb_fn _test_cb = 0;
static void *_test_thread(void *arg) {
    (void)arg;
    if (_test_cb) _test_cb();
    return 0;
}
static void clib_set_cb(_test_cb_fn cb) { _test_cb = cb; }
static void clib_fire(void) {
    pthread_t t;
    pthread_create(&t, 0, _test_thread, 0);
    pthread_join(t, 0);
}
