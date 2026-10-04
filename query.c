/* Query execution (design §6.3).
 *
 * The pipeline, in order:
 *
 *   1. driver selection   pick the cheapest index-backed leaf (§6.2)
 *   2. candidate bitmap   seed from that one index
 *   3. leaf intersection  every other leaf filters the *candidate set*, never
 *                         the whole table -- this is the whole point of step 1
 *   4. FILTER_* re-match  the independent second matcher layer (§1.1 note, F8)
 *   5. sort               multi-level chain, total order guaranteed
 *   6. slice              OFFSET / COUNT
 *
 * Step 1 is what turns the old fixed-order scan into something interactive. A
 * browse request is `parent:"/x" folder: name_ascending count:200`; with
 * `parent:` as the driver the matcher pass walks a directory listing, not the
 * index. The alternative -- seeding from `folder:` and then discovering that
 * `parent:` has to test every directory in the tree -- is what the previous
 * implementation did, and it is why an unfiltered query on /usr spent 42 of its
 * 46 ms in qsort (design §10).
 *
 * Every leaf is a single function with a uniform signature, so adding a function
 * is one table row. That is the FSearch lesson (ref D1) applied to the index
 * side rather than to a scan.
 */

#include "syntax.h"
#include "timer.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>

/* ------------------------------------------------------------ match options */

mod_t match_opts_mods(const match_opts_t *mo)
{
    mod_t m = 0;
    if (!mo) return m;
    if (mo->match_case)          m |= MOD_CASE;
    if (mo->match_whole_word)    m |= MOD_WW;
    if (mo->match_path)          m |= MOD_PATH;
    if (mo->match_diacritics)    m |= MOD_DIACRITICS;
    if (mo->match_prefix)        m |= MOD_PREFIX;
    if (mo->match_suffix)        m |= MOD_SUFFIX;
    if (mo->match_regex)         m |= MOD_REGEX;
    if (mo->ignore_punctuation)  m |= MOD_IGNOREPUNC;
    if (mo->ignore_whitespace)   m |= MOD_IGNOREWS;
    return m;
}

/* ---------------------------------------------------------- sort key names */

/* The 22 names of etp_server.c:474-493. `inverse_size` reuses the size key with
 * the direction flipped (design §1.2). */
static const struct { const char *name; uint16_t key; int asc; } sort_names[] = {
    { "name_ascending",               SORT_NAME,   1 },
    { "name_descending",              SORT_NAME,   0 },
    { "path_ascending",               SORT_PATH,   1 },
    { "path_descending",              SORT_PATH,   0 },
    { "size_ascending",               SORT_SIZE,   1 },
    { "size_descending",              SORT_SIZE,   0 },
    { "inverse_size_ascending",       SORT_SIZE | SORT_INVERSE_SIZE, 0 },
    { "inverse_size_descending",      SORT_SIZE | SORT_INVERSE_SIZE, 1 },
    { "extension_ascending",          SORT_EXT,    1 },
    { "extension_descending",         SORT_EXT,    0 },
    { "date_created_ascending",       SORT_CTIME,  1 },
    { "date_created_descending",      SORT_CTIME,  0 },
    { "date_modified_ascending",      SORT_MTIME,  1 },
    { "date_modified_descending",     SORT_MTIME,  0 },
    { "attributes_ascending",         SORT_ATTRIBUTES, 1 },
    { "attributes_descending",        SORT_ATTRIBUTES, 0 },
    { "file_list_filename_ascending", SORT_FILE_LIST_FILENAME, 1 },
    { "file_list_filename_descending",SORT_FILE_LIST_FILENAME, 0 },
    { "date_recently_changed_ascending",  SORT_RECENTLY_CHANGED, 1 },
    { "date_recently_changed_descending", SORT_RECENTLY_CHANGED, 0 },
};

int sort_from_etp_name(const char *name, uint16_t *out_key, int *out_asc)
{
    if (!name) return -1;
    for (size_t i = 0; i < sizeof(sort_names) / sizeof(sort_names[0]); i++) {
        if (!strcasecmp(name, sort_names[i].name)) {
            *out_key = sort_names[i].key;
            *out_asc = sort_names[i].asc;
            return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------ value parsing */

/* Sizes: kb/mb/gb are decimal (L133-146 says so explicitly), bare k/m/g/t are
 * the 1024-based forms Everything also accepts. */
/* Sizes: kb/mb/gb are decimal (L133-146 says so explicitly), bare k/m/g/t are
 * the 1024-based forms Everything also accepts.
 *
 * The span out-parameter exists so sizes and dates can share parse_range2(); a
 * size is always a point, so it is always left at 0. */
static int64_t parse_size_value(const char *s, int *ok, int64_t *span)
{
    if (span) *span = 0;
    *ok = 0;
    while (*s == ' ') s++;
    if (!*s) return 0;

    static const struct { const char *n; int64_t v; } consts[] = {
        { "empty", 0 }, { "tiny", 10 * 1024 }, { "small", 100 * 1024 },
        { "medium", 1024 * 1024 }, { "large", 16 * 1024 * 1024 },
        { "huge", 128 * 1024 * 1024 }, { "gigantic", 128 * 1024 * 1024 },
        { "unknown", 0 },
    };
    for (size_t i = 0; i < sizeof(consts) / sizeof(consts[0]); i++) {
        if (!strcasecmp(s, consts[i].n)) { *ok = 1; return consts[i].v; }
    }

    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return 0;
    while (*end == ' ') end++;
    int64_t mult = 1;
    if (!strcasecmp(end, "tb")) mult = 1000000000000LL;
    else if (!strcasecmp(end, "gb")) mult = 1000000000LL;
    else if (!strcasecmp(end, "mb")) mult = 1000000LL;
    else if (!strcasecmp(end, "kb")) mult = 1000LL;
    else if (!strcasecmp(end, "t")) mult = 1024LL * 1024 * 1024 * 1024;
    else if (!strcasecmp(end, "g")) mult = 1024LL * 1024 * 1024;
    else if (!strcasecmp(end, "m")) mult = 1024LL * 1024;
    else if (!strcasecmp(end, "k")) mult = 1024LL;
    else if (*end == '\0') mult = 1;
    else return 0;
    *ok = 1;
    return (int64_t)(v * (double)mult);
}

/* Dates and durations (L148-189).
 *
 * Two shapes come back, and conflating them is a silent-wrong-answer bug:
 *
 *   a POINT   -- an instant: `dm:2024-03-05T10:30:00`, a raw epoch, a FILETIME
 *   a PERIOD  -- a span:    `today`, `yesterday`, a bare year, `YYYY-MM`
 *
 * `dm:today` has to match everything modified since midnight, not only what was
 * modified at exactly midnight. A point parser answers zero rows and looks like a
 * correct empty result. `period` is set when the text names a span.
 */
typedef struct {
    int64_t at;
    int64_t span;    /* seconds, 0 for a point */
    int     period;
} tval_t;

static int64_t day_start(int back)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm) - (int64_t)back * 86400;
}

/* midnight of the first day of the month `mo` months ago (negative = future) */
static int64_t month_start(int back)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_mday = 1;
    tm.tm_mon -= back;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

static tval_t parse_time_value(const char *s)
{
    tval_t r = {0, 0, 0};
    while (*s == ' ') s++;
    if (!*s) return r;

    /* ---- periods ---- */
    if (!strcasecmp(s, "today"))     { r.at = day_start(0);  r.span = 86400; r.period = 1; return r; }
    if (!strcasecmp(s, "yesterday")) { r.at = day_start(1);  r.span = 86400; r.period = 1; return r; }
    if (!strcasecmp(s, "tomorrow"))  { r.at = day_start(-1); r.span = 86400; r.period = 1; return r; }
    if (!strcasecmp(s, "mtd") || !strcasecmp(s, "thismonth")) {
        r.at = month_start(0); r.span = 31 * 86400; r.period = 1; return r;
    }
    if (!strcasecmp(s, "lastmonth")) {
        r.at = month_start(1); r.span = 31 * 86400; r.period = 1; return r;
    }
    if (!strcasecmp(s, "ytd") || !strcasecmp(s, "thisyear")) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        tm.tm_mon = 0;
        tm.tm_mday = 1;
        tm.tm_isdst = -1;
        r.at = (int64_t)mktime(&tm);
        r.span = 366 * 86400;
        r.period = 1;
        return r;
    }

    /* <n><unit> relative to now: 7d, 3hours, 45mins. A point, not a period:
     * `dm:>7d` is "newer than seven days ago", and the comparison supplies the
     * other end. */
    {
        char *end = NULL;
        long n = strtol(s, &end, 10);
        if (end != s && *end) {
            int64_t unit = 0;
            if      (!strncasecmp(end, "day", 3))    unit = 86400;
            else if (!strncasecmp(end, "hour", 4))   unit = 3600;
            else if (!strncasecmp(end, "week", 4))   unit = 7 * 86400;
            else if (!strncasecmp(end, "month", 5))  unit = 30 * 86400;
            else if (!strncasecmp(end, "year", 4))   unit = 365 * 86400;
            else if (!strncasecmp(end, "min", 3))    unit = 60;
            else if (!strncasecmp(end, "sec", 3))    unit = 1;
            else if (*end == 'd' && end[1] == '\0') unit = 86400;
            else if (*end == 'h' && end[1] == '\0') unit = 3600;
            else if (*end == 'w' && end[1] == '\0') unit = 7 * 86400;
            else if (*end == 'm' && end[1] == '\0') unit = 60;
            else if (*end == 's' && end[1] == '\0') unit = 1;
            if (unit) { r.at = (int64_t)time(NULL) - (int64_t)n * unit; return r; }
        }
    }

    /* A raw FILETIME. L209 says ">99999999" distinguishes it from a year, but
     * that threshold also swallows any large epoch -- `dm:>99999999999` would
     * then become a date in 1601 and match everything. A FILETIME is 100 ns
     * units, so the smallest plausible one is 1970 in those units. */
    if (strlen(s) > 9) {
        char *end = NULL;
        long long ft = strtoll(s, &end, 10);
        if (end != s && *end == '\0' && ft >= 116444736000000000LL) {
            r.at = (int64_t)((ft / 10000000LL) - 11644473600LL);
            return r;
        }
    }

    /* YYYY[-MM[-DD[Thh[:mm[:ss]]]]] and YYYYMMDD; a bare year or YYYY/MM is a
     * whole-year or whole-month period */
    {
        int y = 0, mo = 1, d = 1, h = 0, mi = 0, se = 0;
        int n = 0;
        if (sscanf(s, "%d-%d-%dT%d:%d:%d%n", &y, &mo, &d, &h, &mi, &se, &n) >= 4 ||
            sscanf(s, "%d-%d-%d%n", &y, &mo, &d, &n) >= 3) {
            struct tm tm;
            memset(&tm, 0, sizeof(tm));
            tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
            tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se;
            tm.tm_isdst = -1;
            time_t t = mktime(&tm);
            if (t != (time_t)-1) { r.at = (int64_t)t; return r; }
        }
        n = 0;
        if (sscanf(s, "%d%2d%2d%2d%2d%2d%n", &y, &mo, &d, &h, &mi, &se, &n) >= 3 ||
            sscanf(s, "%d%2d%2d%n", &y, &mo, &d, &n) >= 3) {
            struct tm tm;
            memset(&tm, 0, sizeof(tm));
            tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
            tm.tm_isdst = -1;
            time_t t = mktime(&tm);
            if (t != (time_t)-1) { r.at = (int64_t)t; return r; }
        }
        n = 0;
        if (sscanf(s, "%d/%d%n", &y, &mo, &n) == 2 && n == (int)strlen(s) &&
            y >= 1000 && y <= 9999) {
            struct tm tm;
            memset(&tm, 0, sizeof(tm));
            tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = 1;
            tm.tm_isdst = -1;
            r.at = (int64_t)mktime(&tm);
            r.span = (mo == 2 && (y % 4 == 0 && (y % 100 || y % 400 == 0))) ? 29 * 86400
                                                                            : 31 * 86400;
            r.period = 1;
            return r;
        }
        /* A bare four-digit number is a year, not an epoch (L204). Reading
         * `dm:2022` as the epoch second 2022 would silently return nothing. */
        if (sscanf(s, "%d%n", &y, &n) == 1 && n == (int)strlen(s) &&
            y >= 1970 && y <= 9999) {
            struct tm tm;
            memset(&tm, 0, sizeof(tm));
            tm.tm_year = y - 1900; tm.tm_mon = 0; tm.tm_mday = 1;
            tm.tm_isdst = -1;
            r.at = (int64_t)mktime(&tm);
            r.span = (y % 4 == 0 && (y % 100 || y % 400 == 0)) ? 366 * 86400 : 365 * 86400;
            r.period = 1;
            return r;
        }
    }

    /* small integers are raw epochs, which is what the CLI accepted before */
    {
        char *end = NULL;
        long long v = strtoll(s, &end, 10);
        if (end != s && *end == '\0') { r.at = (int64_t)v; return r; }
    }
    return r;
}

