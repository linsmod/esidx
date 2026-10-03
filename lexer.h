#ifndef ESIDX_LEXER_H
#define ESIDX_LEXER_H

/* Internal contract between lexer.c and parser.c. Not part of the public API:
 * the AST produced by syntax_parse() is what everything above this layer sees.
 *
 * Why a separate header rather than putting the tokens in syntax.h: the token
 * stream is an implementation detail of the two-stage front end. Exposing it
 * would invite the executor and the protocol layer to depend on lexing. */

#include <stddef.h>

typedef enum {
    T_EOF = 0,
    T_TERM,      /* a word, or function:value */
    T_OR,        /* | */
    T_NOT,       /* ! */
    T_OPEN,      /* < or ( */
    T_CLOSE      /* > or ) */
} tok_kind_t;

typedef struct {
    tok_kind_t kind;
    char      *text;      /* T_TERM: NUL-terminated, owned by the stream */
    int        had_space; /* whitespace preceded this token */
} token_t;

typedef struct {
    token_t *v;
    size_t   n, cap;
    char     err[96];    /* why tokenisation failed, for the parser to report */
} tokstream_t;

/* Tokenise `s`. Returns 0 on success, -1 on allocation failure, an unterminated
 * quote or an unterminated value list. Always leaves a trailing T_EOF. */
int  lex_tokenize(const char *s, tokstream_t *ts);
void tokstream_free(tokstream_t *ts);

#endif /* ESIDX_LEXER_H */
