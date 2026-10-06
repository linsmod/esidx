/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
/* Lexer for the Everything search language (everything-syntax.md L2-72).
 *
 * Design notes
 * ------------
 * Everything has no separate token classes: a "term" is whatever the user typed
 * up to the next operator. That is convenient until a value legitimately
 * contains an operator character, which happens constantly:
 *
 *     size:<1mb            '<' is "less than", not grouping
 *     dm:>7d               '>' is "greater than"
 *     dc:!=2024            '!' is "not equal"
 *     content:<abc 123>    bracketed value list, spaces inside
 *     path:regex:[A-Z]:\\  brackets belong to the pattern
 *
 * So the lexer carries two pieces of state -- `after_colon` (the term so far
 * ends with ':', so the next character is value, not operator) and a bracket
 * depth. Inside a bracket, whitespace no longer terminates a term and the
 * closing bracket is consumed into the value. Everywhere else a bracket is
 * grouping.
 *
 * Character entities (`&sp;`, `&vert;`, `lt:`, …) are decoded here, once, so no
 * later layer has to know they exist (L29-48).
 */

#include "syntax.h"
#include "lexer.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

/* ------------------------------------------------------------ token stream */

void tokstream_free(tokstream_t *ts)
{
    for (size_t i = 0; i < ts->n; i++) free(ts->v[i].text);
    free(ts->v);
    ts->v = NULL; ts->n = ts->cap = 0;
}

static token_t *ts_push(tokstream_t *ts, tok_kind_t k)
{
    if (ts->n == ts->cap) {
        size_t nc = ts->cap ? ts->cap * 2 : 16;
        token_t *nv = realloc(ts->v, nc * sizeof(token_t));
        if (!nv) return NULL;
        ts->v = nv; ts->cap = nc;
    }
    token_t *t = &ts->v[ts->n++];
    memset(t, 0, sizeof(*t));
    t->kind = k;
    return t;
}

/* ------------------------------------------------------------ byte buffer */

typedef struct { char *p; size_t n, cap; } sbuf_t;

static int sb_putc(sbuf_t *b, char c)
{
    if (b->n + 2 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        char *np = realloc(b->p, nc);
        if (!np) return -1;
        b->p = np; b->cap = nc;
    }
    b->p[b->n++] = c;
    return 0;
}

/* Move the pending buffer into a fresh token and reset it. */
static int flush_term(tokstream_t *ts, sbuf_t *b, int had_space)
{
    if (b->n == 0) return 0;
    token_t *t = ts_push(ts, T_TERM);
    if (!t) return -1;
    b->p[b->n] = '\0';
    t->text = b->p;
    t->had_space = had_space;
    b->p = NULL; b->n = b->cap = 0;
    return 0;
}

static int push_op(tokstream_t *ts, tok_kind_t k, int had_space)
{
    token_t *t = ts_push(ts, k);
    if (!t) return -1;
    t->had_space = had_space;
    return 0;
}

/* ------------------------------------------------------ character entities */

/* Decode one entity (everything-syntax.md L29-48). Returns bytes consumed, or 0
 * if `s` does not start an entity. */
static size_t decode_entity(const char *s, sbuf_t *out)
{
    if (!strncmp(s, "&sp;", 4))    { sb_putc(out, ' ');  return 4; }
    if (!strncmp(s, "&vert;", 6))  { sb_putc(out, '|');  return 6; }
    if (!strncmp(s, "&excl;", 6))  { sb_putc(out, '!');  return 6; }
    if (!strncmp(s, "lt:", 3))     { sb_putc(out, '<');  return 3; }
    if (!strncmp(s, "gt:", 3))     { sb_putc(out, '>');  return 3; }
    if (!strncmp(s, "quot:", 5))   { sb_putc(out, '"');  return 5; }
    if (!strncmp(s, "amp;", 4))    { sb_putc(out, '&');  return 4; }
    if (s[0] == '#') {
        const char *q = s + 1;
        int base = 10;
        if (*q == 'x' || *q == 'X') { base = 16; q++; }
        if (isdigit((unsigned char)*q)) {
            char *end = NULL;
            unsigned long v = strtoul(q, &end, base);
            if (end && end > q) {
                /* the language documents these as Unicode code points */
                if (v < 0x80) {
                    sb_putc(out, (char)v);
                } else if (v < 0x800) {
                    sb_putc(out, (char)(0xC0 | (v >> 6)));
                    sb_putc(out, (char)(0x80 | (v & 0x3F)));
                } else {
                    sb_putc(out, (char)(0xE0 | (v >> 12)));
                    sb_putc(out, (char)(0x80 | ((v >> 6) & 0x3F)));
                    sb_putc(out, (char)(0x80 | (v & 0x3F)));
                }
                return (size_t)(end - s);
            }
        }
    }
    return 0;
}

/* --------------------------------------------------------------- the lexer */

/* Tokenise `s`. Returns 0 on success, -1 on allocation failure or an
 * unterminated quote.
 *
 * Two bracket kinds, and conflating them is the bug this function is written
 * around:
 *
 *   vbracket  a `<...>` that opened immediately after `fn:` -- a *value* list.
 *             Inside it, whitespace and `|` are data (everything-syntax.md
 *             L162-163: `ext:<a b>` is an AND list, `ext:<a|b>` an OR list).
 *
 *   grouping  a `<...>` or `(...)` anywhere else. Inside it, whitespace still
 *             separates terms -- `<a b | c>` is `(a AND b) OR c`, not one value.
 */
