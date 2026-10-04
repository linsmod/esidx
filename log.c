#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static log_level_t g_level =
#ifdef ESIDX_DEBUG
    LOG_DEBUG;
#else
    LOG_WARN;
#endif

const char *log_level_name(log_level_t lvl)
{
    switch (lvl) {
    case LOG_OFF:   return "off";
    case LOG_ERROR: return "error";
    case LOG_WARN:  return "warn";
    case LOG_INFO:  return "info";
    case LOG_DEBUG: return "debug";
    case LOG_PERF:  return "perf";
    }
    return "?";
}

static int level_from_name(const char *s, log_level_t *out)
{
    static const struct { const char *n; log_level_t l; } tab[] = {
        { "off",   LOG_OFF   }, { "none",  LOG_OFF   },
        { "error", LOG_ERROR }, { "err",   LOG_ERROR },
        { "warn",  LOG_WARN  }, { "warning", LOG_WARN },
        { "info",  LOG_INFO  },
        { "debug", LOG_DEBUG }, { "trace", LOG_DEBUG },
        { "perf",  LOG_PERF  }, { "split", LOG_PERF  },
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (!strcasecmp(s, tab[i].n)) { *out = tab[i].l; return 0; }
    }
    /* bare number: 0..5 */
    if (s[0] >= '0' && s[0] <= '5' && s[1] == '\0') { *out = (log_level_t)(s[0] - '0'); return 0; }
    return -1;
}

/* drops the verbosity flags from argv so callers do not see them as operands */
static int is_verbose_flag(const char *a)
{
    return !strcmp(a, "-v") || !strncmp(a, "--verbose", 9);
}

/* "-v N": only swallow the next argv if it actually looks like a level, so
 * `esidx -v query db` does not read "query" as the level. */
static int looks_like_level(const char *s)
{
    if (!s || !*s) return 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    (void)v;
    return end && *end == '\0';
}

void log_init(int argc, char **argv)
{
    const char *env = getenv("ESIDX_LOG");
    log_level_t lvl;
    if (env && *env && !level_from_name(env, &lvl))
        g_level = lvl;

    for (int i = 1; i < argc; i++) {
        if (!is_verbose_flag(argv[i])) continue;
        int w;
        if (!strncmp(argv[i], "--verbose=", 10)) {
            w = atoi(argv[i] + 10);
        } else if (argv[i][2] == '\0' && i + 1 < argc && looks_like_level(argv[i + 1])) {
            w = atoi(argv[++i]);
        } else {
            w = LOG_DEBUG;
        }
        if (w < 0) w = 0;
        if (w > LOG_PERF) w = LOG_PERF;
        g_level = (log_level_t)w;
    }
}

int log_strip_flags(int argc, char **argv)
{
    int w = 0;
    for (int i = 0; i < argc; i++) {
        if (i > 0 && is_verbose_flag(argv[i])) {
            if (argv[i][2] == '\0' && i + 1 < argc && looks_like_level(argv[i + 1]))
                i++;
            continue;
        }
        argv[w++] = argv[i];
    }
    for (int i = w; i < argc; i++) argv[i] = NULL;
    return w;
}

log_level_t log_level(void) { return g_level; }

int log_enabled(log_level_t lvl) { return lvl <= g_level; }

void log_emit(log_level_t lvl, const char *file, int line, const char *fmt, ...)
{
    if (!log_enabled(lvl)) return;

    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;

    fprintf(stderr, "[%-5s] %s:%d: ", log_level_name(lvl), base, line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}