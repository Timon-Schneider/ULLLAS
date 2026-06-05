#include "rt_thread.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#else
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <errno.h>
#endif

void rt_log_warn_once(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

int rt_raise_thread_priority(void) {
#ifdef _WIN32
    /* TIME_CRITICAL is the highest non-realtime; doesn't require admin.
     * REALTIME_PRIORITY_CLASS requires SE_INC_BASE_PRIORITY_NAME and
     * can lock up the system if abused, so we don't go that far. */
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        rt_log_warn_once("rt: SetThreadPriority(TIME_CRITICAL) failed (continuing at default)");
        return -1;
    }
    return 0;
#elif defined(__APPLE__)
    /* THREAD_TIME_CONSTRAINT_POLICY is the real-time class. We size the
     * period for a typical 128-sample buffer at 48 kHz (~2.67 ms) - it
     * just needs to be in the right ballpark, the kernel scales. */
    struct thread_time_constraint_policy policy;
    mach_timebase_info_data_t            tb;
    if (mach_timebase_info(&tb) != KERN_SUCCESS) {
        rt_log_warn_once("rt: mach_timebase_info failed");
        return -1;
    }
    /* ns -> abs */
    double ns_to_abs   = (double)tb.denom / (double)tb.numer;
    policy.period      = (uint32_t)(2700000.0 * ns_to_abs); /* 2.7ms */
    policy.computation = (uint32_t)(500000.0 * ns_to_abs);  /* 500us */
    policy.constraint  = (uint32_t)(2700000.0 * ns_to_abs);
    policy.preemptible = 1;
    if (thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy,
                          THREAD_TIME_CONSTRAINT_POLICY_COUNT) != KERN_SUCCESS) {
        rt_log_warn_once("rt: thread_policy_set failed (continuing at default)");
        return -1;
    }
    return 0;
#else
    /* Try SCHED_FIFO at moderate priority. Without CAP_SYS_NICE / rtprio
     * rlimit this returns EPERM - we just warn and continue. */
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    int policy   = SCHED_FIFO;
    int max_prio = sched_get_priority_max(policy);
    int min_prio = sched_get_priority_min(policy);
    int target   = min_prio + (max_prio - min_prio) * 3 / 4;
    param.sched_priority = target;
    if (pthread_setschedparam(pthread_self(), policy, &param) != 0) {
        rt_log_warn_once("rt: pthread_setschedparam(SCHED_FIFO) failed (try `ulimit -r 95` or run as root)");
        return -1;
    }
    return 0;
#endif
}

int rt_lock_memory(void) {
#ifdef _WIN32
    /* Best-effort: raise our minimum working set so the OS keeps pages
     * resident under normal load. Doesn't truly pin like mlockall but
     * doesn't require SeLockMemoryPrivilege either. */
    SIZE_T min_ws = 0, max_ws = 0;
    if (!GetProcessWorkingSetSize(GetCurrentProcess(), &min_ws, &max_ws)) {
        return -1;
    }
    SIZE_T new_min = 64 * 1024 * 1024; /* 64 MiB - far above what we use */
    SIZE_T new_max = max_ws < new_min * 2 ? new_min * 2 : max_ws;
    if (!SetProcessWorkingSetSizeEx(GetCurrentProcess(), new_min, new_max, QUOTA_LIMITS_HARDWS_MIN_ENABLE)) {
        rt_log_warn_once("rt: SetProcessWorkingSetSizeEx failed (continuing)");
        return -1;
    }
    return 0;
#elif defined(__APPLE__)
    /* macOS has no mlockall. THREAD_TIME_CONSTRAINT_POLICY (set above)
     * already prevents the kernel from paging out real-time threads. */
    (void)0;
    return 0;
#else
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        rt_log_warn_once("rt: mlockall failed (try ulimit -l unlimited or set RLIMIT_MEMLOCK)");
        return -1;
    }
    return 0;
#endif
}