/* A `start..end` range (L159), optionally also the `start-end` spelling (L160).
 *
 * The single dash is only honoured where it cannot be confused with something
 * else, and that decision is the caller's: for `size:` a dash can only be a range
 * separator, for a date it is nearly always part of `YYYY-MM-DD`. Reading
 * `dm:2000-01-01` as the range 2000..01-01 yields an empty interval and a silently
 * wrong answer, so the date parser refuses the dash form and users write
 * `dm:2021-07..2022-06`, which is what Everything's own examples do.
 *
 * A single date bound that names a PERIOD widens to that whole period, so
 * `dm:today` means "since midnight", not "at midnight". */
static void parse_range2(const char *v, cmp_op_t cmp,
                         int64_t *lo, int64_t *hi,
                         int64_t (*pv)(const char *, int *, int64_t *),
                         int *ok, int allow_dash)
{
    *ok = 1;
    char a[128], b[128];
    const char *dots = strstr(v, "..");
    const char *dash = (dots || !allow_dash) ? NULL : strchr(v + 1, '-');

    if (dots || dash) {
        const char *sep = dots ? dots : dash;
        size_t la = (size_t)(sep - v);
        if (la >= sizeof(a)) la = sizeof(a) - 1;
        memcpy(a, v, la); a[la] = '\0';
        snprintf(b, sizeof(b), "%s", sep + (dots ? 2 : 1));
        *lo = pv(a, ok, NULL); if (!*ok) { *lo = 0; *ok = 0; return; }
        *hi = pv(b, ok, NULL); if (!*ok) { *hi = 0; *ok = 0; return; }
        return;
    }

    /* widen a period, then apply the comparison against the widened form */
    int64_t span = 0;
    *lo = pv(v, ok, &span);
    if (!*ok) { *lo = 0; *ok = 0; return; }

    if (span > 0) {
        switch (cmp) {
        case CMP_LT: *hi = *lo - 1; *lo = INT64_MIN; break;
        case CMP_LE: *hi = *lo;     *lo = INT64_MIN; break;
        case CMP_GT: *lo = *lo + span; *hi = INT64_MAX; break;
        case CMP_GE: *hi = *lo + span - 1; *lo = *lo; break;
        case CMP_NE: *lo = INT64_MIN; *hi = INT64_MAX; break;
        default:     *hi = *lo + span - 1; break;    /* CMP_EQ: the whole span */
        }
        return;
    }

    switch (cmp) {
    case CMP_LT: *hi = *lo - 1; *lo = INT64_MIN; break;
    case CMP_LE: *hi = *lo;     *lo = INT64_MIN; break;
    case CMP_GT: *lo = *lo + 1; *hi = INT64_MAX; break;
    case CMP_GE: *lo = *lo;     *hi = INT64_MAX; break;
    case CMP_NE: *lo = INT64_MIN; *hi = INT64_MAX; break;
    default:     *hi = *lo; break;
    }
}


/* ----------------------------------------------------------- text matching */

/* Pull the subject a text leaf matches against. Everything's `path:` modifier
 * switches a bare word from the name to the full path; the `path`/`path-part`
 * functions force it. Materialised paths need a buffer, which is why the caller
 * owns one per evaluation. */
typedef struct { char *buf; size_t cap; } scratch_t;


/* Normalise for ignorepunc / ignorews / diacritics, writing into `out`.
 * `mode` is a bitmask: 1 = drop punctuation, 2 = drop whitespace,
 * 4 = strip diacritics (Latin-1 supplement folding, which is what ICU would be
 * pulled in for -- design §2 L2 notes the ICU dependency and we avoid it for
 * the common Latin range). */
