#ifndef ULLLAS_RT_THREAD_H
#define ULLLAS_RT_THREAD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Raise the calling thread's scheduling priority for audio/network work.
 *
 * Best-effort: returns 0 on success, -1 on failure. We never abort - if
 * we can't promote the thread (no privileges, sandbox, etc.) we just
 * keep running at the default priority and log a warning once. The
 * audio driver's own callback already runs at RT priority; this raises
 * the user-space network thread that talks to the socket.
 */
int rt_raise_thread_priority(void);

/* Lock process pages in RAM so we don't get paged out mid-buffer.
 * Best-effort, no abort on failure. */
int rt_lock_memory(void);

/* One-shot logging helpers used by the modules above. */
void rt_log_warn_once(const char *msg);

#ifdef __cplusplus
}
#endif

#endif
