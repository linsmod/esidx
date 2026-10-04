/* Recursive-descent parser: token stream -> ast_t (design §6.1).
 *
 * Grammar, from everything-syntax.md L2-8:
 *
 *     or    := and ( '|' and )*
 *     and   := not ( not )*                 -- juxtaposition is AND
 *     not   := '!' not | primary
 *     prim  := '(' or ')' | '<' or '>' | term
 *
 * `!` binds tighter than juxtaposition, so `!folder: dm:today` is
 * `(!folder:) AND (dm:today)`, which is what Everything does.
 *
 * Two things happen while building a leaf, both of which belong here rather
 * than in the executor:
 *
 *   - the comparison prefix is split off the value (`size:>1k` -> GT, "1k"), so
 *     no matcher has to re-parse it;
 *   - modifiers are peeled off the function name (`nowhole:foo` -> mod.whole
 *     cleared on function `foo`), and a `<a b>` / `<a|b>` value becomes a list
 *     node instead of a scalar.
 *
 * Both are pure syntax. Nothing here consults the index, which is what lets the
 * same AST be reused for the primary search and for the FILTER_* stage.
 */

#include "syntax.h"
#include "lexer.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>

#define MAX_DEPTH 64

typedef struct {
    tokstream_t ts;
    size_t      i;
    int         depth;
    char       *err;
    size_t      errsz;
    int         failed;
} parser_t;

/* ------------------------------------------------------------- allocation */

static ast_t *node(ast_kind_t k)
{
    ast_t *n = calloc(1, sizeof(ast_t));
    if (n) n->kind = k;
    return n;
}

