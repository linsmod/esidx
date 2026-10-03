#ifndef ESIDX_LOG_H
#define ESIDX_LOG_H

/* Leveled logging. Default level is LOG_WARN (or LOG_DEBUG when built with
 * -DESIDX_DEBUG). Raise it at runtime without rebuilding:
 *
 *   ESIDX_LOG=debug ./esidx build /etc -o /tmp/etc.idx
 *   ESIDX_LOG=info  ./esidx query /tmp/etc.idx "ext:conf" "size:>1k"
 *
 * CLI: -v / -v N / --verbose=N  (N = 0..4). Log always goes to stderr so that
 * stdout stays a clean result stream.
 */

#include <stdarg.h>

typedef enum {
    LOG_OFF   = 0,
    LOG_ERROR = 1,
    LOG_WARN  = 2,
    LOG_INFO  = 3,
    LOG_DEBUG = 4
} log_level_t;

/* Parse -v/-v N/--verbose=N out of argv and read ESIDX_LOG. Call once from
 * main() before anything else; safe to call more than once. */
void log_init(int argc, char **argv);

/* Remove the verbosity flags (and a "-v N" level operand) from argv in place so
 * that main()/the subcommand parsers never see them. Returns the new argc;
 * argv[0] is left alone. Call after log_init. */
int log_strip_flags(int argc, char **argv);

log_level_t log_level(void);
const char *log_level_name(log_level_t lvl);
int         log_enabled(log_level_t lvl);

void log_emit(log_level_t lvl, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define LOGE(...) log_emit(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define LOGW(...) log_emit(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOGI(...) log_emit(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOGD(...) log_emit(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)

#endif /* ESIDX_LOG_H */