int lex_tokenize(const char *s, tokstream_t *ts)
{
    const char *p = s;
    sbuf_t b = {0};
    int vbracket = 0;        /* depth of a value bracket we are inside */
    int after_colon = 0;     /* term so far ends with ':' -> next char is value */
    int quoted = 0;
    int had_space = 1;       /* leading space counts, so ! and | bind rightwards */

    for (;;) {
        char c = *p;

        if (c == '\0') {
            if (quoted) { free(b.p); return -1; }
            if (vbracket > 0) {
                snprintf(ts->err, sizeof(ts->err), "unterminated '<' value list");
                free(b.p);
                return -1;
            }
            if (flush_term(ts, &b, had_space) != 0) { free(b.p); return -1; }
            if (!ts_push(ts, T_EOF)) { free(b.p); return -1; }
            free(b.p);
            return 0;
        }

        if (c == ' ' || c == '\t') {
            p++;
            if (!quoted && vbracket == 0) {
                had_space = 1;
                if (flush_term(ts, &b, had_space) != 0) { free(b.p); return -1; }
                after_colon = 0;
            } else {
                sb_putc(&b, c);       /* whitespace inside a value is data */
            }
            continue;
        }

        if (c == '"') {
            p++;
            if (!quoted) {
                /* A quote straight after `fn:` continues the value -- that is the
                 * shape the client sends for a path with a space in it
                 * (`SEARCH parent:"/home/a b" folder:`). Treating it as a new
                 * term would split it into `parent:` AND the bare path, and the
                 * bare path matches no name. */
                if (after_colon && b.n) {
                    quoted = 1;
                    after_colon = 0;
                    had_space = 0;
                    continue;
                }
                /* otherwise a quoted run is a term in itself: close whatever came
                 * before */
                if (flush_term(ts, &b, had_space) != 0) { free(b.p); return -1; }
                quoted = 1;
                after_colon = 0;
                had_space = 0;
            } else {
                quoted = 0;
                if (flush_term(ts, &b, had_space) != 0) { free(b.p); return -1; }
                after_colon = 0;
                had_space = 0;
            }
            continue;
        }

        if (!quoted) {
            if (c == '|') {
                /* inside a value list a bar is data, not an OR operator */
                if (vbracket > 0) { sb_putc(&b, c); p++; continue; }
                if (flush_term(ts, &b, had_space) != 0) { free(b.p); return -1; }
                if (push_op(ts, T_OR, had_space) != 0) { free(b.p); return -1; }
                p++; had_space = 0; after_colon = 0;
                continue;
            }
            /* `!` is NOT only when it cannot be a comparison, i.e. when no
             * value is being accumulated (`dc:!=2024` keeps its bang). */
            if (c == '!' && !after_colon && b.n == 0 && vbracket == 0) {
                if (push_op(ts, T_NOT, had_space) != 0) { free(b.p); return -1; }
                p++; had_space = 0;
                continue;
            }
            /* `<` right after `fn:` is ambiguous: it opens a value list
             * (`ext:<a b>`, L162) or it is "less than" (`size:<1mb`, L151).
             * Everything disambiguates by what follows, and so does this: a
             * number means a comparison, anything else a list. Every documented
             * example of each form agrees with that reading. */
            if ((c == '<' || c == '(') && after_colon) {
                char nx = p[1];
                if (c == '(' || isdigit((unsigned char)nx) || nx == '-' || nx == '+') {
                    sb_putc(&b, c);
                    p++;
                    continue;
                }
                sb_putc(&b, c);
                p++;
                if (c == '<') vbracket++;
                continue;
            }
            if (c == '>' && vbracket > 0) {
                sb_putc(&b, c);
                p++;
                if (--vbracket == 0) {
                    if (flush_term(ts, &b, had_space) != 0) { free(b.p); return -1; }
                    after_colon = 0;
                    had_space = 0;
                }
                continue;
            }
            /* `>` after `fn:` with no open value list is still a comparison:
             * `size:>1k`. The `after_colon` flag is still set here because no
             * `<` was consumed. */
            if ((c == '>' || c == ')') && after_colon) {
                sb_putc(&b, c);
                p++;
                continue;
            }
            if (c == '<' || c == '(') {
                if (flush_term(ts, &b, had_space) != 0) { free(b.p); return -1; }
                if (push_op(ts, T_OPEN, had_space) != 0) { free(b.p); return -1; }
                p++; had_space = 0;
                continue;
            }
            if (c == '>' || c == ')') {
                if (flush_term(ts, &b, had_space) != 0) { free(b.p); return -1; }
                if (push_op(ts, T_CLOSE, had_space) != 0) { free(b.p); return -1; }
                p++; had_space = 0;
                continue;
            }
        }

        {
            size_t e = decode_entity(p, &b);
            if (e) { p += e; after_colon = 0; had_space = 0; continue; }
        }

        sb_putc(&b, c);
        p++;
        after_colon = (c == ':');
        if (c != ':') after_colon = 0;
        had_space = 0;
    }
}
