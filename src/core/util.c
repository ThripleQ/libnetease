#include "netease/util.h"
#include <time.h>

int64_t ne_now_ms(void) {
    struct timespec ts;
#if defined(_WIN32)
    /* WallClock as ms since epoch */
    return (int64_t)time(NULL) * 1000;
#else
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}
int64_t ne_now_unix(void) { return (int64_t)time(NULL); }
