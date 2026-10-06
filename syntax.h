/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
#ifndef ESIDX_SYNTAX_H
#define ESIDX_SYNTAX_H

/* Syntax layer (design §6.1) and the query surface the protocol layer speaks
 * (design §1.1, §6.3).
 *
 *   lexer   text            -> token stream        (lexer.c)
 *   parser  token stream    -> ast_t               (parser.c)
 *   exec    ast_t + options -> sorted, sliced eids (query.c)
 *
 * The AST is deliberately small: binary AND/OR/NOT plus leaves. Everything's
 * grouping, operators and modifiers all collapse into that shape, so the
 * executor only ever has three cases to reason about.
 *
 * Modifiers (`nocase:`, `?whole:`, …) are parsed and attached to leaves rather
 * than rejected, so that a query the client sends is never silently misread;
 * they are applied at match time and never affect the index (design §2, L2).
 */

#include "esidx.h"

/* -------------------------------------------------------------------- AST */

typedef enum {
    AST_AND = 0,   /* implicit: space between two terms */
    AST_OR,        /* `|` */
    AST_NOT,       /* `!` */
    AST_TERM       /* function:value, or a bare word */
} ast_kind_t;

/* comparison forms of design §1.4 / everything-syntax.md L116-131 */
typedef enum {
    CMP_EQ = 0,
    CMP_NE,        /* `!=` and `!` */
    CMP_LT,
    CMP_LE,
    CMP_GT,
    CMP_GE
} cmp_op_t;

/* per-leaf match modifiers, from the ETP match-option subcommands.
 *
 * Plain bit flags rather than a bitfield struct: the parser sets them by name
 * out of a table, and you cannot take the address of a bitfield, so the table
 * could not be written. */
enum {
    MOD_WHOLE      = 1u << 0,   /* whole:  exact string            */
    MOD_WW         = 1u << 1,   /* ww:     whole word              */
    MOD_PREFIX     = 1u << 2,   /* prefix: start of a *word*       */
    MOD_SUFFIX     = 1u << 3,   /* suffix: end of a *word*         */
    MOD_REGEX      = 1u << 4,   /* regex:  value is a pattern      */
    MOD_PATH       = 1u << 5,   /* path:   match the full path     */
    MOD_CASE       = 1u << 6,   /* case:   case sensitive          */
    MOD_DIACRITICS = 1u << 7,
    MOD_IGNOREPUNC = 1u << 8,
    MOD_IGNOREWS   = 1u << 9,
    MOD_LEN        = 1u << 10,  /* len:    value is a length       */
    MOD_RAW        = 1u << 11,  /* whole filename, not a substring */
    /* startwith:/endwith: anchor at the start/end of the *filename*, with no
     * word-boundary requirement -- distinct from prefix:/suffix:, which anchor
     * at a word boundary (everything-syntax.md L82-84). */
    MOD_STARTWITH  = 1u << 12,
    MOD_ENDWITH    = 1u << 13
};

typedef uint32_t mod_t;

typedef struct ast ast_t;

struct ast {
    ast_kind_t kind;
    uint32_t   uid;      /* assigned by the optimiser; identifies the driver */

    /* AST_AND / AST_OR: two operands. AST_NOT: `a` only. */
    ast_t *a, *b;

    /* AST_TERM */
    char  *fn;        /* function name, lowercase; "" for a bare word */
    char  *val;       /* value text, comparison prefix already split off */
    cmp_op_t cmp;
    mod_t  mod;
    /* `fn:<a b>` is an AND list and `fn:<a|b>` an OR list; both become a list
     * node hanging off `a`. NULL for a scalar value. */
    ast_t *list;
    /* `child:<expr>` carries a nested search; `a` holds it. */
    ast_t *sub;
};

/* The longest value one term may carry, NUL excluded.
 *
 * Everything's search box takes tens of thousands of characters and `ext:` is the
 * shape that gets there -- a list of extensions is a single term -- so this is a
 * protocol-sized number on purpose: it matches the longest control line the ETP layer
 * will hand over (its 8 KB buffer), which makes truncation here unreachable from the
 * wire. It was 2048, and a term longer than that was cut *silently*, so the answer was
 * a query nobody asked: `ext:e1;e2;<2 100 characters of names>;e5` matched e1 and e2
 * and dropped the e5 at the end, with no error anywhere (AGENTS.md 5.3).
 *
 * Nothing above this layer may bound a term below this: a value that arrives cut is a
 * query that answers something else. */
#define SYNTAX_VALUE_MAX 8192

/* Parse one query string. Returns NULL and fills `err` on a syntax error.
 * `err` may be NULL. */
ast_t *syntax_parse(const char *s, char *err, size_t errsz);
void   ast_free(ast_t *n);
/* Compact single-line rendering, for LOGD and for error messages. */
void   ast_dump(const ast_t *n, char *out, size_t outsz);

/* ------------------------------------------------------- ETP match options */

/* Mirrors etp_server_client_t's match_* / ignore_* fields, which the
 * `EVERYTHING <sub>` group sets (design §1.1). Everything the client sends is
 * 0 or 1. */
typedef struct {
    int match_case;
    int match_whole_word;
    int match_path;
    int match_diacritics;
    int match_prefix;
    int match_suffix;
    int match_regex;
    int ignore_punctuation;
    int ignore_whitespace;
    int hide_empty_search_results;

    /* second-stage filter layer: an independent matcher applied to whatever
     * survived the primary search (design §1.1 note, F8). `filter_search` is
     * owned by the caller. */
    uint32_t filter_flags;
    const char *filter_search;
} match_opts_t;

