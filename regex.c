/* A small backtracking regular-expression engine (design §2 L2, risk 3).
 *
 * Why not PCRE2: the language we must accept is narrow. Everything's own
 * documentation, and every pattern the ETP client actually sends, uses
 * literals, `.`, character classes, anchors, groups, alternation and the three
 * quantifiers. Pulling in libpcre2 would add a shared library to a project whose
 * stated deployment goal is "any Linux box" (decision D5), and `regex:` is a full
 * scan either way -- it is never index-backed, so a faster engine would not
 * change the asymptotics.
 *
 * Supported:
 *     .            any byte except newline
 *     ^  $         anchors (^ also matches just after a newline, like Everything)
 *     *  +  ?      greedy; a trailing `?` makes them lazy
 *     [abc] [^a-z] [\]] [a-z] [\d\w\s\D\W\S]   classes, ranges, negation
 *     (...)        groups; (?:...) non-capturing
 *     |            alternation
 *     \xHH \n \t \r \f \v \\ \. ...   escapes
 *     (?i)         case-insensitive from that point on
 *
 * Deliberately absent: {n,m} repetition, backreferences, lookaround and Unicode
 * properties. An unsupported construct is a compile error and the term then
 * matches nothing -- the same outcome Everything gives an invalid pattern, and far
 * better than silently matching something else.
 *
 * Implementation: parse to a node tree, emit a flat instruction array from it, run
 * that with a recursive backtracking VM in which the continuation is part of the
 * recursion (run(pc, sp)). That last detail is the whole reason it works: a
 * matcher that only reports "did this node match, and where" cannot backtrack out
 * of a repetition once the rest of the pattern fails -- and that is the common
 * case, since `^b.*conf$` against `b.conf` requires `.*` to give characters back.
 *
 * A step budget bounds a pathological pattern. This matters more here than in a
 * desktop app: an ETP client can send any pattern it likes over the network, and
 * one `(a+)+b` must not be able to wedge the daemon.
 */

#include "syntax.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* ------------------------------------------------------------------- tree */

typedef enum {
    N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL, N_CAT, N_ALT, N_REP
} ntype_t;

typedef struct rnode rnode_t;

struct rnode {
    ntype_t      t;
    unsigned char ch;        /* N_CHAR */
    unsigned char cls[32];   /* N_CLASS bitmap */
    int          fold;       /* this node was inside a (?i) scope */
    rnode_t     *a, *b;      /* N_CAT / N_ALT / N_REP */
    int          min, max;   /* N_REP; max < 0 means unbounded */
    int          greedy;
};

/* --------------------------------------------------------------- program */

typedef enum {
    I_CHAR, I_ANY, I_CLASS, I_BOL, I_EOL, I_JMP, I_SPLIT, I_MATCH
} iop_t;

typedef struct {
    iop_t         op;
    unsigned char ch;
    int           fold;
    unsigned char cls[32];
    int           x, y;
} inst_t;

#define RE_MAX_NODES 512
#define RE_MAX_INST  4096
#define RE_MAX_PARSE_DEPTH 64
/* Bounds total backtracking work: comfortably above any pattern that matches a
 * path, and tripped by the exponential cases. */
#define RE_MAX_STEPS   2000000
#define RE_MAX_VM_DEPTH 400

struct regex {
    rnode_t *pool;
    size_t   npool, cappool;
    rnode_t *root;
    inst_t  *prog;
    int      nprog, capprog;
    char     err[96];
};

static const char *g_err;

const char *re_error(void) { return g_err ? g_err : "ok"; }

/* --------------------------------------------------------------- compiler */

typedef struct {
    struct regex *re;
    const char   *base, *p;
    int           depth;
    int           failed;
    int           fold;      /* current (?i) scope */
} rctx_t;

static int fail(rctx_t *c, const char *msg)
{
    if (!c->failed) {
        c->failed = 1;
        snprintf(c->re->err, sizeof(c->re->err), "%s at offset %d", msg,
                 (int)(c->p - c->base));
        g_err = c->re->err;
    }
    return -1;
}