static void normalise(const char *s, char *out, size_t outsz, unsigned mode)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 2 < outsz; p++) {
        unsigned char c = *p;
        if ((mode & 1) && !isalnum(c)) continue;
        if ((mode & 2) && isspace(c)) continue;
        if ((mode & 4) && c >= 0xC0 && c <= 0xDE && c != 0xD7) c = (unsigned char)(c - 0x20);
        out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* Materialise the entry's path with backslash separators -- the spelling the ETP
 * PATH column carries and the only one the client has ever seen -- into the
 * shared scratch buffer. Returns false if it could not be allocated.
 *
 * Text leaves retry against this when the POSIX spelling misses, because the
 * client composes its `path:regex:` patterns out of the paths it was handed. A
 * pattern written for `C:\Users\x` cannot match `/home/x`, and the client will
 * keep sending the former. */
static bool wire_form(const esidx_t *db, eid_t id, scratch_t *sc)
{
    if (!sc->buf) { sc->buf = malloc(65536); sc->cap = 65536; }
    if (!sc->buf) return false;
    path_of(db, id, sc->buf, sc->cap);
    for (char *p = sc->buf; *p; p++) if (*p == '/') *p = '\\';
    return true;
}

/* Whole-word test used by ww: and by prefix:/suffix: on a word boundary. */
static int is_word_char(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }

/* Does the string end with this suffix? The tail of a `path:` value is anchored
 * this way -- `path:*esidx` is the two entries whose path ends in "esidx" on the
 * reference, not the 267 that contain it. */
static int ends_with(const char *s, const char *suf, int nocase)
{
    size_t sl = strlen(s), fl = strlen(suf);
    if (fl > sl) return 0;
    return nocase ? strcasecmp(s + (sl - fl), suf) == 0
                  : strcmp(s + (sl - fl), suf) == 0;
}

/* The modifiers a leaf is matched with: its own, plus whatever the ETP match
 * options contribute where the leaf stayed silent.
 *
 * One function because the ETP options are *defaults* and this merge is a
 * judgement call, not a lookup: `CASE 1` makes matching case sensitive but
 * `nocase:foo` still wins, and the rule for that is "the leaf spoke for itself
 * anywhere in this group, so the whole group is left alone". Two copies of it --
 * one for the matcher, one for the trigram prefilter that has to agree with it --
 * would drift, and the drift would be silent: results would simply lose rows. */
static mod_t leaf_mods(const ast_t *t, const match_opts_t *mo)
{
    mod_t m = t->mod;
    const unsigned OVERRIDABLE = MOD_CASE | MOD_WW | MOD_PREFIX | MOD_SUFFIX |
                                 MOD_STARTWITH | MOD_ENDWITH | MOD_REGEX | MOD_PATH;
    if (!(m & OVERRIDABLE)) m |= match_opts_mods(mo) & OVERRIDABLE;
    if (mo) {
        if (mo->ignore_punctuation) m |= MOD_IGNOREPUNC;
        if (mo->ignore_whitespace)  m |= MOD_IGNOREWS;
        if (mo->match_diacritics)   m |= MOD_DIACRITICS;
    }
    return m;
}

static int text_match(const esidx_t *db, eid_t id, const ast_t *t,
                      const match_opts_t *mo, scratch_t *sc)
{
    mod_t m = leaf_mods(t, mo);

    const char *pat = t->val ? t->val : "";

    /* An unqualified term is a *filename* term, and the reference is emphatic
     * about it: on one directory, `esidx` answers 3 -- the three entries named
     * esidx -- where reading the path as well would answer 280, and `regex:esidx`
     * answers the same 3 (etp-probe against voidtools' own server on :21).
     *
     * Everything widens the term to the full path the moment the value carries a
     * separator, though: `esidx\main.c` answers 1 and `esidx\*` answers 38 on that
     * same directory. So the separator is the switch, MOD_PATH is that switch said
     * out loud, and both of them select the same subject.
     *
     * The wire_form() retry that follows belongs to the same half of the rule. The
     * client builds its patterns out of the paths it was handed, and the paths it
     * was handed are the backslash-separated ones the ETP PATH column carries, so
     * a pattern written for `C:\Users\x` cannot match `/home/x` (round trip
     * observed, AGENTS.md 5.1). Running that retry for a *name* is what turned
     * `name:sub1` and a bare `sub1` into "the directory and both things in it". */
    const int path_scope = (m & MOD_PATH) || strpbrk(pat, "/\\") != NULL;

    const char *subj;
    if (path_scope) {
        /* fn:path / path: / path-part: all read the full path */
        if (!sc->buf) { sc->buf = malloc(65536); sc->cap = 65536; }
        if (!sc->buf) return 0;
        path_of(db, id, sc->buf, sc->cap);
        subj = sc->buf;
    } else {
        subj = display_name_of(db, id);
    }

    /* stem: drops the extension before matching */
    if (t->fn && !strcmp(t->fn, "stem")) {
        static __thread char stem[4096];
        snprintf(stem, sizeof(stem), "%s", subj);
        char *dot = strrchr(stem, '.');
        if (dot && dot != stem) *dot = '\0';
        subj = stem;
    }

    if (m & MOD_REGEX) {
        re_t *re = re_compile(pat, !(m & MOD_CASE));
        if (!re) {
            LOGW("regex: %s -- pattern ignored, the term matches nothing", re_error());
            return 0;
        }
        int hit = re_match(re, subj);
        /* The retry has to happen BEFORE the free -- using `re` afterwards is a
         * use-after-free that -O2 hides. */
        if (!hit && path_scope) hit = wire_form(db, id, sc) ? re_match(re, sc->buf) : 0;
        re_free(re);
        return hit;
    }

    unsigned nm = ((m & MOD_IGNOREPUNC) ? 1u : 0u) |
                  ((m & MOD_IGNOREWS)   ? 2u : 0u) |
                  ((m & MOD_DIACRITICS) ? 4u : 0u);
    if (nm) {
        static __thread char nb[8192], pb[4096];
        normalise(pat, pb, sizeof(pb), nm);
        normalise(subj, nb, sizeof(nb), nm);
        if (!pb[0]) return 1;                    /* the term reduced to nothing */
        if (strcasestr(nb, pb)) return 1;
        if (path_scope && wire_form(db, id, sc)) {
            normalise(sc->buf, nb, sizeof(nb), nm);
            return strcasestr(nb, pb) != NULL;
        }
        return 0;
    }

    if (wildcard_present(pat)) {
        const int nocase = !(m & MOD_CASE);
        /* A wildcard in a *filename* means "the whole filename"
         * (everything-syntax.md L35), so startwith:/endwith: cannot combine with
         * one: there is no way to say "starts with" and "matches the whole string"
         * at once. */
        if (!path_scope) return wildcard_match(pat, subj, nocase);

        /* In a *path* it means "somewhere in here" (see wildcard_match_in), and
         * `\` is a separator there rather than an escape -- the reference answers
         * 38 for "esidx", a backslash and a star, the same 38 it answers with a
         * forward slash, and the same 38 `find -maxdepth 1` counts, so the
         * backslash is not escaping the star. Comparing both sides in POSIX
         * spelling is what makes the client's own spelling work without a second
         * pass over the wire form. */
        static __thread char ppat[4096];
        size_t o = 0;
        for (const char *p = pat; *p && o < sizeof(ppat) - 1; p++)
            ppat[o++] = (*p == '\\') ? '/' : *p;
        ppat[o] = '\0';

        /* `path:` with a leading star is Everything's contains form, and it is the
         * only shape where a star crosses a separator. On one directory the
         * reference answers 280 for `path:*esidx*`, which is every path containing
         * esidx, and 6590 -- all of them -- for `path:*PC*`, because every path
         * contains "PC". So it is `full_path.contains(literal)` and nothing more:
         * the stars at the ends are what makes it a contains test, and what is
         * left between them is the literal to look for.
         *
         * A value that is itself a path fragment is the exception, and the
         * exception is the trailing star: `path:*PC/esidx*` is 1 there, which is
         * `ends_with("PC/esidx")`, where contains would be 267 -- the star cannot
         * reach across the separator that is in the value. Same reference, same
         * tree. And with no trailing star at all it is ends-with either way:
         * `path:*esidx` is the 2 entries whose path ends in esidx.
         *
         * The leading star is what buys any of this. `path:esidx*` is the same
         * directory's 3 -- the entries *named* esidx -- and a bare `*PC/esidx*` is 1
         * where contains would be 267, so neither an anchored path: value nor a
         * bare term with a separator in it may take this branch. */
        if ((m & MOD_PATH) && ppat[0] == '*') {
            const char *body = ppat + 1;
            while (*body == '*') body++;
            size_t blen = strlen(body);
            bool open = blen && body[blen - 1] == '*';
            while (open && blen && body[blen - 1] == '*') blen--;
            static __thread char lit[4096];
            if (blen >= sizeof(lit)) blen = sizeof(lit) - 1;
            memcpy(lit, body, blen);
            lit[blen] = '\0';
            if (!strpbrk(lit, "/") && open)      /* path:*abc* -- abc occurs anywhere */
                return (nocase ? strcasestr(subj, lit) : strstr(subj, lit)) != NULL;
            /* an interior star survives as a literal, which no path holds, so those
             * shapes fail closed rather than matching something odd */
            return ends_with(subj, lit, nocase);
        }
        return wildcard_match_in(ppat, subj, nocase);
    }

    if (m & MOD_WHOLE)
        return strcmp(pat, subj) == 0;

    /* startwith:/endwith: anchor at the filename edge, no boundary required
     * (L84, L86) */
    if (m & MOD_STARTWITH) {
        size_t plen = strlen(pat);
        if (strlen(subj) < plen) return 0;
        return (m & MOD_CASE) ? strncmp(subj, pat, plen) == 0
                              : strncasecmp(subj, pat, plen) == 0;
    }
    if (m & MOD_ENDWITH) {
        size_t plen = strlen(pat), sl = strlen(subj);
        if (sl < plen) return 0;
        const char *tail = subj + (sl - plen);
        return (m & MOD_CASE) ? strcmp(tail, pat) == 0 : strcasecmp(tail, pat) == 0;
    }

    /* prefix: / suffix: anchor at a word boundary (L82, L83) */
    if (m & MOD_PREFIX) {
        size_t plen = strlen(pat), sl = strlen(subj);
        if (sl < plen) return 0;
        int c = (m & MOD_CASE) ? strncmp(subj, pat, plen) : strncasecmp(subj, pat, plen);
        if (c != 0) return 0;
        return plen == sl || !is_word_char((unsigned char)subj[plen]);
    }
    if (m & MOD_SUFFIX) {
        size_t plen = strlen(pat), sl = strlen(subj);
        if (sl < plen) return 0;
        const char *tail = subj + (sl - plen);
        int c = (m & MOD_CASE) ? strcmp(tail, pat) : strcasecmp(tail, pat);
        if (c != 0) return 0;
        return plen == sl || !is_word_char((unsigned char)tail[-1]);
    }

    /* plain substring, with optional whole-word boundaries */
    if (m & MOD_WW) {
        size_t plen = strlen(pat);
        if (!plen) return 1;
        for (const char *q = subj; (q = strcasestr(q, pat)) != NULL; q++) {
            int lok = (q == subj) || !is_word_char((unsigned char)q[-1]);
            int rok = !q[plen] || !is_word_char((unsigned char)q[plen]);
            if (lok && rok) return 1;
        }
        return 0;
    }

    if (!*pat) return 1;
    if (strcasestr(subj, pat)) return 1;
    if (path_scope && wire_form(db, id, sc)) return strcasestr(sc->buf, pat) != NULL;
    return 0;
}

/* ------------------------------------------------------- extension macros */

/* The type macros of everything-syntax.md L28-35. Everything grows these sets
 * over time; these are the extensions a Linux desktop actually holds, and the
 * rule is deliberately "unknown extension in a macro is simply not indexed",
 * so a macro degrades to fewer matches instead of an error. */
static const char *const macro_audio[] = {
    "aac","aif","aiff","amr","ape","au","caf","dts","flac","m4a","mid","midi",
    "mka","mod","mp1","mp2","mp3","mpc","oga","ogg","opus","ra","ram","snd",
    "spx","tta","voc","wav","wma","wv", NULL };
static const char *const macro_video[] = {
    "3g2","3gp","asf","avi","divx","dv","f4v","flv","m2ts","m2v","m4v","mkv",
    "mov","mp4","mpeg","mpg","mts","mxf","ogm","ogv","rm","rmvb","ts","vob",
    "webm","wmv","wtv","y4m", NULL };
static const char *const macro_image[] = {
    "apng","avif","bmp","gif","heic","heif","ico","jfif","jpeg","jpg","jpe",
    "jxl","png","psd","svg","tif","tiff","webp","xcf", NULL };
static const char *const macro_doc[] = {
    "csv","doc","docx","epub","key","md","mobi","numbers","odp","ods","odt",
    "pages","pdf","ppt","pptx","rtf","tex","txt","wpd","wps","xls","xlsx",
    "xml", NULL };
static const char *const macro_archive[] = {
    "7z","ace","ar","arc","bz2","cab","cpio","deb","dmg","gz","iso","jar",
    "lz","lz4","lzh","lzo","pkg","rar","rpm","tar","tbz","tgz","txz","xar",
    "xz","z","zip","zst", NULL };
static const char *const macro_exe[] = {
    "app","bin","com","cpl","deb","dll","exe","gadget","img","jar","msi",
    "msp","pif","rpm","scr","so", NULL };

/* `type:<name>` (L143). The names are matched loosely -- "picture" and "image"
 * are the same set -- because the ETP client's category picker sends display
 * names rather than internal ones, and Everything itself aliases several of them.
 * The two arrays are index-aligned; TYPE_NAMES[n] resolves to TYPE_SETS[n]. */
static const char *const *const type_sets[] = {
    macro_audio, macro_audio, macro_video, macro_video,
    macro_image, macro_image, macro_doc,   macro_doc,
    macro_archive, macro_archive, macro_exe, macro_exe, macro_exe, macro_exe,
};
static const char *const type_names[] = {
    "music", "audio", "video", "movie", "picture", "image", "document", "doc",
    "archive", "compressed", "executable", "exe", "program", "application",
};
#define TYPE_N (sizeof(type_names) / sizeof(type_names[0]))

/* Resolve a `;`- or space-separated extension list into interned ext ids. A
 * leading `*.` or `.` is accepted because both spellings occur in the wild. */
static uint32_t ext_list_ids(const esidx_t *db, const char *v, uint16_t *out, uint32_t max)
{
    uint32_t n = 0;
    const char *p = v;
    while (*p && n < max) {
        while (*p == ';' || *p == ',' || *p == ' ') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != ';' && *e != ',' && *e != ' ') e++;
        size_t l = (size_t)(e - p);
        const char *src = p;
        if (l >= 2 && src[0] == '*' && src[1] == '.') { src += 2; l -= 2; }
        else if (l >= 1 && src[0] == '.')          { src += 1; l -= 1; }
        else if (l == 1 && src[0] == '*')          { src += 1; l = 0; }
        if (l) {
            /* EXT_NAME_MAX, and it used to be 64 here against 31 in ext_of(): a query
             * could name an extension the index was incapable of holding. */
            char buf[EXT_NAME_MAX];
            if (l >= sizeof(buf)) l = sizeof(buf) - 1;
            for (size_t i = 0; i < l; i++) buf[i] = (char)tolower((unsigned char)src[i]);
            buf[l] = '\0';
            out[n++] = ext_intern((esidx_t *)db, buf);
        }
        p = e;
    }
    return n;
}

/* ------------------------------------------------------------- the matchers */

/* Every leaf is one of these. Two classes:
 *
 *   INDEXED  produces a whole-table bitmap from an index, so it can also seed
 *            the candidate set (design §6.2 step 2) and can be costed before it
 *            runs.
 *   SCAN     filters the incoming candidate set entry by entry. Never a driver,
 *            because producing its output means touching every candidate.
 *
 * `out` arrives holding the incoming candidate set and leaves holding the
 * result, so composition is just "run the children, then combine".
 */

typedef struct qctx qctx_t;

struct qctx {
    const esidx_t *db;
    const match_opts_t *mo;
    uint32_t n;              /* entry count */
    scratch_t sc;            /* path materialisation buffer */
    bitset_t  cand;          /* the candidate set every leaf starts from */
    uint32_t  cand_count;
};

/* Free everything qexec allocated, on the single exit path. A missed cleanup here
 * leaks the 64 KiB path buffer on every query that uses `path:`, which is exactly
 * what the ASan build of the test suite exists to catch -- and the leak also
 * swallows the buffered stdout, so it shows up as three mysteriously empty
 * results rather than as a leak report. */
static void qctx_done(qctx_t *c)
{
    free(c->sc.buf);
    bs_free(&c->cand);
    c->sc.buf = NULL;
    c->sc.cap = 0;
}

static int bs_alloc(qctx_t *c, bitset_t *b)
{
    return bs_init(b, c->n);
}

/* Allocate a set holding every entry that still exists.
 *
 * A removed entry keeps its id and its row (design §11 D8), so "the id is inside
 * the table" is not the same as "the entry is in the index". Every set that can
 * become a final result starts here rather than from bs_set_all(), which is what
 * keeps a tombstone out of the results without any of the forty matchers having
 * to know that removals exist. Sets built as `bs_alloc_full` for an OR or a NOT
 * do contain tombstones -- they are intermediates that are always combined into
 * a live-seeded set with bs_and / bs_or / bs_andnot, never returned. */
static int bs_alloc_live(qctx_t *c, bitset_t *b)
{
    if (bs_init(b, c->n) != 0) return -1;
    bs_copy(b, &c->db->live);
    return 0;
}

/* Allocate a scratch set that already holds every entry.
 *
 * This matters more than it looks. Matchers *intersect* with whatever set they
 * are handed, so a subtree evaluated into a fresh bitmap has to start as
 * "everything" -- otherwise `!folder:` would AND the directory set into a set of
 * zeroes and then complement it, yielding the whole table. One helper, used
 * everywhere a subtree result is computed, keeps that invariant in a single
 * place. */
