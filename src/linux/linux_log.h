/* The run's log: one numbered file per start in the log folder (trespasser_linux_NNN.log), the same lines on stderr.
 * Nothing is logged per frame. */
#ifndef JPT_LINUX_LOG_H
#define JPT_LINUX_LOG_H

/* Opens the next numbered log in dir (or exactly file when it is not NULL). Returns 0 when it cannot be created. */
int lx_log_open(const char* dir, const char* file);
const char* lx_log_path(void);
/* The log file descriptor, -1 without a log; the crash report writes to it. */
int lx_log_fd(void);
void lx_log(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void lx_log_close(void);

#endif