static rnode_t *rn(rctx_t *c)
{
    if (c->re->npool >= RE_MAX_NODES) { fail(c, "pattern too large"); return NULL; }
    if (c->re->npool == c->re->cappool) {
        size_t nc = c->re->cappool ? c->re->cappool * 2 : 32;
        rnode_t *np = realloc(c->re->pool, nc * sizeof(rnode_t));
        if (!np) { fail(c, "out of memory"); return NULL; }
        c->re->pool = np; c->re->cappool = nc;
    }
    rnode_t *n = &c->re->pool[c->re->npool++];
    memset(n, 0, sizeof(*n));
    n->max = -1;
    n->greedy = 1;
    return n;
}

static void cls_set(unsigned char *cls, unsigned char ch) { cls[ch >> 3] |= (unsigned char)(1u << (ch & 7)); }
static int  cls_get(const unsigned char *cls, unsigned char ch) { return (cls[ch >> 3] >> (ch & 7)) & 1; }

/* one escape -> a literal byte */
static int unescape(rctx_t *c, unsigned char *out)
{
    char e = *c->p++;
    switch (e) {
    case 'n': *out = '\n'; return 0;
    case 't': *out = '\t'; return 0;
    case 'r': *out = '\r'; return 0;
    case 'f': *out = '\f'; return 0;
    case 'v': *out = '\v'; return 0;
    case 'a': *out = '\a'; return 0;
    case 'e': *out = 27;    return 0;
    case '0': *out = '\0'; return 0;
    case 'x': {
        int hi = -1, lo = -1, got = 0;
        while (got < 2 && isxdigit((unsigned char)*c->p)) {
            char d = *c->p++;
            int v = isdigit((unsigned char)d) ? d - '0'
                   : (tolower((unsigned char)d) - 'a' + 10);
            if (hi < 0) hi = v; else lo = v;
            got++;
        }
        if (!got) return fail(c, "\\x needs hex digits");
        *out = (unsigned char)((hi << 4) | (lo < 0 ? 0 : lo));
        return 0;
    }
    default: *out = (unsigned char)e; return 0;
    }
}

static rnode_t *parse_alt(rctx_t *c);

/* Parse a character class body; the '[' is already consumed. */
static int parse_class(rctx_t *c, rnode_t *n)
{
    int neg = 0;
    if (*c->p == '^') { neg = 1; c->p++; }

    unsigned char bits[32] = {0};
    int first = 1;
    while (*c->p && (*c->p != ']' || first)) {
        first = 0;
        /* the shorthands \d \w \s and their negations */
        if (c->p[0] == '\\' && c->p[1] && strchr("dDwWsS", c->p[1])) {
            char k = (char)tolower((unsigned char)c->p[1]);
            if (k == 'd') { int i; for (i = '0'; i <= '9'; i++) cls_set(bits, (unsigned char)i); }
            else if (k == 'w') {
                int i;
                for (i = 'a'; i <= 'z'; i++) cls_set(bits, (unsigned char)i);
                for (i = 'A'; i <= 'Z'; i++) cls_set(bits, (unsigned char)i);
                for (i = '0'; i <= '9'; i++) cls_set(bits, (unsigned char)i);
                cls_set(bits, '_');
            } else {
                cls_set(bits, ' ');  cls_set(bits, '\t'); cls_set(bits, '\n');
                cls_set(bits, '\r'); cls_set(bits, '\f'); cls_set(bits, '\v');
            }
            if (isupper((unsigned char)c->p[1])) {
                unsigned char inv[32];
                int i;
                for (i = 0; i < 32; i++) inv[i] = (unsigned char)~bits[i];
                memcpy(bits, inv, 32);
            }
            c->p += 2;
            continue;
        }
        unsigned char lo;
        if (*c->p == '\\') {
            c->p++;
            if (unescape(c, &lo) != 0) return -1;
        } else {
            lo = (unsigned char)*c->p++;
        }
        if (*c->p == '-' && c->p[1] && c->p[1] != ']') {
            c->p++;
            unsigned char hi;
            if (*c->p == '\\') { c->p++; if (unescape(c, &hi) != 0) return -1; }
            else hi = (unsigned char)*c->p++;
            if (hi < lo) return fail(c, "reversed range in a character class");
            for (int i = lo; i <= (int)hi; i++) cls_set(bits, (unsigned char)i);
        } else {
            cls_set(bits, lo);
        }
    }
    if (*c->p != ']') return fail(c, "unterminated character class");
    c->p++;
    if (neg) { int i; for (i = 0; i < 32; i++) bits[i] = (unsigned char)~bits[i]; }
    memcpy(n->cls, bits, 32);
    n->t = N_CLASS;
    n->fold = c->fold;
    return 0;
}