void ast_free(ast_t *n)
{
    if (!n) return;
    ast_free(n->a);
    ast_free(n->b);
    ast_free(n->list);
    ast_free(n->sub);
    free(n->fn);
    free(n->val);
    free(n);
}

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void perr(parser_t *p, const char *fmt, ...)
{
    if (p->failed) return;
    p->failed = 1;
    if (!p->err || p->errsz == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, p->errsz, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------ token access */

static token_t *cur(parser_t *p)
{
    return p->i < p->ts.n ? &p->ts.v[p->i] : NULL;
}

static tok_kind_t curk(parser_t *p)
{
    token_t *t = cur(p);
    return t ? t->kind : T_EOF;
}

static token_t *advance(parser_t *p)
{
    return p->i < p->ts.n ? &p->ts.v[p->i++] : NULL;
}

/* --------------------------------------------------------------- modifiers */

/* Modifiers may be chained and may carry `no` (disable) or `?` (force on).
 * Returns the remaining function name, or NULL when the name is entirely
 * modifiers. `*mod` accumulates across the chain. */
/* Modifier names, longest first so `startwith` cannot be read as `start` and
 * `wholeword` cannot be shadowed by `whole`. (L82-84 draw the distinction the
 * table encodes: prefix:/suffix: anchor at a word, startwith:/endwith: anchor at
 * the filename.) */
static const struct { const char *n; unsigned bit; } mod_table[] = {
    { "ignorepunc",  MOD_IGNOREPUNC },
    { "ignorews",    MOD_IGNOREWS   },
    { "wholeword",   MOD_WW         },
    { "nocase",      MOD_CASE       },
    { "diacritics",  MOD_DIACRITICS },
    { "wildcards",   MOD_WHOLE      },
    { "startwith",   MOD_STARTWITH  },
    { "endwith",     MOD_ENDWITH    },
    { "regex",       MOD_REGEX      },
    { "whole",       MOD_WHOLE      },
    { "prefix",      MOD_PREFIX     },
    { "suffix",      MOD_SUFFIX     },
    { "path",        MOD_PATH       },
    { "ww",          MOD_WW         },
    { "len",         MOD_LEN        },
    { "case",        MOD_CASE       },
};

/* Peel exactly one modifier off the front of `name`. Returns 1 when one was
 * found (and consumed), 0 when `name` is not a modifier. Handles the `no` and
 * `?` prefixes, which combine with any modifier.
 *
 * `len` is deliberately not peelable: it is the only name that is both a
 * modifier and a function (everything-syntax.md L78 vs L88), and `len:12` has to
 * mean the function. Everything resolves the same ambiguity by position -- a
 * modifier is followed by another function or modifier, never by a number -- and
 * so does this: if nothing would be left of the name, `len` stays a function. */
static int peel_one(const char **name, mod_t *mod)
{
    const char *p = *name;
    int on = 1;
    if (p[0] == 'n' && p[1] == 'o' && isalpha((unsigned char)p[2])) {
        on = 0; p += 2;
    } else if (p[0] == '?') {
        p += 1;
    }
    for (size_t i = 0; i < sizeof(mod_table) / sizeof(mod_table[0]); i++) {
        size_t l = strlen(mod_table[i].n);
        if (strncmp(p, mod_table[i].n, l) != 0) continue;
        if (isalpha((unsigned char)p[l])) continue;      /* a longer word */
        if (p[l] == '\0' && !strcmp(mod_table[i].n, "len")) continue;
        if (on) *mod |= mod_table[i].bit;
        else   *mod &= ~mod_table[i].bit;
        *name = p + l;
        return 1;
    }
    return 0;
}

/* Peel every modifier prefix from a function name. */
static const char *peel_mods(const char *name, mod_t *mod)
{
    for (;;) {
        const char *p = name;
        if (!peel_one(&p, mod)) return name;
        name = p;
    }
}

/* `path:regex:foo` stacks modifiers in the *value* once the name is spent,
 * because the ETP client sends exactly that shape (EtpBrowseViewModel builds
 * `path:regex:...$` as a single token). Peel those too. */
static void peel_value_mods(char *val, mod_t *mod)
{
    for (;;) {
        const char *colon = strchr(val, ':');
        if (!colon || colon == val) return;
        size_t nlen = (size_t)(colon - val);
        char head[32];
        if (nlen >= sizeof(head)) return;
        memcpy(head, val, nlen);
        head[nlen] = '\0';
        for (size_t i = 0; i < nlen; i++) head[i] = (char)tolower((unsigned char)head[i]);
        const char *p = head;
        mod_t probe = 0;
        if (!peel_one(&p, &probe) || *p) return;   /* not purely modifiers */
        *mod |= probe;
        memmove(val, colon + 1, strlen(colon + 1) + 1);
    }
}

/* ------------------------------------------------------- value list parsing */

/* `fn:<a b>` is an AND list, `fn:<a|b>` an OR list (L162-163). Both arrive as a
 * single T_TERM whose value is "<a|b>". Rebuild them as a list node. */
static ast_t *parse_list(parser_t *p, const char *inner)
{
    size_t n = strlen(inner);
    char *tmp = malloc(n + 1);
    if (!tmp) { perr(p, "out of memory"); return NULL; }
    memcpy(tmp, inner, n + 1);

    /* decide AND vs OR from the first top-level separator */
    int is_or = 0, depth = 0;
    for (size_t i = 0; i < n; i++) {
        if (tmp[i] == '(' || tmp[i] == '[') depth++;
        else if (tmp[i] == ')' || tmp[i] == ']') depth--;
        else if (tmp[i] == '|' && depth == 0) { is_or = 1; break; }
    }

    ast_t *root = node(is_or ? AST_OR : AST_AND);
    if (!root) { free(tmp); perr(p, "out of memory"); return NULL; }

    /* accumulate leaf texts, honouring quotes and nesting */
    char *acc = malloc(n + 1);
    if (!acc) { free(tmp); ast_free(root); perr(p, "out of memory"); return NULL; }
    size_t alen = 0;
    acc[0] = '\0';
    int inq = 0, nest = 0;

    ast_t **tail = &root->a;
    for (size_t i = 0; i <= n; i++) {
        char c = tmp[i];
        int sep = (i == n) || (c == '|' && !inq && nest == 0 && is_or)
                        || (c == ' ' && !inq && nest == 0 && !is_or);
        if (!sep) {
            if (c == '"') inq = !inq;
            else if (!inq && (c == '(' || c == '[')) nest++;
            else if (!inq && (c == ')' || c == ']')) { if (nest) nest--; }
            acc[alen++] = c;
            acc[alen] = '\0';
            continue;
        }
        if (alen == 0) continue;              /* skip empty fields */
        ast_t *leaf = node(AST_TERM);
        if (!leaf) { free(acc); free(tmp); ast_free(root); perr(p, "out of memory"); return NULL; }
        leaf->val = dupstr(acc);
        leaf->fn = dupstr("");
        if (!leaf->val || !leaf->fn) {
            ast_free(leaf); free(acc); free(tmp); ast_free(root);
            perr(p, "out of memory"); return NULL;
        }
        *tail = leaf;
        tail = &leaf->b;
        alen = 0;
        acc[0] = '\0';
    }
    free(acc);
    free(tmp);
    return root;
}

/* ------------------------------------------------------------- leaf values */

/* Split `name:value`. Returns 1 when a function name was recognised. A bare
 * word keeps everything, because "c:" or "12:30" are values, not functions.
 *
 * Hyphens count as name characters: Everything's function set includes
 * `child-count:`, `path-part:`, `file-list-filename:` and `date-modified:`. */
static int split_function(const char *text, char *fn, size_t fnsz,
                          char *val, size_t valsz)
{
    const char *colon = strchr(text, ':');
    if (!colon) return 0;
    size_t n = (size_t)(colon - text);
    if (n == 0 || n >= fnsz) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!isalpha(c) && c != '_' && c != '-') return 0;
    }
    /* a name that is only hyphens is not a function */
    int saw_alpha = 0;
    for (size_t i = 0; i < n; i++)
        if (isalpha((unsigned char)text[i])) { saw_alpha = 1; break; }
    if (!saw_alpha) return 0;

    memcpy(fn, text, n);
    fn[n] = '\0';
    for (size_t i = 0; i < n; i++) fn[i] = (char)tolower((unsigned char)fn[i]);
    snprintf(val, valsz, "%s", colon + 1);
    return 1;
}