static int bs_alloc_full(qctx_t *c, bitset_t *b)
{
    if (bs_init(b, c->n) != 0) return -1;
    bs_set_all(b, c->n);
    return 0;
}

/* Forward declarations: the leaf table, the evaluator and the matchers are
 * mutually recursive (m_child: evaluates a nested tree; eval_leaf: dispatches
 * through the table; m_ext_list: re-enters eval_leaf per list element). */
static int eval_leaf(qctx_t *c, const ast_t *t, bitset_t *out);
static int eval_node(qctx_t *c, const ast_t *t, bitset_t *out);

/* Run `fn` into `out`, intersecting with whatever `out` already holds. */
typedef int (*leaf_fn)(qctx_t *c, const ast_t *t, bitset_t *out);

static int eval_node(qctx_t *c, const ast_t *t, bitset_t *out);

/* ------------------------------------------------------------- path helpers */

/* Accept a path as the client sends it. The ETP wire is Windows-flavoured: the
 * ETP client joins `path + "\\" + name`, so a path that
 * came from us and went through the client comes back with mixed separators.
 * Normalising here is what keeps `parent:` a hash hit instead of a miss. */
static void normalise_path(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 2 < outsz; p++) {
        char c = (*p == '\\') ? '/' : *p;
        if (c == '/' && o > 0 && out[o - 1] == '/') continue;   /* collapse // */
        out[o++] = c;
    }
    /* a trailing separator is not part of any directory's own path */
    while (o > 1 && out[o - 1] == '/') o--;
    out[o] = '\0';
}

/* ------------------------------------------------------------ INDEXED leaves */

static int m_parent(qctx_t *c, const ast_t *t, bitset_t *out)
{
    char norm[4096];
    normalise_path(t->val ? t->val : "", norm, sizeof(norm));
    eid_t id = di_lookup(c->db, norm);
    if (id == EID_NONE) {
        /* Everything treats parent:"" as the top of a drive; we have exactly one
         * root, so that is our root's children. */
        if (norm[0] == '\0') id = c->db->root_eid;
        else {
            LOGD("parent: no such directory '%s'", norm);
            bs_clear(out);
            return 0;
        }
    }
    bs_clear(out);
    uint32_t cc = di_child_count(c->db, id);
    if (cc && id < c->db->di.child_cap) {
        const childvec_t *cv = &c->db->di.child[id];
        for (uint32_t i = 0; i < cv->n; i++) bs_set(out, cv->items[i]);
    }
    return 0;
}

static int m_root(qctx_t *c, const ast_t *t, bitset_t *out)
{
    (void)t;
    eid_t r = c->db->root_eid;
    bs_clear(out);
    if (r == EID_NONE) return 0;
    const childvec_t *cv = &c->db->di.child[r];
    for (uint32_t i = 0; i < cv->n; i++) bs_set(out, cv->items[i]);
    return 0;
}

static int m_folder(qctx_t *c, const ast_t *t, bitset_t *out)
{
    (void)t;
    bs_and(out, &c->db->type.dirs);
    return 0;
}

static int m_file(qctx_t *c, const ast_t *t, bitset_t *out)
{
    (void)t;
    bs_and(out, &c->db->type.files);
    return 0;
}

static int m_ext(qctx_t *c, const ast_t *t, bitset_t *out)
{
    uint16_t ids[256];
    uint32_t n = ext_list_ids(c->db, t->val, ids, 256);
    if (!n) { bs_clear(out); return 0; }

    bitset_t sel;
    if (bs_alloc(c, &sel) != 0) return -1;
    ext_index_select(&c->db->ext, ids, n, &sel);
    bs_and(out, &sel);
    bs_free(&sel);
    return 0;
}

static int m_macro(qctx_t *c, const ast_t *t, bitset_t *out, const char *const *exts)
{
    (void)t;
    uint16_t ids[256];
    uint32_t n = 0;
    for (size_t i = 0; exts[i] && n < 256; i++)
        ids[n++] = ext_intern((esidx_t *)c->db, (char *)exts[i]);
    if (!n) { bs_clear(out); return 0; }
    bitset_t sel;
    if (bs_alloc(c, &sel) != 0) return -1;
    ext_index_select(&c->db->ext, ids, n, &sel);
    bs_and(out, &sel);
    bs_free(&sel);
    return 0;
}

/* thin table wrappers, so the dispatch table can name each macro directly */
static int m_macro_audio(qctx_t *c, const ast_t *t, bitset_t *out)
{ return m_macro(c, t, out, macro_audio); }
static int m_macro_video(qctx_t *c, const ast_t *t, bitset_t *out)
{ return m_macro(c, t, out, macro_video); }
static int m_macro_image(qctx_t *c, const ast_t *t, bitset_t *out)
{ return m_macro(c, t, out, macro_image); }
static int m_macro_doc(qctx_t *c, const ast_t *t, bitset_t *out)
{ return m_macro(c, t, out, macro_doc); }
static int m_macro_archive(qctx_t *c, const ast_t *t, bitset_t *out)
{ return m_macro(c, t, out, macro_archive); }
static int m_macro_exe(qctx_t *c, const ast_t *t, bitset_t *out)
{ return m_macro(c, t, out, macro_exe); }

/* type:<name> -- dispatch by name onto the macro sets above */
static int m_macro_type(qctx_t *c, const ast_t *t, bitset_t *out)
{
    const char *v = t->val ? t->val : "";
    for (size_t i = 0; i < TYPE_N; i++) {
        if (!strcasecmp(v, type_names[i]))
            return m_macro(c, t, out, type_sets[i]);
    }
    /* Everything also accepts a bare extension list here */
    if (strchr(v, ';') || strchr(v, ',')) return m_ext(c, t, out);
    LOGD("type:%s -- unknown type name, matching nothing", v);
    bs_clear(out);
    return 0;
}

/* Adapter so dates and sizes share parse_range2(): it unwraps the tval_t and
 * publishes the span. */
static int64_t parse_time_adapter(const char *s, int *ok, int64_t *span)
{
    tval_t v = parse_time_value(s);
    /* Distinguish "not a date at all" from "a date at the epoch": an
     * unparseable bound must not silently become 1970 and match the whole tree. */
    if (v.at == 0) { *ok = 0; if (span) *span = 0; return 0; }
    *ok = 1;
    if (span) *span = v.span;
    return v.at;
}

/* One numeric range leaf over a sorted array, shared by size:, dm:, dc: ...
 * `allow_dash` is false for dates, where a dash is part of YYYY-MM-DD. */
static int range_on(qctx_t *c, const ast_t *t, bitset_t *out,
                    const sidx_t *sidx, int is_time)
{
    int64_t lo, hi;
    int ok = 0;
    parse_range2(t->val, t->cmp, &lo, &hi,
                 is_time ? parse_time_adapter : parse_size_value, &ok,
                 is_time ? 0 : 1);
    if (!ok) {
        LOGD("unparseable %s value '%s'", is_time ? "date" : "numeric",
             t->val ? t->val : "");
        bs_clear(out);
        return 0;
    }
    bitset_t sel;
    if (bs_alloc(c, &sel) != 0) return -1;
    sidx_range_to_bitset(sidx, lo, hi, &sel);
    bs_and(out, &sel);
    bs_free(&sel);
    return 0;
}

/* which plain column range_on_column() reads */
enum { COL_CHILDREN, COL_DEPTH, COL_LEN };

/* The same bounds, evaluated against a plain column instead of a sorted array:
 * depth:, len:, child-count:. */
static int range_on_column(qctx_t *c, const ast_t *t, bitset_t *out, int column)
{
    int64_t lo, hi;
    int ok = 0;
    parse_range2(t->val, t->cmp, &lo, &hi, parse_size_value, &ok, 1);
    if (!ok) { bs_clear(out); return 0; }
    const entry_table_t *et = &c->db->et;
    for (uint32_t i = 0, nn = c->n; i < nn; i++) {
        if (!bs_test(out, i)) continue;
        int64_t v;
        switch (column) {
        case COL_DEPTH:   v = et->depth[i]; break;
        case COL_LEN:     v = (int64_t)et->name[i].len; break;
        default:          v = di_child_count(c->db, i); break;   /* COL_CHILDREN */
        }
        if (v >= lo && v <= hi) continue;
        bs_clear_bit(out, i);
    }
    return 0;
}

static int m_size(qctx_t *c, const ast_t *t, bitset_t *out)
{
    return range_on(c, t, out, &c->db->by_size, 0);
}

static int m_mtime(qctx_t *c, const ast_t *t, bitset_t *out)
{
    return range_on(c, t, out, &c->db->by_mtime, 1);
}

static int m_ctime(qctx_t *c, const ast_t *t, bitset_t *out)
{
    return range_on(c, t, out, &c->db->by_ctime, 1);
}

/* depth: is a plain column scan today (design §2, L0 calls it inline O(1)); it is
 * still index-backed in the sense that matters here -- the cost is one linear
 * pass with no allocation, so it can seed the candidate set. */
static int m_depth(qctx_t *c, const ast_t *t, bitset_t *out)
{
    return range_on_column(c, t, out, COL_DEPTH);
}

/* attrib: on ext4 only H (leading dot) and D (from d_type) exist; everything
 * else has no filesystem equivalent and is reported as unsupported
 * (everything-syntax.md L239-240). */
static int m_attrib(qctx_t *c, const ast_t *t, bitset_t *out)
{
    const entry_table_t *et = &c->db->et;
    const char *v = t->val;
    int neg = 0;
    if (*v == '!') { neg = 1; v++; }
    else if (*v == '-') { neg = 1; v++; }
    else if (t->cmp == CMP_NE) neg = 1;
    if (!strcasecmp(v, "n") || !strcasecmp(v, "normal")) neg = !neg;

    int want_h = 0, want_d = 0, unsupported = 0;
    for (const char *p = v; *p; p++) {
        switch (tolower((unsigned char)*p)) {
        case 'h': want_h = 1; break;
        case 'd': want_d = 1; break;
        case ' ': case ',': case '+': break;
        default: unsupported = 1; break;
        }
    }
    if (unsupported)
        LOGW("attrib:%s -- only H (hidden) and D (directory) exist on ext4; "
             "the rest match nothing", v);
    if (!want_h && !want_d) { bs_clear(out); return 0; }

    for (uint32_t i = 0, nn = c->n; i < nn; i++) {
        if (!bs_test(out, i)) continue;
        uint16_t f = et->flags[i];
        int hit = (want_h ? (f & EF_HIDDEN) != 0 : 1) &&
                  (want_d ? (f & EF_DIR) != 0 : 1);
        if (hit == neg) bs_clear_bit(out, i);
    }
    return 0;
}

/* empty: -- a file with no bytes, or a directory with no children (L89 of
 * design §2). child_count is derived from the children vector, so no aggregate
 * column has to be maintained yet (§5.5). */
static int m_empty(qctx_t *c, const ast_t *t, bitset_t *out)
{
    const entry_table_t *et = &c->db->et;
    int want_empty = (t->cmp == CMP_EQ);
    for (uint32_t i = 0, nn = c->n; i < nn; i++) {
        if (!bs_test(out, i)) continue;
        int e = (et->flags[i] & EF_DIR) ? (di_child_count(c->db, i) == 0)
                                        : (et->size[i] == 0);
        if (e != want_empty) bs_clear_bit(out, i);
    }
    return 0;
}

static int m_child_count(qctx_t *c, const ast_t *t, bitset_t *out)
{
    return range_on_column(c, t, out, COL_CHILDREN);
}

