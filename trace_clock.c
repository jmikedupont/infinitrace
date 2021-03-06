/*
 * trace_clock.c
 *
 *  Created on: Oct 29, 2012
 *      Author: yitzikc
 */

#include <time.h>
#include <sys/time.h>
#include <assert.h>
#include "trace_clock.h"

#if defined(CLOCK_MONOTONIC) && defined(CLOCK_REALTIME)

static trace_ts_t trace_get_nsec_from_clk(clockid_t clk_id)
{
	struct timespec now;
	int rc = clock_gettime(clk_id, &now);
	if (0 != rc) {
		return TRACE_FOREVER;
	}

	return now.tv_nsec + TRACE_SECOND * now.tv_sec;
}

trace_ts_t trace_get_nsec(void)
{
     return trace_get_nsec_from_clk(CLOCK_REALTIME);
}

trace_ts_t trace_get_nsec_monotonic(void)
{
	return trace_get_nsec_from_clk(CLOCK_MONOTONIC);
}

#endif

trace_ts_t trace_get_walltime_ns(void)
{
    struct timeval tv;
    assert(0 == gettimeofday(&tv, NULL));

    return TRACE_US * ((((trace_ts_t)tv.tv_sec) * 1000000ULL) + tv.tv_usec);
}

unsigned long long trace_get_walltime_ms(void)
{
	return trace_get_walltime_ns() / TRACE_MS;
}

void trace_init_timespec(struct timespec *ts, trace_ts_t t)
{
    if (NULL != ts) {
        ts->tv_nsec = t % TRACE_SECOND;
        ts->tv_sec  = t / TRACE_SECOND;
    }
 }