/* A group. Returns 1 when a `(?i)` scope opener was consumed (nothing emitted),
 * 0 for an ordinary or (?:...) group, -1 on error. */
static int parse_group(rctx_t *c)
{
    c->p++;                                   /* past '(' */
    if (*c->p != '?') return 0;

    c->p++;
    if (*c->p == 'i') {
        c->fold = 1;
        while (*c->p && *c->p != ')') c->p++;
        if (*c->p != ')') { fail(c, "unterminated (?i)"); return -1; }
        c->p++;
        return 1;
    }
    if (*c->p == ':') { c->p++; return 0; }
    fail(c, "unsupported (?...) group -- only (?:) and (?i) are understood");
    return -1;
}

/* One atom plus an optional quantifier. */
static rnode_t *parse_atom(rctx_t *c)
{
    char ch = *c->p;

    if (ch == '\0') { fail(c, "unexpected end of pattern"); return NULL; }

    rnode_t *n;

    if (ch == '(') {
        if (++c->depth > RE_MAX_PARSE_DEPTH) { fail(c, "pattern nested too deeply"); return NULL; }
        int scope = parse_group(c);
        if (scope < 0) { c->depth--; return NULL; }
        if (scope == 0) {
            n = parse_alt(c);
            c->depth--;
            if (!n) return NULL;
            if (*c->p != ')') { fail(c, "missing ')'"); return NULL; }
            c->p++;
            goto quantifier;
        }
        c->depth--;
        n = rn(c);
        if (!n) return NULL;
        n->t = N_CHAR;              /* a bare `(?i)` matches the empty string */
        n->ch = '\0';
        goto quantifier;
    }
    if (ch == ')')  { fail(c, "unmatched ')'"); return NULL; }
    if (ch == '*' || ch == '+' || ch == '?') {
        fail(c, "quantifier with nothing to repeat");
        return NULL;
    }
    if (ch == '{') {
        fail(c, "{n,m} repetition is not supported");
        return NULL;
    }

    if (ch == '[') {
        c->p++;
        n = rn(c);
        if (!n) return NULL;
        if (parse_class(c, n) != 0) return NULL;
        goto quantifier;
    }
    if (ch == '.') { c->p++; n = rn(c); if (!n) return NULL; n->t = N_ANY; goto quantifier; }
    if (ch == '^') { c->p++; n = rn(c); if (!n) return NULL; n->t = N_BOL; goto quantifier; }
    if (ch == '$') { c->p++; n = rn(c); if (!n) return NULL; n->t = N_EOL; goto quantifier; }

    n = rn(c);
    if (!n) return NULL;
    if (ch == '\\') {
        c->p++;
        if (unescape(c, &n->ch) != 0) return NULL;
    } else {
        n->ch = (unsigned char)ch;
        c->p++;
    }
    n->t = N_CHAR;
    n->fold = c->fold;

quantifier:
    if (*c->p != '*' && *c->p != '+' && *c->p != '?') return n;
    {
        char q = *c->p++;
        int greedy = 1;
        if (*c->p == '?') { greedy = 0; c->p++; }
        rnode_t *rep = rn(c);
        if (!rep) return NULL;
        rep->t = N_REP;
        rep->a = n;
        rep->min = (q == '+') ? 1 : 0;
        rep->max = (q == '?') ? 1 : -1;
        rep->greedy = greedy;
        return rep;
    }
}

static rnode_t *parse_cat(rctx_t *c)
{
    rnode_t *left = NULL;
    while (*c->p && *c->p != '|' && *c->p != ')') {
        rnode_t *a = parse_atom(c);
        if (!a) { return NULL; }
        if (!left) { left = a; continue; }
        rnode_t *cat = rn(c);
        if (!cat) return NULL;
        cat->t = N_CAT;
        cat->a = left;
        cat->b = a;
        left = cat;
    }
    /* an empty branch is legitimate: `a|` matches "a" or the empty string */
    if (!left) {
        left = rn(c);
        if (!left) return NULL;
        left->t = N_CHAR;
        left->ch = '\0';
    }
    return left;
}