static int m_len(qctx_t *c, const ast_t *t, bitset_t *out)
{
    return range_on_column(c, t, out, COL_LEN);
}

/* -------------------------------------------------------------- SCAN leaves */

/* ------------------------------------------------------- trigram prefilter */

/* NAME_MAX on ext4, so a literal longer than this cannot be a substring of any
 * display name. Truncating to it is still sound -- a prefix of a necessary
 * condition is a necessary condition -- and it keeps this off the stack of every
 * nested eval. */
#define TRI_LIT_MAX 256

/* Is this leaf one the name trigram index may narrow?
 *
 * The test is an allowlist of functions and an allowlist of modifiers, and that is
 * the whole safety argument. The index is keyed on the folded *display name*, so
 * every shape below has to be one where text_match() reads that name and compares
 * it with the C locale's case-insensitive substring:
 *
 *   - path: and a separator in the value read the reconstructed path instead, and
 *     the path index is a separate decision (design §5.2, and the path is O(depth)
 *     to materialise -- §12 risk 7).
 *   - stem: reads the name with its extension cut off.
 *   - case:, ignorepunc:, ignorews: and diacritics: all change what is compared,
 *     and a normalising term has no literal left to look up.
 *   - regex: has no literal to extract.
 *
 * MOD_PATH and MOD_CASE are absent from the modifier allowlist, which is how the
 * first and the case rule are enforced without naming them. A modifier nobody has
 * thought of yet is refused by construction rather than silently accepted, which is
 * the direction to be wrong in: a refused prefilter is only slower.
 *
 * Most of the `fn` names below never arrive here -- `whole:`, `ww:`, `path:` and
 * the rest are peeled as modifiers and leave fn empty (parser.c peel_mods) -- but
 * they are listed, because listing what is allowed is the point. */
static bool tri_applies(const ast_t *t, const match_opts_t *mo)
{
    static const char *const ok_fn[] = {
        "", "name", "name-part", "whole", "ww",
        "startwith", "endwith", "prefix", "suffix"
    };
    const mod_t ALLOWED = MOD_WHOLE | MOD_WW | MOD_PREFIX | MOD_SUFFIX |
                          MOD_STARTWITH | MOD_ENDWITH;

    if (leaf_mods(t, mo) & ~ALLOWED) return false;
    const char *fn = t->fn ? t->fn : "";
    bool known = false;
    for (size_t i = 0; i < sizeof(ok_fn) / sizeof(ok_fn[0]); i++)
        if (!strcmp(fn, ok_fn[i])) { known = true; break; }
    if (!known) return false;

    const char *pat = t->val ? t->val : "";
    /* a separator in the value widens the term to the path (text_match's
     * path_scope), which is the same exclusion as MOD_PATH by another route */
    return strpbrk(pat, "/\\") == NULL;
}

/* The longest run of literal bytes in a pattern, copied out. ASCII case is left
 * alone: tri_index_filter() folds it, and folding it here as well would be one
 * more place for the two to disagree about what the index holds.
 *
 * Everything anchors a wildcard to the whole *filename*, so every literal run
 * between two wildcards has to occur in the name -- which makes the longest one a
 * necessary condition and therefore a sound thing to prefilter on. A backslash
 * escape breaks the run too, deliberately: `\x` matches exactly one character, so
 * stopping short of it yields a run that is still a substring of what the pattern
 * really means. A run of `#` or a `[...]` class likewise contributes nothing --
 * `#` is one digit and a class matches one of a set, neither of which is a
 * literal to look up.
 *
 * False means "no run of 3 bytes or more", which is also the shape the index
 * cannot answer at all: ref A9 -- a name under 3 bytes has no trigram, so a
 * 2-byte query has nothing to look up and the scan is the only answer. */
static bool tri_literal(const char *pat, char *out, size_t outsz)
{
    const char *best = pat;
    size_t bestlen = 0, run = 0;

    for (const char *p = pat; *p;) {
        if (*p == '*' || *p == '?' || *p == '#') { p++; run = 0; continue; }
        if (*p == '[') {
            const char *close = strchr(p, ']');
            p = close ? close + 1 : p + strlen(p);
            run = 0;
            continue;
        }
        if (*p == '\\' && p[1]) { p += 2; run = 0; continue; }
        p++;
        run++;
        if (run > bestlen) { bestlen = run; best = p - run; }
    }
    if (bestlen < 3 || bestlen >= outsz) return false;
    memcpy(out, best, bestlen);
    out[bestlen] = '\0';
    return true;
}

/* Text leaves walk the incoming candidate set. This is where the driver choice
 * pays off: after `parent:` seeds the bitmap, a substring test costs one
 * strcasestr per child rather than per indexed entry. */
static int scan_text(qctx_t *c, const ast_t *t, bitset_t *out)
{
    /* design §5.2: before the per-row loop, intersect the candidate set with the
     * trigrams the pattern cannot match without. The loop below still decides
     * every row that is left -- this can only remove rows it would have rejected,
     * which is what makes it safe to apply to some shapes and not others. */
    char lit[TRI_LIT_MAX];
    if (tri_applies(t, c->mo) && tri_literal(t->val ? t->val : "", lit, sizeof(lit))) {
        uint32_t before = bs_count(out);
        if (tri_index_filter(&c->db->tri, lit, out))
            LOGD("text: trigram prefilter '%s': %u -> %u candidates",
                 lit, before, bs_count(out));
    }

    for (uint32_t i = bs_next(out, 0); i < c->n; i = bs_next(out, i + 1)) {
        if (!text_match(c->db, i, t, c->mo, &c->sc)) bs_clear_bit(out, i);
    }
    return 0;
}

/* `child:<expr>` -- the folders that directly contain a match of <expr>
 * (everything-syntax.md L111). The nested search is evaluated unconstrained --
 * the interesting part is the innermost match, not the outer candidate set --
 * and the answer is the set of those matches' parents. */
static int m_child(qctx_t *c, const ast_t *t, bitset_t *out)
{
    if (!t->sub) { bs_clear(out); return 0; }

    bitset_t inner, parents;
    if (bs_alloc(c, &inner) != 0) return -1;
    if (bs_alloc(c, &parents) != 0) { bs_free(&inner); return -1; }

    /* the nested search runs unconstrained against the whole table */
    bitset_t keep_cand = c->cand;
    uint32_t keep_cnt = c->cand_count;
    bs_copy(&inner, &c->db->live);
    c->cand = inner;
    c->cand_count = bs_count(&inner);
    int rc = eval_node(c, t->sub, &inner);
    c->cand = keep_cand;
    c->cand_count = keep_cnt;
    if (rc != 0) { bs_free(&inner); bs_free(&parents); return rc; }

    const entry_table_t *et = &c->db->et;
    for (uint32_t i = bs_next(&inner, 0); i < c->n; i = bs_next(&inner, i + 1)) {
        eid_t p = et->parent[i];
        if (p != EID_NONE) bs_set(&parents, p);
    }
    bs_and(out, &parents);
    bs_free(&inner);
    bs_free(&parents);
    return 0;
}

/* A function we parse but cannot answer. Everything returns an empty result for
 * an unsupported search rather than an error, so the client's UI degrades to
 * "no results" instead of an error dialog. */
static int m_unsupported(qctx_t *c, const ast_t *t, bitset_t *out)
{
    (void)c;
    static int warned = 0;
    if (!warned) {
        LOGW("%s: not supported on Linux -- the term matches nothing "
             "(design §2, L3)", t->fn && t->fn[0] ? t->fn : "?");
        warned = 1;
    }
    bs_clear(out);
    return 0;
}

/* ------------------------------------------------------------ the dispatcher */

typedef struct {
    const char *fn;
    int         indexed;      /* may seed the candidate set */
    leaf_fn     fn_leaf;
    leaf_fn     fn_macro;     /* only for the type macros */
} entry_t;

#define ENTRY(nm, idx, f)        { nm, idx, f, NULL }
#define MACRO(nm, f)             { nm, 1, NULL, f }

/* The table is the single place that maps a language function onto a mechanism,
 * which is the whole point of ref D1: adding a function is one row here, not a
 * branch somewhere in the executor. */
static const entry_t table[] = {
    /* L0 -- structural */
    ENTRY("parent",   1, m_parent),
    ENTRY("root",     1, m_root),
    ENTRY("depth",    1, m_depth),
    ENTRY("child",    0, m_child),

    /* L1 -- scalar / enum, index-backed */
    ENTRY("file",     1, m_file),
    ENTRY("folder",   1, m_folder),
    ENTRY("directory",1, m_folder),
    ENTRY("ext",      1, m_ext),
    ENTRY("extension",1, m_ext),
    ENTRY("size",     1, m_size),
    ENTRY("dm",       1, m_mtime),
    ENTRY("date-modified", 1, m_mtime),
    ENTRY("dc",       1, m_ctime),
    ENTRY("date-created",  1, m_ctime),
    ENTRY("attrib",   1, m_attrib),
    ENTRY("empty",    1, m_empty),
    ENTRY("child-count", 1, m_child_count),
    ENTRY("len",      1, m_len),

    /* type macros -- extension sets folded through the ext bitmap */
    MACRO("audio",   m_macro_audio),
    MACRO("video",   m_macro_video),
    MACRO("image",   m_macro_image),
    MACRO("picture", m_macro_image),
    MACRO("doc",     m_macro_doc),
    MACRO("document",m_macro_doc),
    MACRO("archive", m_macro_archive),
    MACRO("zip",     m_macro_archive),
    MACRO("exe",     m_macro_exe),
    MACRO("type",    m_macro_type),

    /* L2 -- text, in-memory scan (design §5.2: the trigram index is a P4 gate) */
    ENTRY("",        0, scan_text),      /* a bare word */
    ENTRY("name",    0, scan_text),
    ENTRY("name-part", 0, scan_text),
    ENTRY("path",    0, scan_text),
    ENTRY("path-part", 0, scan_text),
    ENTRY("stem",    0, scan_text),
    ENTRY("regex",   0, scan_text),
    ENTRY("whole",   0, scan_text),
    ENTRY("ww",      0, scan_text),
    ENTRY("startwith", 0, scan_text),
    ENTRY("endwith", 0, scan_text),
    ENTRY("prefix",  0, scan_text),
    ENTRY("suffix",  0, scan_text),

    /* L3 -- parsed, and deliberately answered with an empty set */
    ENTRY("content", 0, m_unsupported),
    ENTRY("dupe",    0, m_unsupported),
    ENTRY("filelist",0, m_unsupported),
    ENTRY("filelist-filename", 0, m_unsupported),
    ENTRY("si",      0, m_unsupported),
    ENTRY("index-type", 0, m_unsupported),
    ENTRY("run-count", 0, m_unsupported),
    ENTRY("frn",     0, m_unsupported),
    ENTRY("album",   0, m_unsupported),
    ENTRY("artist",  0, m_unsupported),
    ENTRY("title",   0, m_unsupported),
    ENTRY("genre",   0, m_unsupported),
    ENTRY("comment", 0, m_unsupported),
    ENTRY("tag",     0, m_unsupported),
    ENTRY("track",   0, m_unsupported),
    ENTRY("length",  0, m_unsupported),
    ENTRY("width",   0, m_unsupported),
    ENTRY("height",  0, m_unsupported),
    ENTRY("bitdepth",0, m_unsupported),
    ENTRY("orientation", 0, m_unsupported),
    ENTRY("star-rating", 0, m_unsupported),
    ENTRY("child-file-count", 0, m_unsupported),
    ENTRY("child-folder-count", 0, m_unsupported),
};

