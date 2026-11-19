#ifndef TRESPASSER_AUDIO_LOG_H
#define TRESPASSER_AUDIO_LOG_H

/* The host's log (log.c); tests provide their own. */
void host_log(const char* tag, const char* fmt, ...);
void host_log_perf(const char* tag, const char* fmt, ...);

#endif
