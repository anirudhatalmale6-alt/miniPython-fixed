/* test.c — a self-checking regression suite for miniPython.
 *
 * Each case runs a snippet and compares what it prints against what CPython
 * prints for the same snippet. Expected strings were taken from python3, not
 * from what this interpreter happened to produce -- otherwise the suite would
 * only be asserting that the behaviour has not changed, which is a much weaker
 * claim than that the behaviour is right.
 *
 *   cc -o test test.c -lm && ./test
 */
#define MINI_PY_IMPLEMENTATION
#include "miniPython.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int npass = 0, nfail = 0;

/* Run one snippet with stdout redirected to a pipe, and hand back what it
 * printed. Capturing rather than eyeballing is the whole point: a test you
 * read is a test you stop reading. */
static void check(const char *name, const char *src, const char *want) {
    char buf[4096];
    int  saved, pfd[2];
    ssize_t n;

    fflush(stdout);
    saved = dup(1);
    if (pipe(pfd) != 0) { printf("FAIL %-24s (pipe)\n", name); nfail++; return; }
    dup2(pfd[1], 1);
    close(pfd[1]);

    PyRun_SimpleString(src);
    fflush(stdout);

    dup2(saved, 1);
    close(saved);

    n = read(pfd[0], buf, sizeof(buf) - 1);
    close(pfd[0]);
    if (n < 0) n = 0;
    buf[n] = 0;

    if (!strcmp(buf, want)) {
        printf("ok   %-24s\n", name);
        npass++;
    } else {
        printf("FAIL %-24s\n     wanted: %s     got:    %s", name, want, buf);
        if (n && buf[n - 1] != '\n') printf("\n");
        nfail++;
    }
}

int main(void) {
    Py_Initialize();

    /* --- the crash that started this: parse_or called parse_expr --- */
    check("bool precedence",  "print(True or False and False)\n", "True\n");
    check("or/and nesting",   "print(False or True and True)\n",  "True\n");
    check("not",              "print(not False, not 0, not [])\n", "True True True\n");
    check("comparison",       "print(1 < 2, 2 <= 2, 3 == 3, 4 != 5)\n",
                              "True True True True\n");
    check("not with compare", "print(not 1 == 2)\n", "True\n");
    check("in / not in",      "print('a' in 'abc', 3 not in [1,2])\n", "True True\n");

    /* --- the language --- */
    check("arithmetic",       "print(1 + 2 * 3 - 4)\n", "3\n");
    check("if/else",          "x = 5\nif x < 10:\n    print('small')\nelse:\n    print('big')\n",
                              "small\n");
    check("while",            "i = 0\nwhile i < 3:\n    print(i)\n    i = i + 1\n", "0\n1\n2\n");
    check("for range",        "for i in range(3):\n    print(i)\n", "0\n1\n2\n");
    check("def and return",   "def f(a, b):\n    return a + b\nprint(f(2, 3))\n", "5\n");
    check("recursion",        "def fib(n):\n    if n < 2:\n        return n\n"
                              "    return fib(n-1) + fib(n-2)\nprint(fib(10))\n", "55\n");
    check("list",             "xs = [1, 2, 3]\nxs.append(4)\nprint(xs, len(xs), xs[0], xs[-1])\n",
                              "[1, 2, 3, 4] 4 1 4\n");
    check("sum",              "print(sum([1,2,3]))\n", "6\n");
    check("dict",             "d = {'a': 1, 'b': 2}\nd['c'] = d['a'] + d['b']\nprint(d)\n",
                              "{'a': 1, 'b': 2, 'c': 3}\n");
    check("string methods",   "s = 'hello'\nprint(s.upper(), len(s), s[0], s[-1])\n",
                              "HELLO 5 h o\n");
    check("tuple",            "print((1, 2))\n", "(1, 2)\n");

    /* --- added: conditional expression --- */
    check("ternary true",     "print(1 if True else 2)\n", "1\n");
    check("ternary false",    "print(1 if False else 2)\n", "2\n");
    check("ternary is lazy",  "xs = []\nprint(xs[0] if xs else 'empty')\n", "empty\n");
    check("ternary chains",   "x = 5\nprint('a' if x < 3 else 'b' if x < 10 else 'c')\n", "b\n");
    check("ternary vs or",    "print('y' if False or True else 'n')\n", "y\n");

    /* --- added: slicing --- */
    check("slice",            "xs=[1,2,3,4,5]\nprint(xs[1:3])\n", "[2, 3]\n");
    check("slice open lo",    "xs=[1,2,3,4,5]\nprint(xs[:2])\n", "[1, 2]\n");
    check("slice open hi",    "xs=[1,2,3,4,5]\nprint(xs[3:])\n", "[4, 5]\n");
    check("slice all",        "xs=[1,2,3]\nprint(xs[:])\n", "[1, 2, 3]\n");
    check("slice negative",   "xs=[1,2,3,4,5]\nprint(xs[-2:])\n", "[4, 5]\n");
    /* Out-of-range SLICE bounds clamp; out-of-range INDEX is an error. */
    check("slice clamps",     "xs=[1,2,3]\nprint(xs[0:99])\n", "[1, 2, 3]\n");
    check("slice inverted",   "xs=[1,2,3]\nprint(xs[2:1])\n", "[]\n");
    check("slice string",     "print('hello'[1:4])\n", "ell\n");

    /* --- added: list comprehensions --- */
    check("comprehension",    "print([i for i in range(5)])\n", "[0, 1, 2, 3, 4]\n");
    check("comp with expr",   "print([i*i for i in range(5)])\n", "[0, 1, 4, 9, 16]\n");
    check("comp with filter", "print([i for i in range(10) if i > 6])\n", "[7, 8, 9]\n");
    check("comp over string", "print([c for c in 'abc'])\n", "['a', 'b', 'c']\n");
    check("comp calls a fn",  "def sq(n):\n    return n*n\nprint([sq(i) for i in range(4)])\n",
                              "[0, 1, 4, 9]\n");
    check("comp with ternary","print([i if i else 'z' for i in range(3)])\n", "['z', 1, 2]\n");

    Py_Finalize();

    printf("\n%d passed, %d failed\n", npass, nfail);
    return nfail ? 1 : 0;
}