static const entry_t *find_entry(const char *fn)
{
    if (!fn) fn = "";
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (!strcmp(table[i].fn, fn)) return &table[i];
    return NULL;
}

/* --------------------------------------------------------- cost estimation */

/* What the driver choice is worth. This is design §6.2 step 1 in its simplest
 * form: a cardinality estimate per leaf, and the smallest one seeds the
 * candidate bitmap. There is no histogram yet -- for the leaves that matter the
 * exact count is already available in O(1) or O(log n), which is better than a
 * 128-bucket estimate would be. */
static uint32_t est_leaf(qctx_t *c, const ast_t *t)
{
    const entry_t *e = find_entry(t->fn);
    if (!e || !e->indexed) return UINT32_MAX;

    if (!strcmp(t->fn, "parent")) {
        char norm[4096];
        normalise_path(t->val ? t->val : "", norm, sizeof(norm));
        eid_t id = (norm[0] == '\0') ? c->db->root_eid : di_lookup(c->db, norm);
        return id == EID_NONE ? 0 : di_child_count(c->db, id);
    }
    if (!strcmp(t->fn, "root"))
        return c->db->root_eid == EID_NONE ? 0 : di_child_count(c->db, c->db->root_eid);
    if (!strcmp(t->fn, "file") || !strcmp(t->fn, "folder") ||
        !strcmp(t->fn, "directory")) {
        /* the function picks the set; only a `!=` / `!` comparison flips it */
        uint32_t in_set = (!strcmp(t->fn, "file")) ? bs_count(&c->db->type.files)
                                                   : bs_count(&c->db->type.dirs);
        if (t->cmp == CMP_NE) {
            uint32_t all = bs_count(&c->db->type.all);
            return in_set <= all - in_set ? in_set : all - in_set;
        }
        return in_set;
    }
    if (e->fn_macro) return c->n / 8;      /* a guess is enough to lose to parent: */
    if (!strcmp(t->fn, "ext") || !strcmp(t->fn, "extension")) {
        if (t->list) {
            /* an AND list is bounded by its narrowest element, an OR list by the
             * sum; either way it never beats a single-element estimate */
            uint32_t acc = (t->list->kind == AST_AND) ? UINT32_MAX : 0;
            for (const ast_t *l = t->list->a; l; l = l->b) {
                if (!l->val) continue;
                uint16_t ids[256];
                uint32_t k = ext_list_ids(c->db, l->val, ids, 256);
                uint32_t sum = 0;
                for (uint32_t i = 0; i < k; i++) {
                    uint32_t one = ext_index_count(&c->db->ext, ids[i]);
                    if (one == UINT32_MAX) return UINT32_MAX;
                    sum += one;
                }
                if (t->list->kind == AST_AND) { if (sum < acc) acc = sum; }
                else acc += sum;
            }
            return acc;
        }
        uint16_t ids[256];
        uint32_t k = ext_list_ids(c->db, t->val, ids, 256);
        uint32_t sum = 0;
        for (uint32_t i = 0; i < k; i++) {
            uint32_t one = ext_index_count(&c->db->ext, ids[i]);
            if (one == UINT32_MAX) return UINT32_MAX;   /* unknown ext -> give up */
            sum += one;
        }
        return sum;
    }
    if (!strcmp(t->fn, "size") || !strcmp(t->fn, "dm") || !strcmp(t->fn, "dc")) {
        const sidx_t *s = !strcmp(t->fn, "size") ? &c->db->by_size
                        : !strcmp(t->fn, "dm")   ? &c->db->by_mtime
                                                  : &c->db->by_ctime;
        int64_t lo, hi;
        int ok = 0;
        cmp_op_t cmp = t->cmp;
        if (s == &c->db->by_size) parse_range2(t->val, cmp, &lo, &hi, parse_size_value, &ok, 1);
        else                     parse_range2(t->val, cmp, &lo, &hi, parse_time_adapter, &ok, 0);
        if (!ok || !s->n) return UINT32_MAX;
        /* the same binary search the range query will do, so the estimate is
         * exact rather than sampled */
        uint32_t a = 0, b = s->n;
        while (a < b) { uint32_t m = a + (b - a) / 2; if (s->a[m].v < lo) a = m + 1; else b = m; }
        uint32_t cnt = 0;
        for (uint32_t i = a; i < s->n && s->a[i].v <= hi; i++) cnt++;
        return cnt;
    }
    return UINT32_MAX;
}

/* Walk the tree for the cheapest index-backed leaf. `depth` guards against a
 * pathological AST; the parser already caps nesting at 64. */
static uint32_t pick_driver(qctx_t *c, const ast_t *t, uint32_t *best,
                            const ast_t **best_leaf, int depth)
{
    if (!t || depth > 64) return UINT32_MAX;
    if (t->kind == AST_TERM) {
        uint32_t e = est_leaf(c, t);
        if (e != UINT32_MAX && e < *best) { *best = e; *best_leaf = t; }
        return e;
    }
    uint32_t a = UINT32_MAX, b = UINT32_MAX;
    if (t->kind != AST_NOT) {
        a = pick_driver(c, t->a, best, best_leaf, depth + 1);
        if (t->b) b = pick_driver(c, t->b, best, best_leaf, depth + 1);
    }
    /* A NOT can never seed: its result is the complement, which is large. */
    if (a == UINT32_MAX && b == UINT32_MAX) return UINT32_MAX;
    /* inside an OR the result is at least the sum of the operands */
    if (t->kind == AST_OR) {
        uint64_t s = (uint64_t)a + (uint64_t)b;
        return s > UINT32_MAX ? UINT32_MAX : (uint32_t)s;
    }
    return a < b ? a : b;
}

/* ---------------------------------------------------------------- evaluation */

/* A value list -- `fn:<a b>` is an AND list, `fn:<a|b>` an OR list
 * (everything-syntax.md L162-163).
 *
 * Handled here, above the dispatch table, because it is orthogonal to which
 * function is being applied: `ext:<a b>`, `dm:<a b>` and `size:<a b>` all mean
 * "apply this function to each element and combine". Doing it in one place is
 * also what keeps the two combinations honest -- an AND list narrows the incoming
 * set, an OR list has to build its union from empty before intersecting, because
 * OR-ing into the incoming set would leave every existing candidate in place and
 * quietly turn a filter into a no-op. */
static int eval_list(qctx_t *c, const ast_t *t, const entry_t *e, bitset_t *out)
{
    int is_and = (t->list->kind == AST_AND);

    if (is_and) {
        for (const ast_t *l = t->list->a; l; l = l->b) {
            if (!l->val) continue;
            ast_t tmp = *l;
            tmp.fn = (char *)(t->fn && t->fn[0] ? t->fn : "ext");
            tmp.cmp = t->cmp;
            tmp.mod = t->mod;
            tmp.list = NULL;
            tmp.sub = NULL;
            if (e->fn_macro ? e->fn_macro(c, &tmp, out) : e->fn_leaf(c, &tmp, out)) return -1;
        }
        return 0;
    }

    bitset_t acc;
    if (bs_alloc(c, &acc) != 0) return -1;      /* empty: the union starts here */
    for (const ast_t *l = t->list->a; l; l = l->b) {
        if (!l->val) continue;
        ast_t tmp = *l;
        tmp.fn = (char *)(t->fn && t->fn[0] ? t->fn : "ext");
        tmp.cmp = t->cmp;
        tmp.mod = t->mod;
        tmp.list = NULL;
        tmp.sub = NULL;
        bitset_t one;
        if (bs_alloc_full(c, &one) != 0) { bs_free(&acc); return -1; }
        int rc = e->fn_macro ? e->fn_macro(c, &tmp, &one) : e->fn_leaf(c, &tmp, &one);
        if (rc != 0) { bs_free(&one); bs_free(&acc); return rc; }
        bs_or(&acc, &one);
        bs_free(&one);
    }
    bs_and(out, &acc);
    bs_free(&acc);
    return 0;
}

static int eval_leaf(qctx_t *c, const ast_t *t, bitset_t *out)
{
    const entry_t *e = find_entry(t->fn);
    if (!e) {
        /* An unknown function name is not an error in Everything -- it becomes a
         * search for the whole `name:value` text. */
        ast_t tmp = *t;
        char buf[1200];
        if (t->fn && t->fn[0]) snprintf(buf, sizeof(buf), "%s:%s", t->fn, t->val ? t->val : "");
        else snprintf(buf, sizeof(buf), "%s", t->val ? t->val : "");
        tmp.fn = (char *)"";
        tmp.val = buf;
        return scan_text(c, &tmp, out);
    }
    if (t->list && (e->fn_macro || e->fn_leaf))
        return eval_list(c, t, e, out);
    if (e->fn_macro) return e->fn_macro(c, t, out);
    return e->fn_leaf(c, t, out);
}

static int eval_node(qctx_t *c, const ast_t *t, bitset_t *out)
{
    if (!t) return 0;
    switch (t->kind) {
    case AST_TERM:  return eval_leaf(c, t, out);
    case AST_AND: {
        if (eval_node(c, t->a, out) != 0) return -1;
        if (t->b && eval_node(c, t->b, out) != 0) return -1;
        return 0;
    }
    case AST_OR: {
        bitset_t tmp;
        if (bs_alloc_full(c, &tmp) != 0) return -1;
        if (eval_node(c, t->a, &tmp) != 0) { bs_free(&tmp); return -1; }
        if (t->b && eval_node(c, t->b, out) != 0) { bs_free(&tmp); return -1; }
        bs_or(out, &tmp);
        bs_free(&tmp);
        return 0;
    }
    case AST_NOT: {
        bitset_t tmp;
        if (bs_alloc_full(c, &tmp) != 0) return -1;
        if (eval_node(c, t->a, &tmp) != 0) { bs_free(&tmp); return -1; }
        bs_andnot(out, &tmp);
        bs_free(&tmp);
        return 0;
    }
    }
    return 0;
}

/* ------------------------------------------------------------- FILTER_* layer */

/* The filter group is a second, independent matcher, not a variant of the
 * primary search (design §1.1 note, F8). It runs over the primary result set,
 * which is why `keep` is a separate set rather than another conjunct. */
static void apply_filter(qctx_t *c, bitset_t *set)
{
    const match_opts_t *mo = c->mo;
    if (!mo || (!mo->filter_search && !mo->filter_flags)) return;

    mod_t m = 0;
    if (mo->filter_flags & FF_CASE)            m |= MOD_CASE;
    if (mo->filter_flags & FF_WHOLEWORD)      m |= MOD_WW;
    if (mo->filter_flags & FF_PATH)           m |= MOD_PATH;
    if (mo->filter_flags & FF_DIACRITICS)     m |= MOD_DIACRITICS;
    if (mo->filter_flags & FF_PREFIX)         m |= MOD_PREFIX;
    if (mo->filter_flags & FF_SUFFIX)         m |= MOD_SUFFIX;
    if (mo->filter_flags & FF_IGNORE_PUNCTUATION) m |= MOD_IGNOREPUNC;
    if (mo->filter_flags & FF_IGNORE_WHITESPACE)  m |= MOD_IGNOREWS;
    if (mo->filter_flags & FF_REGEX)          m |= MOD_REGEX;

    /* Flags with no filter string still mean something: they restrict where the
     * secondary pass looks. Without FF_PATH it looks at the name; with it, at the
     * full path. And an empty filter string matches nothing, which is what
     * Everything does -- otherwise `FILTER_PATH 1` with no FILTER_SEARCH would
     * silently widen the result set instead of leaving it alone. */
    if (!mo->filter_search || !*mo->filter_search) {
        LOGD("filter: flags set but no filter string -- the primary set is unchanged");
        return;
    }

    ast_t t;
    memset(&t, 0, sizeof(t));
    t.kind = AST_TERM;
    t.mod = m;
    t.val = (char *)mo->filter_search;
    uint32_t before = bs_count(set);
    for (uint32_t i = bs_next(set, 0); i < c->n; i = bs_next(set, i + 1))
        if (!text_match(c->db, i, &t, NULL, &c->sc)) bs_clear_bit(set, i);
    LOGD("filter: %u of %u survived '%s'", bs_count(set), before, mo->filter_search);
}

