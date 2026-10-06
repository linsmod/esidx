/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
/* order_ref -- print names in the order strcasecmp() puts them.
 *
 * A test oracle, and it exists because the two obvious shell substitutes are both
 * wrong in ways that are invisible until a fixture contains the case that breaks
 * them:
 *
 *   `sort -f`      GNU sort folds for *equality* but orders by the original bytes, so
 *                  it puts Apple.txt before a_dir while strcasecmp puts a_dir first.
 *                  Measured, and it agreed on a lower-case-only fixture, so an
 *                  assertion written against it passes for the wrong reason.
 *   `tr A-Z a-z    folding then sorting gives the right order for ASCII, but under
 *   | sort`        LC_ALL=C `sort` compares bytes as *signed* chars, so any byte
 *                  >= 0x80 sorts before ASCII. strcasecmp compares unsigned, so a
 *                  UTF-8 filename lands in the other half of the order.
 *
 * Neither is a close enough approximation to assert against. This is the definition
 * rather than an approximation of it: the sort used strcasecmp before the name rank
 * existed (design §10), the result order is part of the protocol contract (a client
 * pages by OFFSET), and cmp_ref.sh compares ordered result sets.
 *
 *   order_ref <file>       one name per line -> the same names, strcasecmp order
 *   order_ref -c <a> <b>   exit 1 if the two files differ, print the first few diffs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int cmp_ci(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static char **read_lines(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return NULL; }
    size_t cap = 64, n = 0;
    char **v = malloc(cap * sizeof(char *));
    char line[8192];
    if (!v) { fclose(f); return NULL; }
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (n + 1 >= cap) {
            cap *= 2;
            char **nv = realloc(v, cap * sizeof(char *));
            if (!nv) { fclose(f); return NULL; }
            v = nv;
        }
        v[n++] = strdup(line);
    }
    fclose(f);
    v[n] = NULL;
    *out_n = n;
    return v;
}

static void free_lines(char **v, size_t n)
{
    if (!v) return;
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

static int check(const char *got, const char *want)
{
    size_t ng = 0, nw = 0;
    char **g = read_lines(got, &ng), **w = read_lines(want, &nw);
    if (!g || !w) { free_lines(g, ng); free_lines(w, nw); return 2; }
    qsort(w, nw, sizeof(char *), cmp_ci);

    int bad = 0;
    size_t n = ng < nw ? ng : nw;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(g[i], w[i])) {
            if (bad < 6)
                printf("         row %zu: got '%s' want '%s'\n", i + 1, g[i], w[i]);
            bad++;
        }
    }
    if (ng != nw) {
        printf("         %zu rows, expected %zu\n", ng, nw);
        bad++;
    }
    printf("   %s: %zu rows, %d in the wrong place\n",
           bad ? "order differs" : "same order", ng, bad);
    /* The suites run under ASan with leak detection, and a test tool that leaks turns
     * that report into noise -- which is how a real leak in the server would get
     * missed one day. */
    free_lines(g, ng);
    free_lines(w, nw);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "-c")) return check(argv[2], argv[3]);
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>        print in strcasecmp order\n"
                        "       %s -c <got> <want>  compare two orderings\n", argv[0], argv[0]);
        return 2;
    }
    size_t n = 0;
    char **v = read_lines(argv[1], &n);
    if (!v) return 2;
    qsort(v, n, sizeof(char *), cmp_ci);
    for (size_t i = 0; i < n; i++) printf("%s\n", v[i]);
    /* Freed here because the print mode is not a one-shot debugging aid any more:
     * test_etp.sh 11c uses it to produce the expectation a name sort is compared
     * against, and `make check` runs that suite under AddressSanitizer with
     * detect_leaks=1 -- so a tool that leaks here fails the gate, and did. */
    free_lines(v, n);
    return 0;
}