static rnode_t *parse_alt(rctx_t *c)
{
    rnode_t *left = parse_cat(c);
    if (!left) return NULL;
    while (*c->p == '|') {
        c->p++;
        rnode_t *right = parse_cat(c);
        if (!right) return NULL;
        rnode_t *alt = rn(c);
        if (!alt) return NULL;
        alt->t = N_ALT;
        alt->a = left;
        alt->b = right;
        left = alt;
    }
    return left;
}

/* ----------------------------------------------------------------- program */

static int emit(struct regex *r, iop_t op)
{
    if (r->nprog >= RE_MAX_INST) {
        snprintf(r->err, sizeof(r->err), "pattern too large");
        return -1;
    }
    if (r->nprog == r->capprog) {
        int nc = r->capprog ? r->capprog * 2 : 32;
        inst_t *np = realloc(r->prog, (size_t)nc * sizeof(inst_t));
        if (!np) { snprintf(r->err, sizeof(r->err), "out of memory"); return -1; }
        r->prog = np;
        r->capprog = nc;
    }
    inst_t *in = &r->prog[r->nprog];
    memset(in, 0, sizeof(*in));
    in->op = op;
    return r->nprog++;
}

static int emit_node(struct regex *r, const rnode_t *n)
{
    switch (n->t) {
    case N_CHAR: {
        int at = emit(r, I_CHAR);
        if (at < 0) return -1;
        r->prog[at].ch = n->ch;
        r->prog[at].fold = n->fold;
        return 0;
    }
    case N_ANY:
        return emit(r, I_ANY) < 0 ? -1 : 0;
    case N_BOL:
        return emit(r, I_BOL) < 0 ? -1 : 0;
    case N_EOL:
        return emit(r, I_EOL) < 0 ? -1 : 0;
    case N_CLASS: {
        int at = emit(r, I_CLASS);
        if (at < 0) return -1;
        memcpy(r->prog[at].cls, n->cls, 32);
        r->prog[at].fold = n->fold;
        return 0;
    }
    case N_CAT:
        if (emit_node(r, n->a) != 0) return -1;
        return emit_node(r, n->b);

    case N_ALT: {
        int split = emit(r, I_SPLIT);
        if (split < 0) return -1;
        int a_start = r->nprog;
        if (emit_node(r, n->a) != 0) return -1;
        int jmp = emit(r, I_JMP);
        if (jmp < 0) return -1;
        int b_start = r->nprog;
        if (emit_node(r, n->b) != 0) return -1;
        int end = r->nprog;
        r->prog[split].x = a_start;
        r->prog[split].y = b_start;
        r->prog[jmp].x = end;
        return 0;
    }

    case N_REP: {
        if (n->max == 0) return 0;                 /* x? that never matched: skip */
        if (n->min == 1 && n->max < 0) {
            /* x+ : body; SPLIT body, end */
            int body = r->nprog;
            if (emit_node(r, n->a) != 0) return -1;
            int split = emit(r, I_SPLIT);
            if (split < 0) return -1;
            int end = r->nprog;
            if (n->greedy) { r->prog[split].x = body; r->prog[split].y = end; }
            else           { r->prog[split].x = end;  r->prog[split].y = body; }
            return 0;
        }
        if (n->min == 0 && n->max == 1) {
            /* x? : SPLIT body, end ; body ; end */
            int split = emit(r, I_SPLIT);
            if (split < 0) return -1;
            int body = r->nprog;
            if (emit_node(r, n->a) != 0) return -1;
            int end = r->nprog;
            if (n->greedy) { r->prog[split].x = body; r->prog[split].y = end; }
            else           { r->prog[split].x = end;  r->prog[split].y = body; }
            return 0;
        }
        /* x* : SPLIT body, end ; body ; JMP split ; end */
        {
            int split = emit(r, I_SPLIT);
            if (split < 0) return -1;
            int body = r->nprog;
            if (emit_node(r, n->a) != 0) return -1;
            int jmp = emit(r, I_JMP);
            if (jmp < 0) return -1;
            int end = r->nprog;
            r->prog[jmp].x = split;
            if (n->greedy) { r->prog[split].x = body; r->prog[split].y = end; }
            else           { r->prog[split].x = end;  r->prog[split].y = body; }
            return 0;
        }
    }
    }
    return -1;
}