/* ------------------------------------------------------------------ sorting */

/* One row of the sort. `s` is the primary key, ASCII-folded unless the key is numeric
 * or a path (see fa_put_x), and `dn` is the display name the tie-break falls back on.
 * `nrank` is that same name's rank, which is what the tie-break reads when the index
 * has one -- so the two fields are alternatives, not a pair.
 *
 * Folding once per row here is what lets the comparator use memcmp: a sort performs
 * ~log2(n) comparisons per row, so paying the fold once and memcmp-ing many times is
 * the difference between 169 ms and rather less. The rank is better still, because it
 * removes the comparison instead of making it cheaper.
 *
 * `dn` is not a second copy where it can be a pointer into the first: for a name sort
 * it is `s`, and for a path sort it is the tail of `s` after the last separator. Only
 * the extension and numeric keys need a copy of their own -- and only when the rank is
 * missing, which is the fallback path rather than the ordinary one. */
typedef struct {
    eid_t       id;
    uint32_t    nrank;
    int64_t     num;
    const char *s;
    const char *dn;
} srec_t;

typedef struct {
    const esidx_t *db;
    sort_key_t     key;
    int            desc;
    /* One scratch buffer, reused for every row, so path_of() writes into the same
     * 64 KiB all the way down the column instead of allocating per row. */
    char          *pathbuf;
    size_t         pathcap;
    uint64_t       ncmp;      /* comparisons performed, for the sort breakdown */
    int            ranked;    /* SORT_NAME reads name_rank, so `num` holds the rank */
    int            tie_rank;  /* the tie-break compares nrank instead of `dn` */
} sort_ctx_t;

/* Everything sorts a name case-insensitively, and the sort was 6 M `strcasecmp`
 * calls to say so (design §10: 169 ms of sort over an unfiltered `/usr`, 54 ms of
 * `image:`). Two facts make that avoidable:
 *
 *   1. In the C locale -- the only locale this process ever has, because nothing
 *      calls setlocale() -- `tolower()` maps A-Z and nothing else. So a
 *      case-insensitive byte order *is* an ASCII fold, and a byte >= 0x80 compares
 *      as itself. This is written out rather than borrowed precisely because that
 *      is the assumption: under a real locale, or a UTF-8 case fold, it would order
 *      names differently, and the result order is part of the contract (design §1.2,
 *      22 sort names, and cmp_ref.sh compares ordered result sets).
 *   2. `memcmp` on folded bytes is vectorised; `strcasecmp` cannot be, because it
 *      has to fold as it goes.
 *
 * The same fold builds the trigram keys in trigram.c. That is not a coincidence to
 * be tidy about: it is one definition of "case-insensitive" in the codebase, so the
 * sort and the prefilter cannot disagree about it. */