/* Peel a comparison operator off the front of a value (L148-159). */
static void split_cmp(char *val, cmp_op_t *cmp)
{
    *cmp = CMP_EQ;
    char *p = val;
    if (p[0] == '>' && p[1] == '=') { *cmp = CMP_GE; p += 2; }
    else if (p[0] == '<' && p[1] == '=') { *cmp = CMP_LE; p += 2; }
    else if (p[0] == '>') { *cmp = CMP_GT; p += 1; }
    else if (p[0] == '<') { *cmp = CMP_LT; p += 1; }
    else if (p[0] == '=' && p[1] == '=') { *cmp = CMP_EQ; p += 2; }
    else if (p[0] == '=') { *cmp = CMP_EQ; p += 1; }
    else if (p[0] == '!' && p[1] == '=') { *cmp = CMP_NE; p += 2; }
    else if (p[0] == '!') { *cmp = CMP_NE; p += 1; }
    if (p != val) memmove(val, p, strlen(p) + 1);
}

static ast_t *parse_or(parser_t *p);
static ast_t *parse_expr_str(parser_t *p, const char *s);

/* Parse a nested search string -- `child:<...>` carries one (L111). The
 * sub-parser gets a fresh token stream and inherits the error sink, so a broken
 * nested search is reported at the same place a broken outer one is. */
static ast_t *parse_expr_str(parser_t *p, const char *s)
{
    tokstream_t saved = p->ts;
    size_t saved_i = p->i;
    int saved_depth = p->depth;

    memset(&p->ts, 0, sizeof(p->ts));
    p->i = 0;
    p->depth = 0;

    ast_t *r = NULL;
    if (lex_tokenize(s, &p->ts) != 0) {
        perr(p, "%s", p->ts.err[0] ? p->ts.err : "cannot tokenise the nested search");
    } else {
        r = parse_or(p);
        if (!p->failed && curk(p) != T_EOF)
            perr(p, "trailing input in the nested search");
    }
    if (p->failed) { ast_free(r); r = NULL; }

    tokstream_free(&p->ts);
    p->ts = saved;
    p->i = saved_i;
    p->depth = saved_depth;
    return r;
}

