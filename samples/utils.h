#ifndef _UTILS_H_
#define _UTILS_H_

/*
 * utils.h -- small shared helpers used by several of the interceptor
 * samples: process-priority tweaks, screen metrics, a CPU-spin delay
 * primitive plus its runtime calibration, and a single-instance-per-machine
 * guard built on a named kernel mutex.
 *
 * Plain C, no dependency beyond the Win32 API and the CRT's <time.h>/
 * <string.h> (used by the corresponding .c file; this header itself needs
 * nothing beyond ordinary C).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Raises/lowers the current process's scheduling priority class. Samples
   that spin waiting on input strokes raise it while doing latency-sensitive
   work and lower it again around anything CPU-heavy (e.g. mathpointer's
   curve tracing) so they don't starve the rest of the system. */
void raise_process_priority(void);
void lower_process_priority(void);

/* Primary screen dimensions, in pixels. */
int get_screen_width(void);
int get_screen_height(void);

/* Pure CPU-spin delay: decrements count to zero and returns. Used as the
   building block for short, precise-enough delays that don't want to pay
   for a kernel wait (e.g. the pen-lift pacing in mathpointer). */
void busy_wait(unsigned long count);

/* Empirically measures roughly how many busy_wait() decrements correspond
   to one millisecond of wall-clock time on this machine. Call once and
   reuse the result for the life of the program. */
unsigned long calculate_busy_wait_millisecond(void);

/* Enforces "only one copy of this program running on this machine at a
   time" via a named, session-independent kernel mutex derived from `name`.
   Returns NULL if another instance already owns the identity (or creation
   otherwise failed) -- the caller should not proceed. Otherwise returns a
   handle to hold for the program's lifetime and hand to
   close_single_program() at exit. */
void *try_open_single_program(const char *name);
void close_single_program(void *program_instance);

#ifdef __cplusplus
}
#endif

#endif /* _UTILS_H_ */