/* -------------------------------------------------------------- public API */

re_t *re_compile(const char *pat, int icase)
{
    g_err = NULL;
    if (!pat) { g_err = "null pattern"; return NULL; }

    re_t *r = calloc(1, sizeof(re_t));
    if (!r) { g_err = "out of memory"; return NULL; }

    rctx_t c = { r, pat, pat, 0, 0, icase };
    r->root = parse_alt(&c);
    if (!r->root || c.failed) goto bad;
    if (*c.p) {
        snprintf(r->err, sizeof(r->err), "trailing '%c' at offset %d",
                 *c.p, (int)(c.p - c.base));
        g_err = r->err;
        goto bad;
    }
    if (emit_node(r, r->root) != 0) goto bad;
    if (emit(r, I_MATCH) < 0) goto bad;
    return r;

bad:
    if (r->err[0]) g_err = r->err;
    else g_err = "invalid pattern";
    re_free(r);
    return NULL;
}

void re_free(re_t *re)
{
    if (!re) return;
    free(re->pool);
    free(re->prog);
    free(re);
}

/* ----------------------------------------------------------------- matcher */

typedef struct {
    const char *s;
    const char *end;
    long        steps;
    int         depth;
} vm_t;

static int vm_run(vm_t *m, const re_t *re, int pc, const char *sp)
{
    if (++m->steps > RE_MAX_STEPS) return 0;
    if (++m->depth > RE_MAX_VM_DEPTH) { m->depth--; return 0; }

    for (;;) {
        const inst_t *in = &re->prog[pc];
        switch (in->op) {
        case I_MATCH:
            m->end = sp;
            m->depth--;
            return 1;
        case I_CHAR: {
            unsigned char c = (unsigned char)*sp;
            if (!*sp) { m->depth--; return 0; }
            if (c != in->ch) {
                int fold = in->fold;
                if (!fold || tolower(c) != tolower(in->ch)) { m->depth--; return 0; }
            }
            sp++; pc++;
            continue;
        }
        case I_ANY:
            if (!*sp || *sp == '\n') { m->depth--; return 0; }
            sp++; pc++;
            continue;
        case I_CLASS: {
            if (!*sp) { m->depth--; return 0; }
            unsigned char c = (unsigned char)*sp;
            int hit = cls_get(in->cls, c);
            if (!hit && in->fold)
                hit = cls_get(in->cls, (unsigned char)toupper(c));
            if (!hit) { m->depth--; return 0; }
            sp++; pc++;
            continue;
        }
        case I_BOL:
            if (sp != m->s && sp[-1] != '\n') { m->depth--; return 0; }
            pc++;
            continue;
        case I_EOL:
            if (*sp != '\0' && *sp != '\n') { m->depth--; return 0; }
            pc++;
            continue;
        case I_JMP:
            pc = in->x;
            continue;
        case I_SPLIT:
            /* try the preferred branch first; if the continuation fails, resume
             * at the other one -- this is where backtracking happens */
            if (vm_run(m, re, in->x, sp)) { m->depth--; return 1; }
            pc = in->y;
            continue;
        }
    }
}

int re_match(const re_t *re, const char *s)
{
    if (!re || !s || re->nprog == 0) return 0;
    /* Unanchored: try every start offset, like Everything's substring regex. */
    for (const char *start = s;; start++) {
        vm_t m = { s, NULL, 0, 0 };
        if (vm_run(&m, re, 0, start)) return 1;
        if (!*start) break;
    }
    return 0;
}

/* --------------------------------------------------------------- wildcards */

/* Everything's wildcard syntax (L65-72): `*` any run, `?` one character, `#` one
 * digit, `[abc]` / `[!abc]` a set, `\` an escape. `*` stops at a path separator
 * and `**` does not. A pattern containing a wildcard applies to the whole
 * filename, which is why this is anchored at both ends. */

static int wc_eq(char a, char b, int nocase)
{
    if (a == b) return 1;
    return nocase && tolower((unsigned char)a) == tolower((unsigned char)b);
}