static ast_t *parse_term(parser_t *p, const char *text)
{
    ast_t *n = node(AST_TERM);
    if (!n) { perr(p, "out of memory"); return NULL; }

    char raw_fn[64], fn[80];
    /* One heap buffer for the value where there were two 2 KB stack ones, because
     * SYNTAX_VALUE_MAX is 8 KB and parse_term sits under MAX_DEPTH frames of recursion:
     * 16 KB per frame is a stack overflow waiting for a query with a lot of brackets in
     * it. peel_value_mods() shifts the string left in place, so one buffer is all the
     * two were ever needed for. */
    char *val = malloc(SYNTAX_VALUE_MAX);
    if (!val) { ast_free(n); perr(p, "out of memory"); return NULL; }

    if (split_function(text, raw_fn, sizeof(raw_fn), val, SYNTAX_VALUE_MAX)) {
        const char *rest = peel_mods(raw_fn, &n->mod);
        snprintf(fn, sizeof(fn), "%s", rest);
        /* the name may have been nothing but modifiers; then the value can carry
         * more of them (`path:regex:...`) */
        if (!*fn) peel_value_mods(val, &n->mod);
    } else {
        snprintf(fn, sizeof(fn), "%s", "");
        snprintf(val, SYNTAX_VALUE_MAX, "%s", text);
    }

    /* bracketed value list */
    size_t vl = strlen(val);
    if (vl >= 2 && val[0] == '<' && val[vl - 1] == '>') {
        char *inner = malloc(vl - 1);
        if (!inner) { free(val); ast_free(n); perr(p, "out of memory"); return NULL; }
        memcpy(inner, val + 1, vl - 2);
        inner[vl - 2] = '\0';
        free(val);
        n->list = parse_list(p, inner);
        free(inner);
        if (!n->list) { ast_free(n); return NULL; }
        n->val = dupstr("");
        n->fn = dupstr(fn);
        if (!n->val || !n->fn) { ast_free(n); perr(p, "out of memory"); return NULL; }
        return n;
    }

    /* `child:<expr>` carries a nested search rather than a value */
    if (!strcmp(fn, "child")) {
        n->fn = dupstr(fn);
        n->val = dupstr("");
        if (!n->fn || !n->val) { free(val); ast_free(n); perr(p, "out of memory"); return NULL; }
        n->sub = parse_expr_str(p, val);
        free(val);
        if (!n->sub) { ast_free(n); return NULL; }
        return n;
    }

    split_cmp(val, &n->cmp);

    n->fn = dupstr(fn);
    n->val = dupstr(val);
    free(val);
    if (!n->fn || !n->val) { ast_free(n); perr(p, "out of memory"); return NULL; }
    return n;
}

/* ----------------------------------------------------------------- grammar */

static ast_t *parse_or(parser_t *p);

static ast_t *parse_primary(parser_t *p)
{
    token_t *t = cur(p);
    if (!t) { perr(p, "unexpected end of query"); return NULL; }

    if (t->kind == T_OPEN) {
        advance(p);
        if (++p->depth > MAX_DEPTH) {
            perr(p, "query nested deeper than %d levels", MAX_DEPTH);
            return NULL;
        }
        ast_t *inner = parse_or(p);
        p->depth--;
        if (!inner) return NULL;
        if (curk(p) != T_CLOSE) {
            ast_free(inner);
            perr(p, "missing '>' to close the group");
            return NULL;
        }
        advance(p);
        /* a parenthesised group keeps its own shape; AST_AND with a == NULL is
         * not produced, so wrap nothing when there is a single child */
        return inner;
    }

    if (t->kind == T_TERM) {
        advance(p);
        return parse_term(p, t->text);
    }

    if (t->kind == T_CLOSE) { perr(p, "unbalanced '>'"); return NULL; }
    if (t->kind == T_OR)    { perr(p, "'|' without a left operand"); return NULL; }
    perr(p, "unexpected '%s'", t->kind == T_NOT ? "!" : "end of query");
    return NULL;
}