/* filter_flags bits, matching EVERYTHING_PLUGIN_FILTER_FLAG_* */
#define FF_CASE      0x0001u
#define FF_WHOLEWORD 0x0002u
#define FF_PATH      0x0004u
#define FF_DIACRITICS 0x0008u
#define FF_PREFIX    0x0010u
#define FF_SUFFIX    0x0020u
#define FF_IGNORE_PUNCTUATION 0x0040u
#define FF_IGNORE_WHITESPACE  0x0080u
#define FF_REGEX    0x0100u

/* Turn the ETP match options into the modifier set a bare search term gets. */
mod_t match_opts_mods(const match_opts_t *mo);

/* ------------------------------------------------------------- sort keys */

/* The 22 ETP sort names map onto 10 properties (design §1.2). Keys we cannot
 * answer exactly fall back to the nearest available one and say so at DEBUG. */
typedef enum {
    SORT_NAME = 0,
    SORT_PATH,
    SORT_SIZE,
    SORT_MTIME,     /* date_modified */
    SORT_CTIME,     /* date_created */
    SORT_EXT,
    SORT_ATTRIBUTES,
    SORT_FILE_LIST_FILENAME,
    SORT_RECENTLY_CHANGED
} sort_key_t;

#define SORT_INVERSE_SIZE   0x8000u   /* flag: flip direction on the size key */
#define SORT_KEY_MASK       0x7FFFu

/* Map an ETP sort name ("name_ascending", "inverse_size_descending", …).
 * Returns 0 on success, -1 if the name is not one of the 22. */
int  sort_from_etp_name(const char *name, uint16_t *out_key, int *out_asc);

/* ---------------------------------------------------------------- executor */

typedef struct {
    sort_key_t key;
    int        desc;
} sort_spec_t;

/* A complete, sorted match set.
 *
 * The whole set is kept rather than just the requested page, because the
 * protocol layer re-slices it: the ETP client pages with OFFSET/COUNT and
 * toggles columns without re-issuing SEARCH, and design §6.4's result cache is
 * only possible if the full ordered set survives between queries. `n` is the
 * RESULT_COUNT the protocol reports -- it is independent of any COUNT.
 *
 * `ids` is always fully sorted, even when only one page is ever read; that is
 * what makes the order total and therefore reproducible across queries. */
typedef struct {
    eid_t   *ids;
    uint32_t n;
    uint32_t n_dir, n_file;
    uint64_t t_plan_us, t_eval_us, t_sort_us;
    /* The sort stage split in two, because "sorting is slow" and "comparing is
     * slow" call for different fixes and only one number was being reported.
     * `t_key_us` is the per-row key extraction that has to happen before any
     * comparison can; the rest of `t_sort_us` is the compare. `sort_ncmp` is how
     * many comparisons actually ran, so ns-per-comparison is derivable rather than
     * guessed -- which is what says whether the cost is the comparator or the row
     * count (design §10). */
    uint64_t t_key_us;
    uint64_t sort_ncmp;
    uint32_t driver;    /* leaf that seeded the candidate bitmap, or UINT32_MAX */
    uint32_t seed;      /* candidates the driver produced */
    uint32_t leaf_cnt;  /* index-backed leaves the optimiser could have used */
    int      topk;      /* 1 when the bounded heap replaced the full sort */
} qset_t;

void qset_free(qset_t *s);
/* Copy [offset, offset+count) out of the set. `count == 0` means "to the end".
 * Returns the number of ids written. */
uint32_t qset_slice(const qset_t *s, uint32_t offset, uint32_t count, eid_t **out);

/* Execute `ast` against `db`.
 *
 * Two stages, in the order design §6.3 mandates:
 *   1. the primary search  -> a sorted, complete matched set
 *   2. the FILTER_* re-match over that set
 *
 * When `ast` is NULL the whole tree matches, which is what an empty `SEARCH`
 * means. `mo` may be NULL (all defaults). Returns 0 on success, -1 on
 * allocation failure or a leaf the executor cannot honour. */
int qexec(const esidx_t *db, const ast_t *ast, const match_opts_t *mo,
          sort_spec_t sort, qset_t *out);

/* ------------------------------------------------------------------ regex */

/* A small backtracking regex covering the constructs Everything patterns use
 * in practice: literals, `.`, `*`, `+`, `?`, `[...]` with ranges and negation,
 * `|`, groups, `^`, `$`, and backslash escapes. Enough for `regex:` and
 * `path:regex:`; deliberately not POSIX/PCRE.
 *
 * `re_compile` returns an opaque program; `re_match` is a search (not an
 * anchored match) unless the pattern itself carries ^ and $. */
typedef struct regex re_t;

re_t *re_compile(const char *pat, int icase);
void  re_free(re_t *re);
int   re_match(const re_t *re, const char *s);
const char *re_error(void);

/* Everything's wildcard syntax: `*`, `?`, `#`, `[abc]`, `[!abc]`, `\x`.
 * `*` does not cross a path separator, `**` does (everything-syntax.md L30-35).
 * Unlike a regex this is anchored: with a wildcard present the pattern applies
 * to the whole filename, so ask wildcard_present() to find out which mode the
 * caller is in. */
int wildcard_match(const char *pat, const char *s, int nocase);
/* The unanchored form, for a pattern that searches a *path* rather than a whole
 * filename: it looks for the pattern anywhere in the subject. `*` still refuses
 * to cross a separator, so `esidx` + separator + star is the direct children and
 * not the subtree. (Written out in pieces: a literal slash-star in a C comment
 * opens a nested comment and -Wcomment says so, once per file.) */
int wildcard_match_in(const char *pat, const char *s, int nocase);
int wildcard_present(const char *pat);

#endif /* ESIDX_SYNTAX_H */