static inline unsigned char ascii_fold(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

static void fold_into(const char *s, char *out, size_t outsz)
{
    size_t i = 0;
    for (; s[i] && i + 1 < outsz; i++) out[i] = (char)ascii_fold((unsigned char)s[i]);
    out[i] = '\0';
}

/* Folded keys for one sort: a list of fixed-size blocks that are never moved.
 *
 * It was a single realloc-doubling buffer, which is wrong in a way -O2 hides: every
 * pointer handed out for an earlier row points into the block that the doubling
 * copied away and freed, so `rows[j].s` is dangling for every row extracted before the
 * last growth. The order assertions pass at -O2 because the freed block still holds
 * its bytes, and fail under AddressSanitizer, which is the only reason this was found
 * before it shipped (AGENTS.md 3.1).
 *
 * Blocks rather than one realloc'd buffer because a sort hands out n pointers that all
 * have to stay valid until it ends. A pointer per row is what makes the alternative --
 * resolving an offset at compare time -- an extra indirection in the hot loop. */
#define FA_BLOCK (256u * 1024u)

typedef struct {
    char   **blk;
    size_t  *used;
    uint32_t nblk, cblk;
} foldarena_t;

static void fa_init(foldarena_t *fa) { memset(fa, 0, sizeof(*fa)); }

static void fa_done(foldarena_t *fa)
{
    for (uint32_t i = 0; i < fa->nblk; i++) free(fa->blk[i]);
    free(fa->blk);
    free(fa->used);
    memset(fa, 0, sizeof(*fa));
}

/* Copy `s` into the arena, folded when `fold` is set. Returns a stable pointer the
 * comparator can read, or "" when a block cannot be allocated -- which costs order,
 * not correctness: those rows compare equal and the id tie-break decides.
 *
 * The flag is not a micro-optimisation. A path is ~70 bytes against a name's ~20, and
 * the fold exists only so memcmp can be used; for a path it costs more than the
 * vectorised compare saves -- measured over an unfiltered /usr, folding every path
 * cost 25 % of the sort (144 ms -> 190 ms) while the same fold on a *name* is free,
 * because a name sort does not compare names at all (it compares ranks). So each key
 * gets the comparator its string length justifies, and which is which is recorded in
 * the call site rather than re-derived here. */
static const char *fa_put_x(foldarena_t *fa, const char *s, bool fold)
{
    size_t n = strlen(s) + 1;
    if (n > FA_BLOCK) {           /* one path longer than a block: give it its own */
        char *own = malloc(n);
        if (!own) return "";
        if (fold) fold_into(s, own, n); else memcpy(own, s, n);
        if (fa->nblk == fa->cblk) {
            uint32_t nc = fa->cblk ? fa->cblk * 2 : 8;
            char **nb = realloc(fa->blk, (size_t)nc * sizeof(char *));
            size_t *nu = realloc(fa->used, (size_t)nc * sizeof(size_t));
            if (!nb || !nu) { free(own); free(nb); free(nu); return ""; }
            fa->blk = nb; fa->used = nu; fa->cblk = nc;
        }
        fa->blk[fa->nblk] = own;
        fa->used[fa->nblk] = n;
        fa->nblk++;
        return own;
    }
    if (fa->nblk == 0 || fa->used[fa->nblk - 1] + n > FA_BLOCK) {
        char *b = malloc(FA_BLOCK);
        if (!b) return "";
        if (fa->nblk == fa->cblk) {
            uint32_t nc = fa->cblk ? fa->cblk * 2 : 8;
            char **nb = realloc(fa->blk, (size_t)nc * sizeof(char *));
            size_t *nu = realloc(fa->used, (size_t)nc * sizeof(size_t));
            if (!nb || !nu) { free(b); free(nb); free(nu); return ""; }
            fa->blk = nb; fa->used = nu; fa->cblk = nc;
        }
        fa->blk[fa->nblk] = b;
        fa->used[fa->nblk] = 0;
        fa->nblk++;
    }
    char *out = fa->blk[fa->nblk - 1] + fa->used[fa->nblk - 1];
    if (fold) fold_into(s, out, FA_BLOCK - fa->used[fa->nblk - 1]);
    else      memcpy(out, s, n);
    fa->used[fa->nblk - 1] += n;
    return out;
}

static const char *fa_put(foldarena_t *fa, const char *s)
{
    return fa_put_x(fa, s, true);
}

/* The primary key, raw. The caller folds it into the arena unless the key is
 * numeric, so this function stays the single place that decides *what* the key is.
 * For a path key it fills the shared scratch and returns that, which is why the
 * caller must fold before asking again. */
static const char *sort_string(sort_ctx_t *sc, eid_t id)
{
    const esidx_t *db = sc->db;
    switch (sc->key) {
    case SORT_EXT:
        return ext_of_str(db, id);
    case SORT_PATH:
    case SORT_FILE_LIST_FILENAME: {
        if (!sc->pathbuf) {
            sc->pathbuf = malloc(65536);
            sc->pathcap = 65536;
        }
        if (!sc->pathbuf) return "";
        path_of(db, id, sc->pathbuf, sc->pathcap);
        return sc->pathbuf;
    }
    case SORT_ATTRIBUTES:
    case SORT_RECENTLY_CHANGED:
    case SORT_MTIME:
    case SORT_CTIME:
    case SORT_SIZE:
        return "";
    default:
        return display_name_of(db, id);
    }
}

static bool sort_key_is_numeric(sort_key_t k)
{
    return k == SORT_SIZE || k == SORT_MTIME || k == SORT_CTIME ||
           k == SORT_ATTRIBUTES || k == SORT_RECENTLY_CHANGED;
}

static int64_t sort_number(sort_ctx_t *sc, eid_t id)
{
    const esidx_t *db = sc->db;
    switch (sc->key) {
    case SORT_SIZE:  return db->et.size[id];
    case SORT_MTIME: return db->et.mtime[id];
    case SORT_CTIME: return db->et.ctime[id];
    /* there is no recently_changed column; mtime is its upper bound, so ordering
     * by mtime is the closest honest answer (warned once at DEBUG by qexec) */
    case SORT_RECENTLY_CHANGED: return db->et.mtime[id];
    case SORT_ATTRIBUTES: return (int64_t)esidx_win_attributes(db, id);
    default: return 0;
    }
}

/* ---------------------------------------------------------------- name order */

/* Order two folded strings: memcmp over the common prefix, then the length. That is
 * exactly what strcasecmp does in the C locale and therefore the same total order --
 * which matters, because cmp_ref.sh compares ordered result sets and a client pages
 * by OFFSET. */
static int cmp_folded(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la < lb ? la : lb;
    int r = n ? memcmp(a, b, n) : 0;
    if (r == 0) r = (la < lb) ? -1 : (la > lb) ? 1 : 0;
    return r;
}

/* Which keys compare raw and which compare folded. A path key does not fold, because
 * measured on an unfiltered /usr the fold costs more than the memcmp saves (see
 * fa_put_x); every other string key does, because it does. Recorded in one place so
 * the comparator and the extraction loop cannot disagree about which is which -- the
 * same reason leaf_mods() exists for the modifiers. */
static bool sort_key_is_path(sort_key_t k)
{
    return k == SORT_PATH || k == SORT_FILE_LIST_FILENAME;
}

/* Multi-level chain: primary key, then name, then id. The id tiebreak is what
 * makes the order total, so two runs of the same query always cut their pages at
 * the same boundary (ref D3).
 *
 * Three comparators for the primary key, and all three produce strcasecmp's order:
 *
 *   name + rank   an integer compare. Equal names share a rank on purpose, so they
 *                 fall through to the name tie-break and then the id exactly as they
 *                 did when the name itself was compared (design §10).
 *   path          strcasecmp, because the path is not folded (fa_put_x).
 *   everything    memcmp over names folded once per row on the way in.
 *
 * The tie-break is an integer compare too, on the display name's rank, which is the
 * same order by construction: the rank is dense over sorted position, and two names
 * that strcasecmp calls equal share a rank and so fall through to the id. It is worth
 * as much as the primary key on the four numeric keys, which is where the comparisons
 * actually go -- measured over an unfiltered /usr, they spent 19.5-28.6 ns per
 * comparison with a string tie-break and 10.3-12.1 ns with an integer one, against the
 * 9.9 ns floor a name sort already had (design §10, ./sortcmp.sh). The fallback is
 * there because the rank is built, and a build that could not allocate one has to
 * answer correctly anyway.
 *
 * The order is the contract, not an implementation detail: a client pages by OFFSET
 * and cmp_ref.sh compares ordered result sets, so a different order is a different
 * answer even when it looks like a reasonable one. */
static int cmp_rec(const void *pa, const void *pb, void *arg)
{
    const srec_t *a = pa, *b = pb;
    sort_ctx_t *sc = arg;
    int r = 0;

    sc->ncmp++;
    if (sc->key == SORT_NAME && sc->ranked) {
        r = (a->num < b->num) ? -1 : (a->num > b->num) ? 1 : 0;
    } else if (sort_key_is_numeric(sc->key)) {
        int64_t x = a->num, y = b->num;
        r = (x < y) ? -1 : (x > y) ? 1 : 0;
    } else if (sort_key_is_path(sc->key)) {
        r = strcasecmp(a->s, b->s);
    } else {
        r = cmp_folded(a->s, b->s);
    }
    if (r == 0) {
        if (sc->tie_rank)
            r = (a->nrank < b->nrank) ? -1 : (a->nrank > b->nrank);
        else
            r = sort_key_is_path(sc->key) ? strcasecmp(a->dn, b->dn)
                                          : cmp_folded(a->dn, b->dn);
    }
    if (r == 0) r = (a->id < b->id) ? -1 : (a->id > b->id);
    return sc->desc ? -r : r;
}

static int cmp_plain(const void *pa, const void *pb, void *arg)
{
    return cmp_rec(pa, pb, arg);
}

/* The scratch buffer is the only thing the context owns; the per-row path copies
 * belong to the rows array and are released with it. */
static void sc_done(sort_ctx_t *sc)
{
    free(sc->pathbuf);
    sc->pathbuf = NULL;
    sc->pathcap = 0;
}

/* --------------------------------------------------------------- the driver */

void qset_free(qset_t *s)
{
    if (!s) return;
    free(s->ids);
    memset(s, 0, sizeof(*s));
}

uint32_t qset_slice(const qset_t *s, uint32_t offset, uint32_t count, eid_t **out)
{
    *out = NULL;
    if (offset >= s->n) return 0;
    uint32_t cnt = (count == 0) ? (s->n - offset) : count;
    if (cnt > s->n - offset) cnt = s->n - offset;
    eid_t *r = malloc((cnt ? cnt : 1) * sizeof(eid_t));
    if (!r) return 0;
    memcpy(r, s->ids + offset, cnt * sizeof(eid_t));
    *out = r;
    return cnt;
}

/* Number the leaves so the chosen driver can be named in a log line and so the
 * protocol layer has a cheap identity for cache invalidation. One pass per
 * query, not one per node visit. */
static uint32_t number_leaves(ast_t *t, uint32_t next)
{
    if (!t) return next;
    if (t->kind == AST_TERM) t->uid = next++;
    next = number_leaves(t->a, next);
    next = number_leaves(t->b, next);
    next = number_leaves(t->list, next);
    next = number_leaves(t->sub, next);
    return next;
}

int qexec(const esidx_t *db, const ast_t *ast_in, const match_opts_t *mo,
          sort_spec_t sort, qset_t *out)
{
    memset(out, 0, sizeof(*out));
    out->driver = UINT32_MAX;
    if (!db || db->et.count == 0) return 0;

    /* the optimiser annotates, so it needs a mutable root */
    ast_t *ast = (ast_t *)ast_in;
    uint32_t nleaf = number_leaves(ast, 0);

    qctx_t c;
    memset(&c, 0, sizeof(c));
    c.db = db;
    c.mo = mo;
    c.n = db->et.count;

    uint64_t t_plan0 = ts_us();

    /* ---- steps 1 and 2: driver selection, then candidate seeding ---- */
    if (ast) {
        uint32_t best = UINT32_MAX;
        const ast_t *leaf = NULL;
        out->leaf_cnt = nleaf;
        best = pick_driver(&c, ast, &best, &leaf, 0);
        if (leaf) {
            /* The matchers intersect with whatever set they are handed, so the
             * seed has to start as "everything" and be narrowed by the driver --
             * which also makes re-running the driver during the tree walk
             * idempotent. */
            if (bs_alloc_live(&c, &c.cand) != 0) { qctx_done(&c); return -1; }
            if (eval_leaf(&c, leaf, &c.cand) != 0) { qctx_done(&c); return -1; }
            out->seed = bs_count(&c.cand);
            out->driver = leaf->uid;
            char fn[64] = "", val[256] = "";
            if (leaf->fn && leaf->fn[0]) snprintf(fn, sizeof(fn), "%s:", leaf->fn);
            snprintf(val, sizeof(val), "%s", leaf->val ? leaf->val : "");
            LOGD("plan: driver=%s%s estimated=%u actual=%u of %u entries (%u leaves)",
                 fn, val, best, out->seed, c.n, nleaf);
        } else {
            if (bs_alloc_live(&c, &c.cand) != 0) { qctx_done(&c); return -1; }
            LOGD("plan: no index-backed leaf; all %u entries are candidates", bs_count(&c.cand));
        }
    } else {
        if (bs_alloc_live(&c, &c.cand) != 0) { qctx_done(&c); return -1; }
    }
    c.cand_count = bs_count(&c.cand);
    out->t_plan_us = ts_us() - t_plan0;

    /* ---- step 3: evaluate the tree against the candidate set ---- */
    uint64_t t_eval0 = ts_us();
    bitset_t set;
    if (bs_alloc(&c, &set) != 0) { qctx_done(&c); return -1; }
    memcpy(set.w, c.cand.w, ((size_t)(c.n + 63) / 64) * sizeof(uint64_t));
    if (ast && eval_node(&c, ast, &set) != 0) {
        bs_free(&set); qctx_done(&c); return -1;
    }

    /* ---- step 4: the FILTER_* second stage ---- */
    if (mo) apply_filter(&c, &set);

    uint32_t total = bs_count(&set);
    uint32_t ndir = 0, nfile = 0;
    for (uint32_t i = bs_next(&set, 0); i < c.n; i = bs_next(&set, i + 1)) {
        if (db->et.flags[i] & EF_DIR) ndir++; else nfile++;
    }
    out->t_eval_us = ts_us() - t_eval0;

    eid_t *ids = malloc((total ? total : 1) * sizeof(eid_t));
    if (!ids) { bs_free(&set); qctx_done(&c); return -1; }
    out->ids = ids;

    if (total == 0) {
        bs_free(&set); qctx_done(&c);
        return 0;
    }

    /* ---- step 5: sort ----
     *
     * Always a full sort, never a bounded heap. The reason is RESULT_COUNT: the
     * ETP client uses it to decide whether to offer "load more", so it has to be
     * the size of the whole matched set (etp_server.c:5190) and every id must be
     * collected anyway. A heap would replace the O(n log n) compare stage with
     * O(n log k) -- real, but on the key this client actually sorts by the row
     * set is already a directory listing, and it would break the result cache,
     * which re-slices this array on a new OFFSET without re-running the query.
     * design.md §6.2 step 4 is therefore deferred with this reasoning recorded
     * rather than implemented on speculation.
     *
     * What made the sort expensive was not the shape of it but the comparator: see
     * ascii_fold. The stage is timed in two halves anyway, because "key extraction"
     * and "comparison" are different fixes and reporting only their sum is what let
     * a strcasecmp stay in here for this long. */
    uint64_t t_sort0 = ts_us();
    sort_ctx_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.db = db;
    sc.key = sort.key;
    sc.desc = sort.desc;
    if (sc.key == SORT_RECENTLY_CHANGED)
        LOGD("sort: no date_recently_changed column; ordering by mtime instead");

    srec_t *rows = malloc((size_t)total * sizeof(srec_t));
    foldarena_t fa;
    fa_init(&fa);
    if (!rows) { bs_free(&set); qctx_done(&c); sc_done(&sc); fa_done(&fa); return -1; }

    /* Extract and fold, once per row. `dn` is a pointer into `s` wherever the key
     * already contains the name, so only the extension and numeric keys pay for a
     * second copy -- and nothing pays for one at all while the rank exists, because
     * the tie-break is an integer compare then and never reads a string. A name sort
     * pays for neither copy: the rank is already in the index. */
    const int numeric = sort_key_is_numeric(sc.key);
    const bool key_is_path = sort_key_is_path(sc.key);
    sc.ranked = (sc.key == SORT_NAME && db->name_rank != NULL);
    sc.tie_rank = (db->name_rank != NULL);
    uint64_t t_key0 = ts_us();
    uint32_t ri = 0;
    for (uint32_t i = bs_next(&set, 0); i < c.n; i = bs_next(&set, i + 1)) {
        const char *name = display_name_of(db, i);
        rows[ri].id = i;
        rows[ri].s  = "";
        rows[ri].dn = "";
        rows[ri].nrank = sc.tie_rank ? esidx_name_rank(db, i) : 0;
        rows[ri].num = sc.ranked ? (int64_t)rows[ri].nrank : sort_number(&sc, i);
        if (!sc.ranked) {
            if (key_is_path) {
                /* A path gets its own exact-sized copy and nothing else: no arena, no
                 * fold. Both were tried and both measured worse over an unfiltered
                 * /usr -- 190 ms folded into the arena, 218 ms unfolded into it,
                 * against 144 ms for the plain copy this replaced. The arena's block
                 * boundaries scatter 26 MB of path strings in a way a sequential
                 * allocation does not, and a path sort's primary key is unique so it
                 * almost never reaches the tie-break. Kept as its own branch because
                 * that is a measurement, not a preference. */
                const char *key = sort_string(&sc, i);
                size_t n = strlen(key) + 1;
                char *own = malloc(n);
                if (own) { memcpy(own, key, n); rows[ri].s = own; }
                if (!sc.tie_rank) {
                    const char *tail = strrchr(rows[ri].s, '/');
                    rows[ri].dn = tail ? tail + 1 : rows[ri].s;
                }
            } else {
                if (!numeric) rows[ri].s = fa_put(&fa, sort_string(&sc, i));
                if (!sc.tie_rank) rows[ri].dn = fa_put(&fa, name);
            }
        }
        ri++;
    }
    out->t_key_us = ts_us() - t_key0;

    if (total > 1) qsort_r(rows, total, sizeof(srec_t), cmp_plain, &sc);
    for (uint32_t i = 0; i < total; i++) ids[i] = rows[i].id;

    out->t_sort_us = ts_us() - t_sort0;
    out->sort_ncmp = sc.ncmp;
    out->n = total;
    out->n_dir = ndir;
    out->n_file = nfile;

    if (key_is_path) for (uint32_t i = 0; i < total; i++) free((char *)rows[i].s);
    free(rows);
    /* Only the path key owns its strings -- one exact-sized malloc per row, from the
     * branch above. Everything else is either in the arena, which fa_done() releases,
     * or a pointer into the name pool, which is the index. */
    fa_done(&fa);
    bs_free(&set);
    qctx_done(&c);
    sc_done(&sc);

    LOGD("exec: matched=%u (dirs=%u files=%u) of %u | plan=%.3f eval=%.3f "
         "sort=%.3f ms (key %.3f ms, %llu comparisons, %.1f ns each)",
         total, ndir, nfile, c.n,
         (double)out->t_plan_us / 1000.0, (double)out->t_eval_us / 1000.0,
         (double)out->t_sort_us / 1000.0, (double)out->t_key_us / 1000.0,
         (unsigned long long)out->sort_ncmp,
         out->sort_ncmp ? (double)(out->t_sort_us - out->t_key_us) * 1000.0 /
                          (double)out->sort_ncmp : 0.0);
    return 0;
}