static int wc_match(const char *pat, const char *s, int nocase)
{
    while (*pat) {
        if (pat[0] == '*' && pat[1] == '*') {
            const char *np = pat + 2;
            /* consume greedily across separators, so a leading double star can
             * also match the empty prefix */
            for (;;) {
                if (wc_match(np, s, nocase)) return 1;
                if (!*s) return 0;
                s++;
            }
        }
        if (pat[0] == '*') {
            const char *np = pat + 1;
            if (wc_match(np, s, nocase)) return 1;
            for (; *s; s++) {
                if (*s == '/' || *s == '\\') return 0;
                if (wc_match(np, s, nocase)) return 1;
            }
            /* The loop stops on the terminating NUL without ever offering it as a
             * position, so a `*` that has to swallow the tail of the name -- and a
             * `*` may match nothing, so the tail may be empty -- was unreachable.
             * `*conf*` happened to work because its literal ran to the end of the
             * name, and `*on*` did not, because `on` does not: `b.conf` matched
             * `conf*` against "" and `on*` against "f" was never tried. */
            return wc_match(np, s, nocase);
        }
        if (!*s) {
            /* a trailing separator in the pattern is optional, so both `foo\`
             * and `foo` match the directory `foo` */
            if ((*pat == '/' || *pat == '\\') && !pat[1]) return 1;
            return 0;
        }
        if (pat[0] == '?') {
            if (*s == '/' || *s == '\\') return 0;
            pat++; s++;
            continue;
        }
        if (pat[0] == '#') {
            if (!isdigit((unsigned char)*s)) return 0;
            pat++; s++;
            continue;
        }
        if (pat[0] == '[') {
            const char *p = pat + 1;
            int neg = 0;
            if (*p == '!' || *p == '^') { neg = 1; p++; }
            int hit = 0, first = 1;
            while (*p && (*p != ']' || first)) {
                first = 0;
                char lo = *p;
                if (lo == '\\' && p[1]) { p++; lo = *p; }
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    unsigned char u = (unsigned char)*s;
                    if (u >= (unsigned char)lo && u <= (unsigned char)p[2]) hit = 1;
                    p += 3;
                    continue;
                }
                p++;
                if (wc_eq(lo, *s, nocase)) hit = 1;
            }
            if (*p != ']') return 0;             /* unterminated: no match */
            p++;
            if (hit == neg) return 0;
            pat = p;
            s++;
            continue;
        }
        if (pat[0] == '\\' && pat[1]) {
            pat++;
            if (!wc_eq(*pat, *s, nocase)) return 0;
            pat++; s++;
            continue;
        }
        if (!wc_eq(*pat, *s, nocase)) return 0;
        pat++; s++;
    }
    return *s == '\0';
}

int wildcard_match(const char *pat, const char *s, int nocase)
{
    if (!pat || !s) return 0;
    return wc_match(pat, s, nocase);
}

/* The same match, unanchored: find the pattern anywhere in the subject.
 *
 * Everything anchors a wildcard to the whole *filename* (everything-syntax.md
 * L35), which is what wildcard_match() does, but a `*` in a *path* finds its
 * pattern anywhere in the path instead. Measured against voidtools' server on :21
 * over one directory: "esidx", a separator and a star answers 38 there, and 38 is
 * exactly what `find <dir>/esidx -mindepth 1 -maxdepth 1 | wc -l` says -- the
 * direct children, not the subtree and not zero. An anchored whole-path match
 * could not produce that: the pattern cannot consume a path that starts at
 * `/mnt/c/`.
 *
 * The candidate start offsets are the ends of the path, the beginnings of its
 * components, and the separators themselves -- a `*` may match nothing, so
 * `path:` + a star + a separator + "main.c" finds the separator that ends
 * `esidx/` even though the pattern begins with a component boundary the reference
 * does not offer. What it may not do is begin *inside* a component: the reference
 * answers 38 for "esidx", a separator and a star, and 0 for the same pattern with
 * the leading `e` dropped. The `*` in wc_match() still refuses to cross a
 * separator, which is what holds that count at the direct children. */
int wildcard_match_in(const char *pat, const char *s, int nocase)
{
    if (!pat || !s) return 0;
    for (const char *start = s;; start++) {
        int sep = (*start == '/' || *start == '\\' ||
                   start == s || start[-1] == '/' || start[-1] == '\\');
        if (sep && wc_match(pat, start, nocase)) return 1;
        if (!*start) break;
    }
    return 0;
}

int wildcard_present(const char *pat)
{
    return pat && strpbrk(pat, "*?#[]") != NULL;
}
