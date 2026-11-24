/* The run's log: one numbered file per start in the log folder (trespasser_recomp_NNN.log), the same lines on stderr.
 * Nothing is logged per frame. */
#ifndef JPT_LINUX_LOG_H
#define JPT_LINUX_LOG_H

/* Opens the next numbered log in dir (or appends to file when it is not NULL). Returns 0 when it cannot be created. */
int lx_log_open(const char* dir, const char* file);
int lx_log_open_from(const char* dir, unsigned first_number);
const char* lx_log_path(void);
/* The log file descriptor, -1 without a log; the crash report writes to it. */
int lx_log_fd(void);
void lx_log(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void lx_log_close(void);
/* Drops the lines instead of printing them (the headless self-check keeps its output to the JSON). */
void lx_log_quiet(int quiet);

#endif