static ast_t *parse_not(parser_t *p)
{
    if (curk(p) == T_NOT) {
        advance(p);
        if (++p->depth > MAX_DEPTH) {
            perr(p, "query nested deeper than %d levels", MAX_DEPTH);
            return NULL;
        }
        ast_t *inner = parse_not(p);
        p->depth--;
        if (!inner) return NULL;
        ast_t *n = node(AST_NOT);
        if (!n) { ast_free(inner); perr(p, "out of memory"); return NULL; }
        n->a = inner;
        return n;
    }
    return parse_primary(p);
}

static ast_t *parse_and(parser_t *p)
{
    ast_t *left = parse_not(p);
    if (!left) return NULL;
    while (!p->failed) {
        tok_kind_t k = curk(p);
        if (k == T_TERM || k == T_OPEN || k == T_NOT) {
            ast_t *right = parse_not(p);
            if (!right) { ast_free(left); return NULL; }
            ast_t *n = node(AST_AND);
            if (!n) { ast_free(left); ast_free(right); perr(p, "out of memory"); return NULL; }
            n->a = left;
            n->b = right;
            left = n;
            continue;
        }
        break;
    }
    return left;
}

static ast_t *parse_or(parser_t *p)
{
    ast_t *left = parse_and(p);
    if (!left) return NULL;
    while (!p->failed && curk(p) == T_OR) {
        advance(p);
        ast_t *right = parse_and(p);
        if (!right) { ast_free(left); return NULL; }
        ast_t *n = node(AST_OR);
        if (!n) { ast_free(left); ast_free(right); perr(p, "out of memory"); return NULL; }
        n->a = left;
        n->b = right;
        left = n;
    }
    return left;
}

ast_t *syntax_parse(const char *s, char *err, size_t errsz)
{
    if (err && errsz) err[0] = '\0';
    if (!s) return NULL;

    /* an all-whitespace query means "everything", which the protocol layer
     * represents as a NULL AST rather than a match-all node */
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return NULL;

    parser_t p_ = {0};
    p_.err = err;
    p_.errsz = errsz;

    if (lex_tokenize(s, &p_.ts) != 0) {
        if (err && errsz)
            snprintf(err, errsz, "%s", p_.ts.err[0] ? p_.ts.err : "cannot tokenise the query");
        tokstream_free(&p_.ts);
        return NULL;
    }

    ast_t *n = parse_or(&p_);
    if (!p_.failed && curk(&p_) != T_EOF)
        perr(&p_, "trailing input after the end of the query");

    tokstream_free(&p_.ts);
    if (p_.failed) { ast_free(n); return NULL; }
    return n;
}

/* ------------------------------------------------------------------ dumping */

static void dump1(const ast_t *n, char *out, size_t outsz, size_t *pos);

static void emit(char *out, size_t outsz, size_t *pos, const char *s)
{
    size_t l = strlen(s);
    if (*pos + l + 1 >= outsz) return;
    memcpy(out + *pos, s, l);
    *pos += l;
    out[*pos] = '\0';
}

static void dump1(const ast_t *n, char *out, size_t outsz, size_t *pos)
{
    static const char *ops[] = { " AND ", " OR ", " NOT " };
    if (!n) { emit(out, outsz, pos, "<null>"); return; }
    switch (n->kind) {
    case AST_AND: case AST_OR: case AST_NOT:
        if (n->kind == AST_NOT) emit(out, outsz, pos, "NOT ");
        if (n->a) dump1(n->a, out, outsz, pos);
        if (n->kind != AST_NOT) {
            emit(out, outsz, pos, ops[n->kind]);
            if (n->b) dump1(n->b, out, outsz, pos);
        }
        break;
    case AST_TERM: {
        char tmp[1100];
        if (n->fn && n->fn[0]) snprintf(tmp, sizeof(tmp), "%s:%s", n->fn, n->val ? n->val : "");
        else snprintf(tmp, sizeof(tmp), "%s", n->val ? n->val : "");
        emit(out, outsz, pos, tmp);
        break;
    }
    }
}

void ast_dump(const ast_t *n, char *out, size_t outsz)
{
    if (!out || !outsz) return;
    out[0] = '\0';
    size_t pos = 0;
    dump1(n, out, outsz, &pos);
}
