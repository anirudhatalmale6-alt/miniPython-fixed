/* mini_python.h
 *
 * A single-file mini Python-like interpreter and tiny C API.
 *
 * Usage:
 *   #define MINI_PY_IMPLEMENTATION
 *   #include "mini_python.h"
 *
 * Compile example:
 *   cc main.c -lm
 */

#ifndef MINI_PY_H
#define MINI_PY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PyMiniObject PyObject;

extern PyObject *Py_None;
extern PyObject *Py_True;
extern PyObject *Py_False;

void Py_Initialize(void);
void Py_Finalize(void);
int  PyRun_SimpleString(const char *src);

void Py_IncRef(PyObject *o);
void Py_DecRef(PyObject *o);

PyObject *PyLong_FromLongLong(long long v);
long long PyLong_AsLongLong(PyObject *o);

PyObject *PyFloat_FromDouble(double v);
double PyFloat_AsDouble(PyObject *o);

PyObject *PyUnicode_FromString(const char *s);
const char *PyUnicode_AsUTF8(PyObject *o);

PyObject *PyBool_FromLong(long v);

PyObject *PyList_New(size_t n);
int PyList_Append(PyObject *list, PyObject *item);

PyObject *PyDict_New(void);
int PyDict_SetItem(PyObject *dict, PyObject *key, PyObject *value);

PyObject *PyObject_Repr(PyObject *o);
PyObject *PyObject_Str(PyObject *o);
PyObject *PyObject_CallObject(PyObject *callable, PyObject *args);

#ifdef __cplusplus
}
#endif

#endif /* MINI_PY_H */

#ifdef MINI_PY_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>

/* ---------------------------------------------------------------- */
/* Error state                                                       */
/* ---------------------------------------------------------------- */

static char py_err[1024];

static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(py_err, sizeof(py_err), fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- */
/* Forward types                                                     */
/* ---------------------------------------------------------------- */

typedef struct Node Node;
typedef struct Env Env;

enum {
    PY_NONE = 0,
    PY_BOOL,
    PY_INT,
    PY_FLOAT,
    PY_STR,
    PY_LIST,
    PY_TUPLE,
    PY_DICT,
    PY_FUNC,
    PY_BUILTIN
};

struct PyMiniObject {
    int type;
    int refcnt;

    union {
        int b;
        long long i;
        double f;

        struct {
            char *s;
            size_t len;
        } str;

        struct {
            PyObject **items;
            size_t len;
            size_t cap;
        } seq;

        struct {
            PyObject **keys;
            PyObject **vals;
            size_t len;
            size_t cap;
        } dict;

        struct {
            const char *name;
            PyObject *(*fn)(PyObject *args);
        } builtin;

        struct {
            char *name;
            Node *body;
            char **params;
            int nparams;
            Env *closure;
        } func;
    } u;
};

typedef struct {
    char *name;
    PyObject *val;
} Var;

struct Env {
    int refcnt;
    Env *parent;
    Var *vars;
    int n;
    int cap;
};

/* AST node kinds */
enum {
    N_SUITE = 0,
    N_EXPR_STMT,
    N_ASSIGN,
    N_IF,
    N_WHILE,
    N_FOR,
    N_DEF,
    N_RETURN,
    N_BREAK,
    N_CONTINUE,
    N_PASS,

    N_INT,
    N_FLOAT,
    N_STR,
    N_NAME,
    N_LIST,
    N_TUPLE,
    N_DICT,
    N_BIN,
    N_UNARY,
    N_CALL,
    N_INDEX,
    N_ATTR,
    N_TRUE,
    N_FALSE,
    N_NONE,

    /* Added: the three things the demo in main.c used and the parser did not
       have. N_COND is `a if c else b`; N_SLICE is the [lo:hi] inside an index;
       N_COMP is a list comprehension. */
    N_COND,
    N_SLICE,
    N_COMP
};

struct Node {
    int kind;
    char *str;

    long long ival;
    double fval;

    Node **kids;
    int nkids;
    int kcap;

    char **names;
    int nnames;
    int ncap;
};

/* Token kinds */
enum {
    TK_END = 0,
    TK_NAME,
    TK_NUM,
    TK_STR,
    TK_OP,
    TK_NEWLINE,
    TK_INDENT,
    TK_DEDENT
};

typedef struct {
    int type;
    char *text;
} Token;

/* Execution statuses */
enum {
    S_OK = 0,
    S_BREAK,
    S_CONT,
    S_RET,
    S_ERR
};

/* Globals */
PyObject *Py_None = NULL;
PyObject *Py_True = NULL;
PyObject *Py_False = NULL;

static Env *GLOBAL_ENV = NULL;
static int mp_initialized = 0;

static Token *toks = NULL;
static int ntok = 0;
static int cap_toks = 0;
static int pos = 0;

/* ---------------------------------------------------------------- */
/* Small utility functions                                           */
/* ---------------------------------------------------------------- */

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return q;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ---------------------------------------------------------------- */
/* Dynamic string buffer                                             */
/* ---------------------------------------------------------------- */

typedef struct {
    char *s;
    size_t len;
    size_t cap;
} Buf;

static void buf_init(Buf *b) {
    b->cap = 64;
    b->len = 0;
    b->s = xmalloc(b->cap);
    b->s[0] = 0;
}

static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        while (b->cap < b->len + extra + 1) b->cap *= 2;
        b->s = xrealloc(b->s, b->cap);
    }
}

static void buf_add(Buf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void buf_puts(Buf *b, const char *s) {
    buf_add(b, s, strlen(s));
}

static void buf_addf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0) n = 0;

    va_start(ap, fmt);
    buf_reserve(b, (size_t)n);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);

    b->len += (size_t)n;
}

/* ---------------------------------------------------------------- */
/* Refcounting forward declarations                                  */
/* ---------------------------------------------------------------- */

static void free_object(PyObject *o);
static void env_incref(Env *e);
static void env_decref(Env *e);

void Py_IncRef(PyObject *o) {
    if (o && o != Py_None && o != Py_True && o != Py_False)
        o->refcnt++;
}

void Py_DecRef(PyObject *o) {
    if (!o || o == Py_None || o == Py_True || o == Py_False)
        return;

    if (--o->refcnt == 0)
        free_object(o);
}

/* ---------------------------------------------------------------- */
/* Environments                                                      */
/* ---------------------------------------------------------------- */

static Env *env_new(Env *parent) {
    Env *e = xmalloc(sizeof(Env));
    memset(e, 0, sizeof(*e));
    e->refcnt = 1;
    e->parent = parent;
    env_incref(parent);
    return e;
}

static void env_incref(Env *e) {
    if (e) e->refcnt++;
}

static void env_decref(Env *e) {
    if (!e) return;

    if (--e->refcnt == 0) {
        env_decref(e->parent);

        for (int i = 0; i < e->n; i++) {
            free(e->vars[i].name);
            Py_DecRef(e->vars[i].val);
        }

        free(e->vars);
        free(e);
    }
}

static PyObject *env_get(Env *e, const char *name) {
    for (; e; e = e->parent) {
        for (int i = 0; i < e->n; i++) {
            if (!strcmp(e->vars[i].name, name)) {
                Py_IncRef(e->vars[i].val);
                return e->vars[i].val;
            }
        }
    }
    return NULL;
}

static void env_set_local(Env *e, const char *name, PyObject *val) {
    for (int i = 0; i < e->n; i++) {
        if (!strcmp(e->vars[i].name, name)) {
            Py_IncRef(val);
            Py_DecRef(e->vars[i].val);
            e->vars[i].val = val;
            return;
        }
    }

    if (e->n == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 8;
        e->vars = xrealloc(e->vars, e->cap * sizeof(Var));
    }

    e->vars[e->n].name = xstrdup(name);
    Py_IncRef(val);
    e->vars[e->n].val = val;
    e->n++;
}

/* ---------------------------------------------------------------- */
/* Object constructors                                               */
/* ---------------------------------------------------------------- */

static PyObject *new_obj(int type) {
    PyObject *o = xmalloc(sizeof(PyObject));
    memset(o, 0, sizeof(*o));
    o->type = type;
    o->refcnt = 1;
    return o;
}

static PyObject *new_int(long long v) {
    PyObject *o = new_obj(PY_INT);
    o->u.i = v;
    return o;
}

static PyObject *new_float(double v) {
    PyObject *o = new_obj(PY_FLOAT);
    o->u.f = v;
    return o;
}

static PyObject *new_bool(int v) {
    return v ? Py_True : Py_False;
}

static PyObject *new_str_len(const char *s, size_t len) {
    PyObject *o = new_obj(PY_STR);
    o->u.str.s = xstrndup(s, len);
    o->u.str.len = len;
    return o;
}

static PyObject *new_str(const char *s) {
    return new_str_len(s, strlen(s));
}

static PyObject *new_seq(int type, size_t cap) {
    PyObject *o = new_obj(type);
    o->u.seq.cap = cap;
    o->u.seq.items = cap ? xmalloc(cap * sizeof(PyObject *)) : NULL;
    return o;
}

static PyObject *new_list(void) {
    return new_seq(PY_LIST, 4);
}

static PyObject *new_tuple(void) {
    return new_seq(PY_TUPLE, 4);
}

static PyObject *new_dict(void) {
    PyObject *o = new_obj(PY_DICT);
    o->u.dict.cap = 8;
    o->u.dict.keys = xmalloc(8 * sizeof(PyObject *));
    o->u.dict.vals = xmalloc(8 * sizeof(PyObject *));
    return o;
}

static PyObject *new_builtin(const char *name, PyObject *(*fn)(PyObject *)) {
    PyObject *o = new_obj(PY_BUILTIN);
    o->u.builtin.name = name;
    o->u.builtin.fn = fn;
    return o;
}

static PyObject *new_func(const char *name, Node *body, char **params,
                          int nparams, Env *closure) {
    PyObject *o = new_obj(PY_FUNC);
    o->u.func.name = xstrdup(name);
    o->u.func.body = body;
    o->u.func.nparams = nparams;
    o->u.func.params = xmalloc((nparams ? nparams : 1) * sizeof(char *));

    for (int i = 0; i < nparams; i++)
        o->u.func.params[i] = xstrdup(params[i]);

    o->u.func.closure = closure;
    env_incref(closure);

    return o;
}

/* ---------------------------------------------------------------- */
/* Object freeing                                                    */
/* ---------------------------------------------------------------- */

static void free_object(PyObject *o) {
    switch (o->type) {
        case PY_STR:
            free(o->u.str.s);
            break;

        case PY_LIST:
        case PY_TUPLE:
            for (size_t i = 0; i < o->u.seq.len; i++)
                Py_DecRef(o->u.seq.items[i]);
            free(o->u.seq.items);
            break;

        case PY_DICT:
            for (size_t i = 0; i < o->u.dict.len; i++) {
                Py_DecRef(o->u.dict.keys[i]);
                Py_DecRef(o->u.dict.vals[i]);
            }
            free(o->u.dict.keys);
            free(o->u.dict.vals);
            break;

        case PY_FUNC:
            free(o->u.func.name);
            for (int i = 0; i < o->u.func.nparams; i++)
                free(o->u.func.params[i]);
            free(o->u.func.params);
            env_decref(o->u.func.closure);
            break;

        default:
            break;
    }

    free(o);
}

/* ---------------------------------------------------------------- */
/* Sequence helpers                                                  */
/* ---------------------------------------------------------------- */

static void seq_append(PyObject *seq, PyObject *item) {
    if (seq->u.seq.len == seq->u.seq.cap) {
        seq->u.seq.cap = seq->u.seq.cap ? seq->u.seq.cap * 2 : 4;
        seq->u.seq.items = xrealloc(seq->u.seq.items,
                                    seq->u.seq.cap * sizeof(PyObject *));
    }

    Py_IncRef(item);
    seq->u.seq.items[seq->u.seq.len++] = item;
}

/* ---------------------------------------------------------------- */
/* Equality / dict helpers                                           */
/* ---------------------------------------------------------------- */

static int obj_eq(PyObject *a, PyObject *b);
static int dict_find(PyObject *d, PyObject *key);

static int obj_eq(PyObject *a, PyObject *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;

    if ((a->type == PY_INT || a->type == PY_FLOAT || a->type == PY_BOOL) &&
        (b->type == PY_INT || b->type == PY_FLOAT || b->type == PY_BOOL)) {
        double x = (a->type == PY_FLOAT) ? a->u.f :
                   (a->type == PY_INT) ? (double)a->u.i : (double)a->u.b;
        double y = (b->type == PY_FLOAT) ? b->u.f :
                   (b->type == PY_INT) ? (double)b->u.i : (double)b->u.b;
        return x == y;
    }

    if (a->type != b->type) return 0;

    switch (a->type) {
        case PY_NONE:
            return 1;

        case PY_BOOL:
            return a->u.b == b->u.b;

        case PY_INT:
            return a->u.i == b->u.i;

        case PY_FLOAT:
            return a->u.f == b->u.f;

        case PY_STR:
            return a->u.str.len == b->u.str.len &&
                   !memcmp(a->u.str.s, b->u.str.s, a->u.str.len);

        case PY_LIST:
        case PY_TUPLE:
            if (a->u.seq.len != b->u.seq.len) return 0;
            for (size_t i = 0; i < a->u.seq.len; i++) {
                if (!obj_eq(a->u.seq.items[i], b->u.seq.items[i]))
                    return 0;
            }
            return 1;

        case PY_DICT:
            if (a->u.dict.len != b->u.dict.len) return 0;
            for (size_t i = 0; i < a->u.dict.len; i++) {
                int j = dict_find(b, a->u.dict.keys[i]);
                if (j < 0) return 0;
                if (!obj_eq(a->u.dict.vals[i], b->u.dict.vals[j]))
                    return 0;
            }
            return 1;

        default:
            return 0;
    }
}

static int dict_find(PyObject *d, PyObject *key) {
    for (size_t i = 0; i < d->u.dict.len; i++) {
        if (obj_eq(d->u.dict.keys[i], key))
            return (int)i;
    }
    return -1;
}

static void dict_set(PyObject *d, PyObject *k, PyObject *v) {
    int i = dict_find(d, k);

    if (i >= 0) {
        Py_IncRef(v);
        Py_DecRef(d->u.dict.vals[i]);
        d->u.dict.vals[i] = v;
        return;
    }

    if (d->u.dict.len == d->u.dict.cap) {
        d->u.dict.cap = d->u.dict.cap ? d->u.dict.cap * 2 : 8;
        d->u.dict.keys = xrealloc(d->u.dict.keys,
                                  d->u.dict.cap * sizeof(PyObject *));
        d->u.dict.vals = xrealloc(d->u.dict.vals,
                                  d->u.dict.cap * sizeof(PyObject *));
    }

    Py_IncRef(k);
    Py_IncRef(v);

    d->u.dict.keys[d->u.dict.len] = k;
    d->u.dict.vals[d->u.dict.len] = v;
    d->u.dict.len++;
}

static PyObject *dict_get_obj(PyObject *d, PyObject *k) {
    int i = dict_find(d, k);
    if (i >= 0) {
        Py_IncRef(d->u.dict.vals[i]);
        return d->u.dict.vals[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Printing / repr / str                                             */
/* ---------------------------------------------------------------- */

static void obj_to_buf(PyObject *o, Buf *b, int repr) {
    if (!o) {
        buf_puts(b, "<null>");
        return;
    }

    switch (o->type) {
        case PY_NONE:
            buf_puts(b, "None");
            break;

        case PY_BOOL:
            buf_puts(b, o->u.b ? "True" : "False");
            break;

        case PY_INT:
            buf_addf(b, "%lld", o->u.i);
            break;

        case PY_FLOAT: {
            double v = o->u.f;
            if (isfinite(v) && v == floor(v) && fabs(v) < 1e15)
                buf_addf(b, "%.1f", v);
            else
                buf_addf(b, "%g", v);
            break;
        }

        case PY_STR:
            if (!repr) {
                buf_add(b, o->u.str.s, o->u.str.len);
            } else {
                buf_puts(b, "'");
                for (size_t i = 0; i < o->u.str.len; i++) {
                    char c = o->u.str.s[i];

                    if (c == '\\' || c == '\'') {
                        buf_add(b, "\\", 1);
                        buf_add(b, &c, 1);
                    } else if (c == '\n') {
                        buf_puts(b, "\\n");
                    } else if (c == '\t') {
                        buf_puts(b, "\\t");
                    } else if (c == '\r') {
                        buf_puts(b, "\\r");
                    } else {
                        buf_add(b, &c, 1);
                    }
                }
                buf_puts(b, "'");
            }
            break;

        case PY_LIST:
            buf_puts(b, "[");
            for (size_t i = 0; i < o->u.seq.len; i++) {
                if (i) buf_puts(b, ", ");
                obj_to_buf(o->u.seq.items[i], b, 1);
            }
            buf_puts(b, "]");
            break;

        case PY_TUPLE:
            buf_puts(b, "(");
            for (size_t i = 0; i < o->u.seq.len; i++) {
                if (i) buf_puts(b, ", ");
                obj_to_buf(o->u.seq.items[i], b, 1);
                if (o->u.seq.len == 1 && i == 0)
                    buf_puts(b, ",");
            }
            buf_puts(b, ")");
            break;

        case PY_DICT:
            buf_puts(b, "{");
            for (size_t i = 0; i < o->u.dict.len; i++) {
                if (i) buf_puts(b, ", ");
                obj_to_buf(o->u.dict.keys[i], b, 1);
                buf_puts(b, ": ");
                obj_to_buf(o->u.dict.vals[i], b, 1);
            }
            buf_puts(b, "}");
            break;

        case PY_FUNC:
            buf_addf(b, "<function %s>", o->u.func.name);
            break;

        case PY_BUILTIN:
            buf_addf(b, "<builtin function %s>", o->u.builtin.name);
            break;

        default:
            buf_puts(b, "<object>");
            break;
    }
}

static PyObject *obj_repr(PyObject *o) {
    Buf b;
    buf_init(&b);
    obj_to_buf(o, &b, 1);
    PyObject *r = new_str(b.s);
    free(b.s);
    return r;
}

static PyObject *obj_str(PyObject *o) {
    if (o && o->type == PY_STR)
        return new_str_len(o->u.str.s, o->u.str.len);
    return obj_repr(o);
}

/* ---------------------------------------------------------------- */
/* Truthiness / numeric helpers                                      */
/* ---------------------------------------------------------------- */

static int obj_true(PyObject *o) {
    switch (o->type) {
        case PY_NONE:
            return 0;

        case PY_BOOL:
            return o->u.b;

        case PY_INT:
            return o->u.i != 0;

        case PY_FLOAT:
            return o->u.f != 0.0;

        case PY_STR:
            return o->u.str.len != 0;

        case PY_LIST:
        case PY_TUPLE:
            return o->u.seq.len != 0;

        case PY_DICT:
            return o->u.dict.len != 0;

        default:
            return 1;
    }
}

static int is_num(PyObject *o) {
    return o && (o->type == PY_INT || o->type == PY_FLOAT || o->type == PY_BOOL);
}

static double as_double(PyObject *o) {
    if (o->type == PY_FLOAT) return o->u.f;
    if (o->type == PY_INT) return (double)o->u.i;
    return (double)o->u.b;
}

static long long as_long(PyObject *o) {
    if (o->type == PY_INT) return o->u.i;
    if (o->type == PY_FLOAT) return (long long)o->u.f;
    return o->u.b ? 1 : 0;
}

static int obj_less(PyObject *a, PyObject *b) {
    if (is_num(a) && is_num(b))
        return as_double(a) < as_double(b);

    if (a && b && a->type == PY_STR && b->type == PY_STR)
        return strcmp(a->u.str.s, b->u.str.s) < 0;

    set_error("unsupported comparison");
    return -1;
}

/* ---------------------------------------------------------------- */
/* Membership / indexing                                             */
/* ---------------------------------------------------------------- */

static int obj_contains(PyObject *container, PyObject *item) {
    if (container->type == PY_LIST || container->type == PY_TUPLE) {
        for (size_t i = 0; i < container->u.seq.len; i++) {
            if (obj_eq(container->u.seq.items[i], item))
                return 1;
        }
        return 0;
    }

    if (container->type == PY_DICT) {
        for (size_t i = 0; i < container->u.dict.len; i++) {
            if (obj_eq(container->u.dict.keys[i], item))
                return 1;
        }
        return 0;
    }

    if (container->type == PY_STR) {
        if (item->type != PY_STR) {
            set_error("'in <string>' requires string as left operand");
            return -1;
        }
        return strstr(container->u.str.s, item->u.str.s) != NULL;
    }

    set_error("argument of type '%s' is not iterable",
              container->type == PY_NONE ? "NoneType" : "object");
    return -1;
}

static int seq_index(PyObject *idx, long long len, long long *out) {
    if (!is_num(idx)) {
        set_error("indices must be integers");
        return -1;
    }

    long long i = as_long(idx);
    if (i < 0) i += len;

    if (i < 0 || i >= len) {
        set_error("index out of range");
        return -1;
    }

    *out = i;
    return 0;
}

static PyObject *get_index(PyObject *o, PyObject *idx) {
    if (o->type == PY_LIST || o->type == PY_TUPLE) {
        long long i;
        if (seq_index(idx, (long long)o->u.seq.len, &i))
            return NULL;

        Py_IncRef(o->u.seq.items[i]);
        return o->u.seq.items[i];
    }

    if (o->type == PY_STR) {
        long long i;
        if (seq_index(idx, (long long)o->u.str.len, &i))
            return NULL;

        return new_str_len(o->u.str.s + i, 1);
    }

    if (o->type == PY_DICT) {
        PyObject *v = dict_get_obj(o, idx);
        if (!v) {
            set_error("KeyError");
            return NULL;
        }
        return v;
    }

    set_error("object is not subscriptable");
    return NULL;
}

static int set_index(PyObject *o, PyObject *idx, PyObject *val) {
    if (o->type == PY_LIST) {
        long long i;
        if (seq_index(idx, (long long)o->u.seq.len, &i))
            return -1;

        Py_IncRef(val);
        Py_DecRef(o->u.seq.items[i]);
        o->u.seq.items[i] = val;
        return 0;
    }

    if (o->type == PY_DICT) {
        dict_set(o, idx, val);
        return 0;
    }

    set_error("object does not support item assignment");
    return -1;
}

/* ---------------------------------------------------------------- */
/* Arithmetic helpers                                                */
/* ---------------------------------------------------------------- */

static long long floor_div_ll(long long a, long long b) {
    long long q = a / b;
    long long r = a % b;

    if (r != 0 && ((r < 0) != (b < 0)))
        q--;

    return q;
}

static long long floor_mod_ll(long long a, long long b) {
    long long r = a % b;

    if (r != 0 && ((r < 0) != (b < 0)))
        r += b;

    return r;
}

static PyObject *do_binop(const char *op, PyObject *l, PyObject *r) {
    /* String concatenation */
    if (!strcmp(op, "+") && l->type == PY_STR && r->type == PY_STR) {
        Buf b;
        buf_init(&b);
        buf_add(&b, l->u.str.s, l->u.str.len);
        buf_add(&b, r->u.str.s, r->u.str.len);

        PyObject *o = new_str_len(b.s, b.len);
        free(b.s);
        return o;
    }

    /* Sequence concatenation */
    if (!strcmp(op, "+") &&
        (l->type == PY_LIST || l->type == PY_TUPLE) &&
        l->type == r->type) {

        PyObject *o = new_seq(l->type, l->u.seq.len + r->u.seq.len);

        for (size_t i = 0; i < l->u.seq.len; i++)
            seq_append(o, l->u.seq.items[i]);

        for (size_t i = 0; i < r->u.seq.len; i++)
            seq_append(o, r->u.seq.items[i]);

        return o;
    }

    /* String repetition */
    if (!strcmp(op, "*")) {
        if (l->type == PY_STR && is_num(r)) {
            long long n = as_long(r);
            if (n < 0) n = 0;

            Buf b;
            buf_init(&b);

            for (long long i = 0; i < n; i++)
                buf_add(&b, l->u.str.s, l->u.str.len);

            PyObject *o = new_str_len(b.s, b.len);
            free(b.s);
            return o;
        }

        if (r->type == PY_STR && is_num(l)) {
            long long n = as_long(l);
            if (n < 0) n = 0;

            Buf b;
            buf_init(&b);

            for (long long i = 0; i < n; i++)
                buf_add(&b, r->u.str.s, r->u.str.len);

            PyObject *o = new_str_len(b.s, b.len);
            free(b.s);
            return o;
        }

        if ((l->type == PY_LIST || l->type == PY_TUPLE) && is_num(r)) {
            long long n = as_long(r);
            if (n < 0) n = 0;

            PyObject *o = new_seq(l->type, 4);

            for (long long k = 0; k < n; k++) {
                for (size_t i = 0; i < l->u.seq.len; i++)
                    seq_append(o, l->u.seq.items[i]);
            }

            return o;
        }

        if ((r->type == PY_LIST || r->type == PY_TUPLE) && is_num(l)) {
            long long n = as_long(l);
            if (n < 0) n = 0;

            PyObject *o = new_seq(r->type, 4);

            for (long long k = 0; k < n; k++) {
                for (size_t i = 0; i < r->u.seq.len; i++)
                    seq_append(o, r->u.seq.items[i]);
            }

            return o;
        }
    }

    if (!is_num(l) || !is_num(r)) {
        set_error("unsupported operand types for %s", op);
        return NULL;
    }

    int int_mode = (l->type == PY_INT || l->type == PY_BOOL) &&
                   (r->type == PY_INT || r->type == PY_BOOL);

    long long li = as_long(l);
    long long ri = as_long(r);

    double ld = as_double(l);
    double rd = as_double(r);

    if (!strcmp(op, "+"))
        return int_mode ? new_int(li + ri) : new_float(ld + rd);

    if (!strcmp(op, "-"))
        return int_mode ? new_int(li - ri) : new_float(ld - rd);

    if (!strcmp(op, "*"))
        return int_mode ? new_int(li * ri) : new_float(ld * rd);

    if (!strcmp(op, "/")) {
        if (rd == 0.0) {
            set_error("ZeroDivisionError");
            return NULL;
        }
        return new_float(ld / rd);
    }

    if (!strcmp(op, "//")) {
        if (rd == 0.0) {
            set_error("ZeroDivisionError");
            return NULL;
        }

        if (int_mode)
            return new_int(floor_div_ll(li, ri));

        return new_float(floor(ld / rd));
    }

    if (!strcmp(op, "%")) {
        if (rd == 0.0) {
            set_error("ZeroDivisionError");
            return NULL;
        }

        if (int_mode)
            return new_int(floor_mod_ll(li, ri));

        double m = fmod(ld, rd);
        if (m != 0.0 && ((m < 0.0) != (rd < 0.0)))
            m += rd;

        return new_float(m);
    }

    if (!strcmp(op, "**")) {
        if (int_mode && ri >= 0) {
            long long res = 1;
            long long base = li;

            for (long long i = 0; i < ri; i++)
                res *= base;

            return new_int(res);
        }

        return new_float(pow(ld, rd));
    }

    set_error("bad operator");
    return NULL;
}

/* ---------------------------------------------------------------- */
/* AST helpers                                                       */
/* ---------------------------------------------------------------- */

static Node *node_new(int kind) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    return n;
}

static void node_add(Node *n, Node *child) {
    if (!child) return;

    if (n->nkids == n->kcap) {
        n->kcap = n->kcap ? n->kcap * 2 : 4;
        n->kids = xrealloc(n->kids, n->kcap * sizeof(Node *));
    }

    n->kids[n->nkids++] = child;
}

static void node_add_name(Node *n, const char *name) {
    if (n->nnames == n->ncap) {
        n->ncap = n->ncap ? n->ncap * 2 : 4;
        n->names = xrealloc(n->names, n->ncap * sizeof(char *));
    }

    n->names[n->nnames++] = xstrdup(name);
}

/* Freeing an AST.
 *
 * Every n->str and every entry of n->names is xstrdup'd, and every child is
 * owned, so this is a plain recursive free with nothing borrowed inside a
 * node.
 *
 * WHAT IS borrowed is the node itself: `def` stores the function body as a
 * RAW POINTER into this tree (see new_func's `o->u.func.body = body`). So an
 * AST must NOT be freed when its script finishes -- any function that script
 * defined would be left pointing at freed memory, and calling it from a later
 * PyRun_SimpleString would be a use-after-free. That is a worse bug than the
 * leak it replaces.
 *
 * So the roots are kept and freed in Py_Finalize, when nothing can call into
 * them any more. That makes the memory BOUNDED and reclaimed rather than lost;
 * it does not make a long-running REPL free each script as it goes, which
 * would need the function object to own or refcount its body. */
static void node_free(Node *n) {
    if (!n) return;

    for (int i = 0; i < n->nkids; i++)
        node_free(n->kids[i]);

    for (int i = 0; i < n->nnames; i++)
        free(n->names[i]);

    free(n->kids);
    free(n->names);
    free(n->str);
    free(n);
}

/* The ASTs this interpreter owns, in the order they were run. */
static Node **mp_roots = NULL;
static int mp_nroots = 0;
static int mp_rootcap = 0;

static void mp_keep_ast(Node *root) {
    if (!root) return;

    if (mp_nroots == mp_rootcap) {
        mp_rootcap = mp_rootcap ? mp_rootcap * 2 : 8;
        mp_roots = xrealloc(mp_roots, mp_rootcap * sizeof(Node *));
    }

    mp_roots[mp_nroots++] = root;
}

static void mp_free_asts(void) {
    for (int i = 0; i < mp_nroots; i++)
        node_free(mp_roots[i]);

    free(mp_roots);
    mp_roots = NULL;
    mp_nroots = 0;
    mp_rootcap = 0;
}

static Node *unary_node(const char *op, Node *a) {
    Node *n = node_new(N_UNARY);
    n->str = xstrdup(op);
    node_add(n, a);
    return n;
}

static Node *binary_node(const char *op, Node *a, Node *b) {
    Node *n = node_new(N_BIN);
    n->str = xstrdup(op);
    node_add(n, a);
    node_add(n, b);
    return n;
}

/* ---------------------------------------------------------------- */
/* Token helpers                                                     */
/* ---------------------------------------------------------------- */

static void add_token(int type, const char *text, int len) {
    if (ntok == cap_toks) {
        cap_toks = cap_toks ? cap_toks * 2 : 256;
        toks = xrealloc(toks, cap_toks * sizeof(Token));
    }

    toks[ntok].type = type;
    toks[ntok].text = xstrndup(text, len);
    ntok++;
}

static void clear_tokens(void) {
    if (toks) {
        for (int i = 0; i < ntok; i++)
            free(toks[i].text);

        free(toks);
        toks = NULL;
    }

    ntok = cap_toks = pos = 0;
}

static Token *cur(void) {
    return &toks[pos];
}

static int tk_is(int type, const char *text) {
    Token *t = cur();

    if (t->type != type)
        return 0;

    if (text && strcmp(t->text, text) != 0)
        return 0;

    return 1;
}

static int tk_accept(int type, const char *text) {
    if (tk_is(type, text)) {
        pos++;
        return 1;
    }
    return 0;
}

static Token *tk_expect(int type, const char *text) {
    if (tk_is(type, text))
        return &toks[pos++];

    set_error("expected %s", text ? text : "token");
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Lexer                                                             */
/* ---------------------------------------------------------------- */

static int lex_src(const char *src) {
    int stack[512];
    int top = 0;
    stack[0] = 0;

    int paren = 0;
    const char *p = src;

    while (*p) {
        const char *line = p;

        while (*p && *p != '\n')
            p++;

        int line_len = (int)(p - line);
        if (*p == '\n')
            p++;

        int eff = line_len;
        int in_str = 0;
        char quote = 0;

        for (int i = 0; i < line_len; i++) {
            char c = line[i];

            if (in_str) {
                if (c == '\\')
                    i++;
                else if (c == quote)
                    in_str = 0;
            } else {
                if (c == '\'' || c == '"') {
                    in_str = 1;
                    quote = c;
                } else if (c == '#') {
                    eff = i;
                    break;
                }
            }
        }

        int first = 0;
        while (first < eff && (line[first] == ' ' || line[first] == '\t'))
            first++;

        if (first >= eff)
            continue;

        if (!paren) {
            int indent = 0;

            for (int i = 0; i < first; i++) {
                if (line[i] == '\t')
                    indent += 8 - (indent % 8);
                else
                    indent++;
            }

            if (indent > stack[top]) {
                if (top >= 511) {
                    set_error("indentation too deep");
                    return -1;
                }

                stack[++top] = indent;
                add_token(TK_INDENT, "", 0);
            }

            while (indent < stack[top]) {
                top--;
                add_token(TK_DEDENT, "", 0);
            }

            if (indent != stack[top]) {
                set_error("indentation error");
                return -1;
            }
        }

        int i = first;

        while (i < eff) {
            unsigned char c = line[i];

            if (isspace(c)) {
                i++;
                continue;
            }

            if (c == '(' || c == '[' || c == '{') {
                paren++;
                add_token(TK_OP, (char *)&c, 1);
                i++;
                continue;
            }

            if (c == ')' || c == ']' || c == '}') {
                if (paren > 0)
                    paren--;

                add_token(TK_OP, (char *)&c, 1);
                i++;
                continue;
            }

            if (c == ';') {
                add_token(TK_NEWLINE, ";", 1);
                i++;
                continue;
            }

            if (isalpha(c) || c == '_') {
                int st = i;
                while (i < eff && (isalnum(line[i]) || line[i] == '_'))
                    i++;

                add_token(TK_NAME, line + st, i - st);
                continue;
            }

            if (isdigit(c) || (c == '.' && i + 1 < eff && isdigit(line[i + 1]))) {
                int st = i;

                while (i < eff && isdigit(line[i]))
                    i++;

                if (i < eff && line[i] == '.') {
                    i++;
                    while (i < eff && isdigit(line[i]))
                        i++;
                }

                if (i < eff && (line[i] == 'e' || line[i] == 'E')) {
                    i++;

                    if (i < eff && (line[i] == '+' || line[i] == '-'))
                        i++;

                    while (i < eff && isdigit(line[i]))
                        i++;
                }

                add_token(TK_NUM, line + st, i - st);
                continue;
            }

            if (c == '\'' || c == '"') {
                char q = c;
                i++;

                Buf b;
                buf_init(&b);

                int bad = 0;

                while (i < eff && line[i] != q) {
                    if (line[i] == '\\' && i + 1 < eff) {
                        i++;
                        char e = line[i];
                        char ch = e;

                        switch (e) {
                            case 'n': ch = '\n'; break;
                            case 't': ch = '\t'; break;
                            case 'r': ch = '\r'; break;
                            case '\\': ch = '\\'; break;
                            case '\'': ch = '\''; break;
                            case '"': ch = '"'; break;
                            default:
                                buf_add(&b, "\\", 1);
                                ch = e;
                                break;
                        }

                        buf_add(&b, &ch, 1);
                        i++;
                    } else {
                        buf_add(&b, (char *)&line[i], 1);
                        i++;
                    }
                }

                if (i >= eff)
                    bad = 1;
                else
                    i++;

                if (bad) {
                    set_error("unterminated string");
                    free(b.s);
                    return -1;
                }

                add_token(TK_STR, b.s, (int)b.len);
                free(b.s);
                continue;
            }

            if (i + 1 < eff) {
                char c2 = line[i + 1];
                char c3 = (i + 2 < eff) ? line[i + 2] : 0;

                if (c == '*' && c2 == '*') {
                    if (c3 == '=') {
                        add_token(TK_OP, "**=", 3);
                        i += 3;
                    } else {
                        add_token(TK_OP, "**", 2);
                        i += 2;
                    }
                    continue;
                }

                if (c == '/' && c2 == '/') {
                    if (c3 == '=') {
                        add_token(TK_OP, "//=", 3);
                        i += 3;
                    } else {
                        add_token(TK_OP, "//", 2);
                        i += 2;
                    }
                    continue;
                }

                if ((c == '=' && c2 == '=') ||
                    (c == '!' && c2 == '=') ||
                    (c == '<' && c2 == '=') ||
                    (c == '>' && c2 == '=') ||
                    (c == '+' && c2 == '=') ||
                    (c == '-' && c2 == '=') ||
                    (c == '*' && c2 == '=') ||
                    (c == '/' && c2 == '=') ||
                    (c == '%' && c2 == '=')) {
                    add_token(TK_OP, line + i, 2);
                    i += 2;
                    continue;
                }
            }

            if (strchr("+-*/%<>=!.,:()", c)) {
                add_token(TK_OP, (char *)&c, 1);
                i++;
                continue;
            }

            set_error("unexpected character '%c'", c);
            return -1;
        }

        if (paren == 0)
            add_token(TK_NEWLINE, "\\n", 1);
    }

    if (paren != 0) {
        set_error("unclosed delimiter");
        return -1;
    }

    add_token(TK_NEWLINE, "\\n", 1);

    while (top > 0) {
        add_token(TK_DEDENT, "", 0);
        top--;
    }

    add_token(TK_END, "", 0);
    return 0;
}

/* ---------------------------------------------------------------- */
/* Parser                                                            */
/* ---------------------------------------------------------------- */

static Node *parse_expr(void);
static Node *parse_statement(void);
static Node *parse_simple_stmt(void);
static Node *parse_comparison(void);
static Node *parse_and(void);
static Node *parse_not(void);

static Node *parse_or(void) {
    Node *l = parse_and();
    if (!l) return NULL;

    while (tk_is(TK_NAME, "or")) {
        pos++;
        Node *r = parse_and();
        if (!r) return NULL;
        l = binary_node("or", l, r);
    }

    return l;
}

static Node *parse_and(void) {
    Node *l = parse_not();
    if (!l) return NULL;

    while (tk_is(TK_NAME, "and")) {
        pos++;
        Node *r = parse_not();
        if (!r) return NULL;
        l = binary_node("and", l, r);
    }

    return l;
}

static Node *parse_not(void) {
    if (tk_is(TK_NAME, "not")) {
        pos++;
        Node *e = parse_not();
        if (!e) return NULL;
        return unary_node("not", e);
    }

    return parse_comparison();
}

static Node *parse_add(void);
static Node *parse_mul(void);
static Node *parse_unary(void);
static Node *parse_power(void);
static Node *parse_postfix(void);
static Node *parse_atom(void);

static Node *parse_comparison(void) {
    Node *l = parse_add();
    if (!l) return NULL;

    for (;;) {
        const char *op = NULL;

        if (tk_is(TK_OP, "==") || tk_is(TK_OP, "!=") ||
            tk_is(TK_OP, "<") || tk_is(TK_OP, ">") ||
            tk_is(TK_OP, "<=") || tk_is(TK_OP, ">=")) {
            op = cur()->text;
            pos++;
        } else if (tk_is(TK_NAME, "in")) {
            op = "in";
            pos++;
        } else if (tk_is(TK_NAME, "is")) {
            op = "is";
            pos++;

            if (tk_accept(TK_NAME, "not"))
                op = "is not";
        } else if (tk_is(TK_NAME, "not") &&
                   pos + 1 < ntok &&
                   toks[pos + 1].type == TK_NAME &&
                   !strcmp(toks[pos + 1].text, "in")) {
            pos += 2;
            op = "not in";
        } else {
            break;
        }

        Node *r = parse_add();
        if (!r) return NULL;

        l = binary_node(op, l, r);
    }

    return l;
}

static Node *parse_add(void) {
    Node *l = parse_mul();
    if (!l) return NULL;

    while (tk_is(TK_OP, "+") || tk_is(TK_OP, "-")) {
        const char *op = cur()->text;
        pos++;

        Node *r = parse_mul();
        if (!r) return NULL;

        l = binary_node(op, l, r);
    }

    return l;
}

static Node *parse_mul(void) {
    Node *l = parse_unary();
    if (!l) return NULL;

    while (tk_is(TK_OP, "*") || tk_is(TK_OP, "/") ||
           tk_is(TK_OP, "//") || tk_is(TK_OP, "%")) {
        const char *op = cur()->text;
        pos++;

        Node *r = parse_unary();
        if (!r) return NULL;

        l = binary_node(op, l, r);
    }

    return l;
}

static Node *parse_unary(void) {
    if (tk_is(TK_OP, "-") || tk_is(TK_OP, "+")) {
        const char *op = cur()->text;
        pos++;

        Node *e = parse_unary();
        if (!e) return NULL;

        return unary_node(op, e);
    }

    return parse_power();
}

static Node *parse_power(void) {
    Node *l = parse_postfix();
    if (!l) return NULL;

    if (tk_is(TK_OP, "**")) {
        pos++;

        Node *r = parse_unary();
        if (!r) return NULL;

        l = binary_node("**", l, r);
    }

    return l;
}

static Node *parse_postfix(void) {
    Node *atom = parse_atom();
    if (!atom) return NULL;

    for (;;) {
        if (tk_is(TK_OP, "(")) {
            pos++;

            Node *call = node_new(N_CALL);
            node_add(call, atom);

            if (!tk_is(TK_OP, ")")) {
                for (;;) {
                    Node *arg = parse_expr();
                    if (!arg) return NULL;

                    node_add(call, arg);

                    if (tk_accept(TK_OP, ",")) {
                        if (tk_is(TK_OP, ")"))
                            break;
                        continue;
                    }

                    break;
                }
            }

            if (!tk_expect(TK_OP, ")"))
                return NULL;

            atom = call;
        } else if (tk_is(TK_OP, "[")) {
            pos++;

            /* A subscript is either [i] or a slice [lo:hi], and either end of a
               slice may be omitted: [:n], [n:], even [:]. So the colon is what
               decides which node this is, and it can appear before any
               expression has been seen at all. */
            Node *lo = NULL;

            if (!tk_is(TK_OP, ":")) {
                lo = parse_expr();
                if (!lo) return NULL;
            }

            if (tk_is(TK_OP, ":")) {
                pos++;

                Node *hi = NULL;
                if (!tk_is(TK_OP, "]")) {
                    hi = parse_expr();
                    if (!hi) return NULL;
                }

                if (!tk_expect(TK_OP, "]"))
                    return NULL;

                Node *sl = node_new(N_SLICE);
                node_add(sl, atom);
                /* A missing bound is stored as N_NONE rather than as a null
                   child, so the evaluator never has to check nkids to know
                   what it is holding. */
                node_add(sl, lo ? lo : node_new(N_NONE));
                node_add(sl, hi ? hi : node_new(N_NONE));

                atom = sl;
                continue;
            }

            if (!lo) {
                set_error("empty subscript");
                return NULL;
            }

            if (!tk_expect(TK_OP, "]"))
                return NULL;

            Node *n = node_new(N_INDEX);
            node_add(n, atom);
            node_add(n, lo);

            atom = n;
        } else if (tk_is(TK_OP, ".")) {
            pos++;

            Token *name = tk_expect(TK_NAME, NULL);
            if (!name) return NULL;

            Node *n = node_new(N_ATTR);
            n->str = xstrdup(name->text);
            node_add(n, atom);

            atom = n;
        } else {
            break;
        }
    }

    return atom;
}

static Node *parse_atom(void) {
    if (tk_is(TK_NUM, NULL)) {
        Token *t = cur();
        pos++;

        Node *n;

        if (strpbrk(t->text, ".eE")) {
            n = node_new(N_FLOAT);
            n->fval = strtod(t->text, NULL);
        } else {
            n = node_new(N_INT);
            n->ival = strtoll(t->text, NULL, 10);
        }

        return n;
    }

    if (tk_is(TK_STR, NULL)) {
        Token *t = cur();
        pos++;

        Node *n = node_new(N_STR);
        n->str = xstrdup(t->text);
        return n;
    }

    if (tk_is(TK_NAME, NULL)) {
        Token *t = cur();
        pos++;

        if (!strcmp(t->text, "True"))
            return node_new(N_TRUE);

        if (!strcmp(t->text, "False"))
            return node_new(N_FALSE);

        if (!strcmp(t->text, "None"))
            return node_new(N_NONE);

        Node *n = node_new(N_NAME);
        n->str = xstrdup(t->text);
        return n;
    }

    if (tk_is(TK_OP, "[")) {
        pos++;

        if (tk_accept(TK_OP, "]"))
            return node_new(N_LIST);

        Node *first = parse_expr();
        if (!first) return NULL;

        /* The N_LIST node is NOT allocated until we know this really is a
           list. Allocating it up front and then returning an N_COMP instead
           orphans it -- which is exactly what the first version of this did,
           for one leaked node per comprehension. */

        /* [expr for name in iterable]  and  [expr for name in iterable if c].
           Decided here, after the first element, because that is the first
           point at which a comprehension is distinguishable from a plain list
           -- `[x` could still become either. */
        if (tk_is(TK_NAME, "for")) {
            pos++;

            if (!tk_is(TK_NAME, NULL) || cur()->type != TK_NAME) {
                set_error("expected a name after 'for'");
                return NULL;
            }

            char *var = cur()->text;
            pos++;

            if (!tk_expect(TK_NAME, "in"))
                return NULL;

            Node *iter = parse_or();          /* not parse_expr: a trailing
                                                 `if` here is the filter, not a
                                                 conditional expression */
            if (!iter) return NULL;

            Node *cond = NULL;
            if (tk_is(TK_NAME, "if")) {
                pos++;
                cond = parse_or();
                if (!cond) return NULL;
            }

            if (!tk_expect(TK_OP, "]"))
                return NULL;

            Node *c = node_new(N_COMP);
            c->str = xstrdup(var);
            node_add(c, first);
            node_add(c, iter);
            node_add(c, cond ? cond : node_new(N_NONE));
            return c;
        }

        Node *l = node_new(N_LIST);
        node_add(l, first);

        while (tk_accept(TK_OP, ",")) {
            if (tk_is(TK_OP, "]"))
                break;

            Node *e = parse_expr();
            if (!e) return NULL;

            node_add(l, e);
        }

        if (!tk_expect(TK_OP, "]"))
            return NULL;

        return l;
    }

    if (tk_is(TK_OP, "(")) {
        pos++;

        if (tk_accept(TK_OP, ")"))
            return node_new(N_TUPLE);

        Node *first = parse_expr();
        if (!first) return NULL;

        if (tk_is(TK_OP, ",")) {
            Node *t = node_new(N_TUPLE);
            node_add(t, first);

            while (tk_accept(TK_OP, ",")) {
                if (tk_is(TK_OP, ")"))
                    break;

                Node *e = parse_expr();
                if (!e) return NULL;

                node_add(t, e);
            }

            if (!tk_expect(TK_OP, ")"))
                return NULL;

            return t;
        }

        if (!tk_expect(TK_OP, ")"))
            return NULL;

        return first;
    }

    if (tk_is(TK_OP, "{")) {
        pos++;

        Node *d = node_new(N_DICT);

        if (tk_accept(TK_OP, "}"))
            return d;

        for (;;) {
            Node *k = parse_expr();
            if (!k) return NULL;

            if (!tk_expect(TK_OP, ":"))
                return NULL;

            Node *v = parse_expr();
            if (!v) return NULL;

            node_add(d, k);
            node_add(d, v);

            if (tk_accept(TK_OP, ",")) {
                if (tk_is(TK_OP, "}"))
                    break;
                continue;
            }

            break;
        }

        if (!tk_expect(TK_OP, "}"))
            return NULL;

        return d;
    }

    set_error("invalid syntax");
    return NULL;
}

/* The conditional expression, `a if c else b`.
 *
 * Python puts it BELOW `or` in precedence -- `x if a or b else y` groups the
 * condition as `(a or b)` -- so it goes here, between parse_expr and
 * parse_or, and its three parts are parsed at the level below it. The `else`
 * branch is parsed at THIS level so that `a if c else b if d else e` chains to
 * the right, which is what Python does. */
static Node *parse_expr(void) {
    Node *l = parse_or();
    if (!l) return NULL;

    if (tk_is(TK_NAME, "if")) {
        pos++;

        Node *cond = parse_or();
        if (!cond) return NULL;

        if (!tk_expect(TK_NAME, "else"))
            return NULL;

        Node *other = parse_expr();
        if (!other) return NULL;

        Node *n = node_new(N_COND);
        node_add(n, cond);
        node_add(n, l);
        node_add(n, other);
        return n;
    }

    return l;
}

static Node *parse_suite(void) {
    if (!tk_expect(TK_NEWLINE, NULL))
        return NULL;

    if (!tk_expect(TK_INDENT, NULL))
        return NULL;

    Node *suite = node_new(N_SUITE);

    while (!tk_is(TK_DEDENT, NULL) && !tk_is(TK_END, NULL)) {
        while (tk_accept(TK_NEWLINE, NULL))
            ;

        if (tk_is(TK_DEDENT, NULL) || tk_is(TK_END, NULL))
            break;

        Node *st = parse_statement();
        if (!st) return NULL;

        node_add(suite, st);
    }

    if (!tk_expect(TK_DEDENT, NULL))
        return NULL;

    return suite;
}

static Node *parse_if(void) {
    pos++; /* if or elif */

    Node *n = node_new(N_IF);

    Node *cond = parse_expr();
    if (!cond) return NULL;

    if (!tk_expect(TK_OP, ":"))
        return NULL;

    Node *body = parse_suite();
    if (!body) return NULL;

    node_add(n, cond);
    node_add(n, body);

    if (tk_is(TK_NAME, "elif")) {
        Node *elif = parse_if();
        if (!elif) return NULL;

        node_add(n, elif);
    } else if (tk_is(TK_NAME, "else")) {
        pos++;

        if (!tk_expect(TK_OP, ":"))
            return NULL;

        Node *else_body = parse_suite();
        if (!else_body) return NULL;

        node_add(n, else_body);
    }

    return n;
}

static Node *parse_while(void) {
    pos++;

    Node *n = node_new(N_WHILE);

    Node *cond = parse_expr();
    if (!cond) return NULL;

    if (!tk_expect(TK_OP, ":"))
        return NULL;

    Node *body = parse_suite();
    if (!body) return NULL;

    node_add(n, cond);
    node_add(n, body);

    return n;
}

static Node *parse_for(void) {
    pos++;

    Token *var = tk_expect(TK_NAME, NULL);
    if (!var) return NULL;

    if (!tk_expect(TK_NAME, "in"))
        return NULL;

    Node *iter = parse_expr();
    if (!iter) return NULL;

    if (!tk_expect(TK_OP, ":"))
        return NULL;

    Node *body = parse_suite();
    if (!body) return NULL;

    Node *n = node_new(N_FOR);
    n->str = xstrdup(var->text);

    node_add(n, iter);
    node_add(n, body);

    return n;
}

static Node *parse_def(void) {
    pos++;

    Token *name = tk_expect(TK_NAME, NULL);
    if (!name) return NULL;

    if (!tk_expect(TK_OP, "("))
        return NULL;

    Node *n = node_new(N_DEF);
    n->str = xstrdup(name->text);

    if (!tk_is(TK_OP, ")")) {
        for (;;) {
            Token *p = tk_expect(TK_NAME, NULL);
            if (!p) return NULL;

            node_add_name(n, p->text);

            if (tk_accept(TK_OP, ",")) {
                if (tk_is(TK_OP, ")"))
                    break;
                continue;
            }

            break;
        }
    }

    if (!tk_expect(TK_OP, ")"))
        return NULL;

    if (!tk_expect(TK_OP, ":"))
        return NULL;

    Node *body = parse_suite();
    if (!body) return NULL;

    node_add(n, body);

    return n;
}

static Node *parse_simple_stmt(void) {
    if (tk_is(TK_NAME, "return")) {
        pos++;

        Node *n = node_new(N_RETURN);

        if (!tk_is(TK_NEWLINE, NULL) && !tk_is(TK_END, NULL)) {
            Node *e = parse_expr();
            if (!e) return NULL;

            node_add(n, e);
        }

        return n;
    }

    if (tk_is(TK_NAME, "break")) {
        pos++;
        return node_new(N_BREAK);
    }

    if (tk_is(TK_NAME, "continue")) {
        pos++;
        return node_new(N_CONTINUE);
    }

    if (tk_is(TK_NAME, "pass")) {
        pos++;
        return node_new(N_PASS);
    }

    Node *e = parse_expr();
    if (!e) return NULL;

    if (tk_is(TK_OP, "=") ||
        tk_is(TK_OP, "+=") ||
        tk_is(TK_OP, "-=") ||
        tk_is(TK_OP, "*=") ||
        tk_is(TK_OP, "/=") ||
        tk_is(TK_OP, "%=") ||
        tk_is(TK_OP, "//=") ||
        tk_is(TK_OP, "**=")) {
        const char *op = cur()->text;
        pos++;

        Node *v = parse_expr();
        if (!v) return NULL;

        Node *n = node_new(N_ASSIGN);
        n->str = xstrdup(op);

        node_add(n, e);
        node_add(n, v);

        return n;
    }

    Node *n = node_new(N_EXPR_STMT);
    node_add(n, e);
    return n;
}

static Node *parse_statement(void) {
    if (tk_is(TK_NAME, "if"))
        return parse_if();

    if (tk_is(TK_NAME, "while"))
        return parse_while();

    if (tk_is(TK_NAME, "for"))
        return parse_for();

    if (tk_is(TK_NAME, "def"))
        return parse_def();

    Node *s = parse_simple_stmt();
    if (!s) return NULL;

    if (!tk_expect(TK_NEWLINE, NULL))
        return NULL;

    return s;
}

static Node *parse_program(void) {
    Node *p = node_new(N_SUITE);

    while (!tk_is(TK_END, NULL)) {
        while (tk_accept(TK_NEWLINE, NULL))
            ;

        if (tk_is(TK_END, NULL))
            break;

        if (tk_accept(TK_DEDENT, NULL))
            continue;

        Node *s = parse_statement();
        if (!s) return NULL;

        node_add(p, s);
    }

    return p;
}

/* ---------------------------------------------------------------- */
/* Interpreter forward declarations                                  */
/* ---------------------------------------------------------------- */

static PyObject *eval(Node *n, Env *env);
static int exec(Node *n, Env *env, PyObject **ret);
static PyObject *call_object(PyObject *f, PyObject *args);
static PyObject *call_method(PyObject *obj, const char *name, PyObject *args);
static int exec_assign(Node *target, PyObject *val, Env *env);
static PyObject *eval_target_value(Node *target, Env *env);

/* ---------------------------------------------------------------- */
/* Expression evaluation                                             */
/* ---------------------------------------------------------------- */

static PyObject *eval_call(Node *n, Env *env) {
    Node *callee = n->kids[0];

    if (callee->kind == N_ATTR) {
        PyObject *obj = eval(callee->kids[0], env);
        if (!obj) return NULL;

        PyObject *args = new_list();

        for (int i = 1; i < n->nkids; i++) {
            PyObject *a = eval(n->kids[i], env);
            if (!a) {
                Py_DecRef(obj);
                Py_DecRef(args);
                return NULL;
            }

            seq_append(args, a);
            Py_DecRef(a);
        }

        PyObject *res = call_method(obj, callee->str, args);

        Py_DecRef(obj);
        Py_DecRef(args);

        return res;
    }

    PyObject *f = eval(callee, env);
    if (!f) return NULL;

    PyObject *args = new_list();

    for (int i = 1; i < n->nkids; i++) {
        PyObject *a = eval(n->kids[i], env);
        if (!a) {
            Py_DecRef(f);
            Py_DecRef(args);
            return NULL;
        }

        seq_append(args, a);
        Py_DecRef(a);
    }

    PyObject *res = call_object(f, args);

    Py_DecRef(f);
    Py_DecRef(args);

    return res;
}

static PyObject *eval(Node *n, Env *env) {
    switch (n->kind) {
        case N_INT:
            return new_int(n->ival);

        case N_FLOAT:
            return new_float(n->fval);

        case N_STR:
            return new_str(n->str);

        case N_TRUE:
            return Py_True;

        case N_FALSE:
            return Py_False;

        case N_NONE:
            return Py_None;

        case N_NAME: {
            PyObject *v = env_get(env, n->str);
            if (!v) {
                set_error("NameError: name '%s' is not defined", n->str);
                return NULL;
            }
            return v;
        }

        case N_LIST: {
            PyObject *l = new_list();

            for (int i = 0; i < n->nkids; i++) {
                PyObject *v = eval(n->kids[i], env);
                if (!v) {
                    Py_DecRef(l);
                    return NULL;
                }

                seq_append(l, v);
                Py_DecRef(v);
            }

            return l;
        }

        case N_TUPLE: {
            PyObject *t = new_tuple();

            for (int i = 0; i < n->nkids; i++) {
                PyObject *v = eval(n->kids[i], env);
                if (!v) {
                    Py_DecRef(t);
                    return NULL;
                }

                seq_append(t, v);
                Py_DecRef(v);
            }

            return t;
        }

        case N_DICT: {
            PyObject *d = new_dict();

            for (int i = 0; i + 1 < n->nkids; i += 2) {
                PyObject *k = eval(n->kids[i], env);
                if (!k) {
                    Py_DecRef(d);
                    return NULL;
                }

                PyObject *v = eval(n->kids[i + 1], env);
                if (!v) {
                    Py_DecRef(k);
                    Py_DecRef(d);
                    return NULL;
                }

                dict_set(d, k, v);

                Py_DecRef(k);
                Py_DecRef(v);
            }

            return d;
        }

        case N_BIN: {
            const char *op = n->str;

            if (!strcmp(op, "and")) {
                PyObject *l = eval(n->kids[0], env);
                if (!l) return NULL;

                if (!obj_true(l))
                    return l;

                Py_DecRef(l);
                return eval(n->kids[1], env);
            }

            if (!strcmp(op, "or")) {
                PyObject *l = eval(n->kids[0], env);
                if (!l) return NULL;

                if (obj_true(l))
                    return l;

                Py_DecRef(l);
                return eval(n->kids[1], env);
            }

            PyObject *l = eval(n->kids[0], env);
            if (!l) return NULL;

            PyObject *r = eval(n->kids[1], env);
            if (!r) {
                Py_DecRef(l);
                return NULL;
            }

            PyObject *res = NULL;

            if (!strcmp(op, "==")) {
                res = new_bool(obj_eq(l, r));
            } else if (!strcmp(op, "!=")) {
                res = new_bool(!obj_eq(l, r));
            } else if (!strcmp(op, "<")) {
                int v = obj_less(l, r);
                if (v >= 0) res = new_bool(v);
            } else if (!strcmp(op, "<=")) {
                int v = obj_less(l, r);
                if (v >= 0) res = new_bool(v || obj_eq(l, r));
            } else if (!strcmp(op, ">")) {
                int v = obj_less(r, l);
                if (v >= 0) res = new_bool(v);
            } else if (!strcmp(op, ">=")) {
                int v = obj_less(r, l);
                if (v >= 0) res = new_bool(v || obj_eq(r, l));
            } else if (!strcmp(op, "in")) {
                int c = obj_contains(r, l);
                if (c >= 0) res = new_bool(c);
            } else if (!strcmp(op, "not in")) {
                int c = obj_contains(r, l);
                if (c >= 0) res = new_bool(!c);
            } else if (!strcmp(op, "is")) {
                res = new_bool(l == r);
            } else if (!strcmp(op, "is not")) {
                res = new_bool(l != r);
            } else {
                res = do_binop(op, l, r);
            }

            Py_DecRef(l);
            Py_DecRef(r);

            return res;
        }

        case N_UNARY: {
            PyObject *v = eval(n->kids[0], env);
            if (!v) return NULL;

            PyObject *res = NULL;

            if (!strcmp(n->str, "not")) {
                res = new_bool(!obj_true(v));
            } else if (!strcmp(n->str, "-")) {
                if (!is_num(v)) {
                    set_error("bad operand type for unary -");
                } else if (v->type == PY_FLOAT) {
                    res = new_float(-v->u.f);
                } else {
                    res = new_int(-as_long(v));
                }
            } else if (!strcmp(n->str, "+")) {
                if (!is_num(v)) {
                    set_error("bad operand type for unary +");
                } else if (v->type == PY_FLOAT) {
                    res = new_float(v->u.f);
                } else {
                    res = new_int(as_long(v));
                }
            } else {
                set_error("bad unary operator");
            }

            Py_DecRef(v);
            return res;
        }

        case N_CALL:
            return eval_call(n, env);

        case N_INDEX: {
            PyObject *obj = eval(n->kids[0], env);
            if (!obj) return NULL;

            PyObject *idx = eval(n->kids[1], env);
            if (!idx) {
                Py_DecRef(obj);
                return NULL;
            }

            PyObject *res = get_index(obj, idx);

            Py_DecRef(obj);
            Py_DecRef(idx);

            return res;
        }

        /* a if c else b. Only the branch that is taken is evaluated, which is
           the whole reason this is an expression form and not a function --
           `x[0] if x else None` has to be safe when x is empty. */
        case N_COND: {
            PyObject *c = eval(n->kids[0], env);
            if (!c) return NULL;

            int t = obj_true(c);
            Py_DecRef(c);

            return eval(n->kids[t ? 1 : 2], env);
        }

        /* obj[lo:hi], with either bound omitted.
         *
         * Slice bounds are NOT index bounds and the difference matters: an
         * index out of range is an error, a slice bound out of range is
         * clamped. `[1,2,3][0:99]` is the whole list, not a failure. Negative
         * bounds count from the end, and after that clamping still applies. */
        case N_SLICE: {
            PyObject *obj = eval(n->kids[0], env);
            if (!obj) return NULL;

            long long len;
            if (obj->type == PY_LIST || obj->type == PY_TUPLE)
                len = (long long)obj->u.seq.len;
            else if (obj->type == PY_STR)
                len = (long long)obj->u.str.len;
            else {
                set_error("object is not sliceable");
                Py_DecRef(obj);
                return NULL;
            }

            long long lo = 0, hi = len;

            if (n->kids[1]->kind != N_NONE) {
                PyObject *v = eval(n->kids[1], env);
                if (!v) { Py_DecRef(obj); return NULL; }
                if (v->type != PY_INT && v->type != PY_BOOL) {
                    set_error("slice indices must be integers");
                    Py_DecRef(v); Py_DecRef(obj);
                    return NULL;
                }
                lo = (v->type == PY_INT) ? v->u.i : (long long)v->u.b;
                Py_DecRef(v);
            }

            if (n->kids[2]->kind != N_NONE) {
                PyObject *v = eval(n->kids[2], env);
                if (!v) { Py_DecRef(obj); return NULL; }
                if (v->type != PY_INT && v->type != PY_BOOL) {
                    set_error("slice indices must be integers");
                    Py_DecRef(v); Py_DecRef(obj);
                    return NULL;
                }
                hi = (v->type == PY_INT) ? v->u.i : (long long)v->u.b;
                Py_DecRef(v);
            }

            if (lo < 0) lo += len;
            if (hi < 0) hi += len;
            if (lo < 0) lo = 0;
            if (hi > len) hi = len;
            if (hi < lo) hi = lo;

            if (obj->type == PY_STR) {
                PyObject *r = new_str_len(obj->u.str.s + lo, (size_t)(hi - lo));
                Py_DecRef(obj);
                return r;
            }

            {
                PyObject *r = new_list();
                for (long long i = lo; i < hi; i++)
                    seq_append(r, obj->u.seq.items[i]);
                Py_DecRef(obj);
                return r;
            }
        }

        /* [expr for name in iterable] and [expr for name in iterable if cond].
         *
         * The loop variable is set in the ENCLOSING environment, which is what
         * Python 2 did and what this interpreter's `for` statement already
         * does -- matching the statement rather than inventing a second scope
         * rule for the expression form. */
        case N_COMP: {
            PyObject *coll = eval(n->kids[1], env);
            if (!coll) return NULL;

            PyObject *out = new_list();
            long long len;

            if (coll->type == PY_LIST || coll->type == PY_TUPLE)
                len = (long long)coll->u.seq.len;
            else if (coll->type == PY_STR)
                len = (long long)coll->u.str.len;
            else {
                set_error("object is not iterable");
                Py_DecRef(out);
                Py_DecRef(coll);
                return NULL;
            }

            for (long long i = 0; i < len; i++) {
                PyObject *item;

                if (coll->type == PY_STR)
                    item = new_str_len(coll->u.str.s + i, 1);
                else {
                    item = coll->u.seq.items[i];
                    Py_IncRef(item);
                }

                env_set_local(env, n->str, item);
                Py_DecRef(item);

                if (n->kids[2]->kind != N_NONE) {
                    PyObject *c = eval(n->kids[2], env);
                    if (!c) { Py_DecRef(out); Py_DecRef(coll); return NULL; }

                    int keep = obj_true(c);
                    Py_DecRef(c);

                    if (!keep) continue;
                }

                {
                    PyObject *v = eval(n->kids[0], env);
                    if (!v) { Py_DecRef(out); Py_DecRef(coll); return NULL; }

                    seq_append(out, v);
                    Py_DecRef(v);
                }
            }

            Py_DecRef(coll);
            return out;
        }

        case N_ATTR:
            set_error("attribute access only supported for method calls");
            return NULL;

        default:
            set_error("cannot evaluate node");
            return NULL;
    }
}

/* ---------------------------------------------------------------- */
/* Assignment helpers                                                */
/* ---------------------------------------------------------------- */

static PyObject *eval_target_value(Node *target, Env *env) {
    if (target->kind == N_NAME) {
        PyObject *v = env_get(env, target->str);
        if (!v)
            set_error("name '%s' is not defined", target->str);
        return v;
    }

    if (target->kind == N_INDEX) {
        PyObject *obj = eval(target->kids[0], env);
        if (!obj) return NULL;

        PyObject *idx = eval(target->kids[1], env);
        if (!idx) {
            Py_DecRef(obj);
            return NULL;
        }

        PyObject *v = get_index(obj, idx);

        Py_DecRef(obj);
        Py_DecRef(idx);

        return v;
    }

    set_error("invalid assignment target");
    return NULL;
}

static int exec_assign(Node *target, PyObject *val, Env *env) {
    if (target->kind == N_NAME) {
        env_set_local(env, target->str, val);
        return 0;
    }

    if (target->kind == N_INDEX) {
        PyObject *obj = eval(target->kids[0], env);
        if (!obj) return -1;

        PyObject *idx = eval(target->kids[1], env);
        if (!idx) {
            Py_DecRef(obj);
            return -1;
        }

        int rc = set_index(obj, idx, val);

        Py_DecRef(obj);
        Py_DecRef(idx);

        return rc;
    }

    set_error("invalid assignment target");
    return -1;
}

/* ---------------------------------------------------------------- */
/* Statement execution                                               */
/* ---------------------------------------------------------------- */

static int exec(Node *n, Env *env, PyObject **ret) {
    if (!n) return S_OK;

    switch (n->kind) {
        case N_SUITE: {
            for (int i = 0; i < n->nkids; i++) {
                int st = exec(n->kids[i], env, ret);
                if (st != S_OK)
                    return st;
            }
            return S_OK;
        }

        case N_EXPR_STMT: {
            PyObject *v = eval(n->kids[0], env);
            if (!v) return S_ERR;

            Py_DecRef(v);
            return S_OK;
        }

        case N_ASSIGN: {
            PyObject *rhs = eval(n->kids[1], env);
            if (!rhs) return S_ERR;

            const char *op = n->str;

            if (!op || !strcmp(op, "=")) {
                if (exec_assign(n->kids[0], rhs, env) < 0) {
                    Py_DecRef(rhs);
                    return S_ERR;
                }

                Py_DecRef(rhs);
                return S_OK;
            }

            PyObject *old = eval_target_value(n->kids[0], env);
            if (!old) {
                Py_DecRef(rhs);
                return S_ERR;
            }

            char base[16];
            size_t olen = strlen(op);

            if (olen >= sizeof(base))
                olen = sizeof(base);

            memcpy(base, op, olen - 1);
            base[olen - 1] = 0;

            PyObject *newv = do_binop(base, old, rhs);
            if (!newv) {
                Py_DecRef(old);
                Py_DecRef(rhs);
                return S_ERR;
            }

            int rc = exec_assign(n->kids[0], newv, env);

            Py_DecRef(old);
            Py_DecRef(rhs);
            Py_DecRef(newv);

            return rc < 0 ? S_ERR : S_OK;
        }

        case N_IF: {
            PyObject *c = eval(n->kids[0], env);
            if (!c) return S_ERR;

            int t = obj_true(c);
            Py_DecRef(c);

            if (t)
                return exec(n->kids[1], env, ret);

            if (n->nkids > 2)
                return exec(n->kids[2], env, ret);

            return S_OK;
        }

        case N_WHILE: {
            for (;;) {
                PyObject *c = eval(n->kids[0], env);
                if (!c) return S_ERR;

                int t = obj_true(c);
                Py_DecRef(c);

                if (!t)
                    break;

                int st = exec(n->kids[1], env, ret);

                if (st == S_BREAK)
                    break;

                if (st == S_CONT)
                    continue;

                if (st != S_OK)
                    return st;
            }

            return S_OK;
        }

        case N_FOR: {
            PyObject *coll = eval(n->kids[0], env);
            if (!coll) return S_ERR;

            int st = S_OK;

            if (coll->type == PY_LIST || coll->type == PY_TUPLE) {
                for (size_t i = 0; i < coll->u.seq.len; i++) {
                    env_set_local(env, n->str, coll->u.seq.items[i]);

                    st = exec(n->kids[1], env, ret);

                    if (st == S_BREAK) {
                        st = S_OK;
                        break;
                    }

                    if (st == S_CONT) {
                        st = S_OK;
                        continue;
                    }

                    if (st != S_OK)
                        break;
                }
            } else if (coll->type == PY_STR) {
                for (size_t i = 0; i < coll->u.str.len; i++) {
                    PyObject *ch = new_str_len(coll->u.str.s + i, 1);

                    env_set_local(env, n->str, ch);
                    Py_DecRef(ch);

                    st = exec(n->kids[1], env, ret);

                    if (st == S_BREAK) {
                        st = S_OK;
                        break;
                    }

                    if (st == S_CONT) {
                        st = S_OK;
                        continue;
                    }

                    if (st != S_OK)
                        break;
                }
            } else if (coll->type == PY_DICT) {
                for (size_t i = 0; i < coll->u.dict.len; i++) {
                    env_set_local(env, n->str, coll->u.dict.keys[i]);

                    st = exec(n->kids[1], env, ret);

                    if (st == S_BREAK) {
                        st = S_OK;
                        break;
                    }

                    if (st == S_CONT) {
                        st = S_OK;
                        continue;
                    }

                    if (st != S_OK)
                        break;
                }
            } else {
                set_error("object is not iterable");
                st = S_ERR;
            }

            Py_DecRef(coll);
            return st;
        }

        case N_DEF: {
            PyObject *f = new_func(n->str, n->kids[0], n->names, n->nnames, env);
            env_set_local(env, n->str, f);
            Py_DecRef(f);
            return S_OK;
        }

        case N_RETURN: {
            PyObject *v = NULL;

            if (n->nkids > 0) {
                v = eval(n->kids[0], env);
                if (!v) return S_ERR;
            } else {
                v = Py_None;
            }

            if (ret)
                *ret = v;
            else
                Py_DecRef(v);

            return S_RET;
        }

        case N_BREAK:
            return S_BREAK;

        case N_CONTINUE:
            return S_CONT;

        case N_PASS:
            return S_OK;

        default:
            set_error("invalid statement");
            return S_ERR;
    }
}

/* ---------------------------------------------------------------- */
/* Calls                                                             */
/* ---------------------------------------------------------------- */

static PyObject *call_object(PyObject *f, PyObject *args) {
    if (!f) return NULL;

    if (f->type == PY_BUILTIN)
        return f->u.builtin.fn(args);

    if (f->type == PY_FUNC) {
        if ((int)args->u.seq.len != f->u.func.nparams) {
            set_error("function %s expects %d arguments, got %d",
                      f->u.func.name,
                      f->u.func.nparams,
                      (int)args->u.seq.len);
            return NULL;
        }

        Env *e = env_new(f->u.func.closure);

        for (int i = 0; i < f->u.func.nparams; i++)
            env_set_local(e, f->u.func.params[i], args->u.seq.items[i]);

        PyObject *ret = NULL;
        int st = exec(f->u.func.body, e, &ret);

        env_decref(e);

        if (st == S_RET) {
            if (!ret)
                return Py_None;
            return ret;
        }

        if (st == S_OK)
            return Py_None;

        if (st == S_BREAK || st == S_CONT)
            set_error("break/continue outside loop");

        return NULL;
    }

    set_error("object is not callable");
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Sorting helper                                                    */
/* ---------------------------------------------------------------- */

static int sort_list(PyObject *l) {
    for (size_t i = 1; i < l->u.seq.len; i++) {
        PyObject *x = l->u.seq.items[i];
        int j = (int)i - 1;

        while (j >= 0) {
            int less = obj_less(x, l->u.seq.items[j]);
            if (less < 0)
                return -1;

            if (!less)
                break;

            l->u.seq.items[j + 1] = l->u.seq.items[j];
            j--;
        }

        l->u.seq.items[j + 1] = x;
    }

    return 0;
}

/* ---------------------------------------------------------------- */
/* Iterable helper                                                   */
/* ---------------------------------------------------------------- */

static PyObject *iterable_to_list(PyObject *o) {
    PyObject *l = new_list();

    if (!o) {
        set_error("no iterable");
        Py_DecRef(l);
        return NULL;
    }

    if (o->type == PY_LIST || o->type == PY_TUPLE) {
        for (size_t i = 0; i < o->u.seq.len; i++)
            seq_append(l, o->u.seq.items[i]);
        return l;
    }

    if (o->type == PY_STR) {
        for (size_t i = 0; i < o->u.str.len; i++) {
            PyObject *c = new_str_len(o->u.str.s + i, 1);
            seq_append(l, c);
            Py_DecRef(c);
        }
        return l;
    }

    if (o->type == PY_DICT) {
        for (size_t i = 0; i < o->u.dict.len; i++)
            seq_append(l, o->u.dict.keys[i]);
        return l;
    }

    set_error("object is not iterable");
    Py_DecRef(l);
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Builtins                                                          */
/* ---------------------------------------------------------------- */

static int args_len(PyObject *a) {
    return (int)a->u.seq.len;
}

static PyObject *arg(PyObject *a, int i) {
    return a->u.seq.items[i];
}

static PyObject *bi_print(PyObject *args) {
    for (int i = 0; i < args_len(args); i++) {
        if (i)
            putchar(' ');

        PyObject *s = obj_str(arg(args, i));
        fputs(s->u.str.s, stdout);
        Py_DecRef(s);
    }

    putchar('\n');
    return Py_None;
}

static PyObject *bi_len(PyObject *args) {
    if (args_len(args) != 1) {
        set_error("len() takes exactly one argument");
        return NULL;
    }

    PyObject *o = arg(args, 0);

    switch (o->type) {
        case PY_STR:
            return new_int((long long)o->u.str.len);

        case PY_LIST:
        case PY_TUPLE:
            return new_int((long long)o->u.seq.len);

        case PY_DICT:
            return new_int((long long)o->u.dict.len);

        default:
            set_error("object has no len()");
            return NULL;
    }
}

static PyObject *bi_range(PyObject *args) {
    int n = args_len(args);

    long long start = 0;
    long long stop = 0;
    long long step = 1;

    if (n == 1) {
        if (!is_num(arg(args, 0))) {
            set_error("range() requires integer arguments");
            return NULL;
        }
        stop = as_long(arg(args, 0));
    } else if (n == 2 || n == 3) {
        if (!is_num(arg(args, 0)) || !is_num(arg(args, 1))) {
            set_error("range() requires integer arguments");
            return NULL;
        }

        start = as_long(arg(args, 0));
        stop = as_long(arg(args, 1));

        if (n == 3) {
            if (!is_num(arg(args, 2))) {
                set_error("range() requires integer step");
                return NULL;
            }

            step = as_long(arg(args, 2));
        }
    } else {
        set_error("range() takes 1 to 3 arguments");
        return NULL;
    }

    if (step == 0) {
        set_error("range() step cannot be zero");
        return NULL;
    }

    PyObject *l = new_list();

    if (step > 0) {
        for (long long i = start; i < stop; i += step) {
            PyObject *v = new_int(i);
            seq_append(l, v);
            Py_DecRef(v);
        }
    } else {
        for (long long i = start; i > stop; i += step) {
            PyObject *v = new_int(i);
            seq_append(l, v);
            Py_DecRef(v);
        }
    }

    return l;
}

static PyObject *bi_type(PyObject *args) {
    if (args_len(args) != 1) {
        set_error("type() takes exactly one argument");
        return NULL;
    }

    PyObject *o = arg(args, 0);

    switch (o->type) {
        case PY_NONE: return new_str("NoneType");
        case PY_BOOL: return new_str("bool");
        case PY_INT: return new_str("int");
        case PY_FLOAT: return new_str("float");
        case PY_STR: return new_str("str");
        case PY_LIST: return new_str("list");
        case PY_TUPLE: return new_str("tuple");
        case PY_DICT: return new_str("dict");
        case PY_FUNC: return new_str("function");
        case PY_BUILTIN: return new_str("builtin_function_or_method");
        default: return new_str("object");
    }
}

static PyObject *bi_str(PyObject *args) {
    if (args_len(args) == 0)
        return new_str("");

    if (args_len(args) != 1) {
        set_error("str() takes at most one argument");
        return NULL;
    }

    return obj_str(arg(args, 0));
}

static PyObject *bi_repr(PyObject *args) {
    if (args_len(args) != 1) {
        set_error("repr() takes exactly one argument");
        return NULL;
    }

    return obj_repr(arg(args, 0));
}

static PyObject *bi_int(PyObject *args) {
    if (args_len(args) == 0)
        return new_int(0);

    if (args_len(args) != 1) {
        set_error("int() takes at most one argument");
        return NULL;
    }

    PyObject *o = arg(args, 0);

    if (o->type == PY_INT || o->type == PY_BOOL)
        return new_int(o->type == PY_BOOL ? o->u.b : o->u.i);

    if (o->type == PY_FLOAT)
        return new_int((long long)o->u.f);

    if (o->type == PY_STR) {
        char *end = NULL;
        long long v = strtoll(o->u.str.s, &end, 10);

        if (end == o->u.str.s) {
            set_error("invalid literal for int()");
            return NULL;
        }

        return new_int(v);
    }

    set_error("int() argument must be a string or number");
    return NULL;
}

static PyObject *bi_float(PyObject *args) {
    if (args_len(args) == 0)
        return new_float(0.0);

    if (args_len(args) != 1) {
        set_error("float() takes at most one argument");
        return NULL;
    }

    PyObject *o = arg(args, 0);

    if (is_num(o))
        return new_float(as_double(o));

    if (o->type == PY_STR) {
        char *end = NULL;
        double v = strtod(o->u.str.s, &end);

        if (end == o->u.str.s) {
            set_error("could not convert string to float");
            return NULL;
        }

        return new_float(v);
    }

    set_error("float() argument must be a string or number");
    return NULL;
}

static PyObject *bi_bool(PyObject *args) {
    if (args_len(args) == 0)
        return Py_False;

    if (args_len(args) != 1) {
        set_error("bool() takes at most one argument");
        return NULL;
    }

    return new_bool(obj_true(arg(args, 0)));
}

static PyObject *bi_abs(PyObject *args) {
    if (args_len(args) != 1) {
        set_error("abs() takes exactly one argument");
        return NULL;
    }

    PyObject *o = arg(args, 0);

    if (!is_num(o)) {
        set_error("abs() requires a number");
        return NULL;
    }

    if (o->type == PY_FLOAT)
        return new_float(fabs(o->u.f));

    long long v = as_long(o);
    if (v < 0) v = -v;

    return new_int(v);
}

static PyObject *bi_minmax(PyObject *args, int is_min) {
    int n = args_len(args);

    if (n == 0) {
        set_error("min/max requires arguments");
        return NULL;
    }

    PyObject *seq = NULL;
    int owned = 0;

    if (n == 1) {
        seq = iterable_to_list(arg(args, 0));
        if (!seq) return NULL;
        owned = 1;
    } else {
        seq = args;
    }

    if (seq->u.seq.len == 0) {
        if (owned)
            Py_DecRef(seq);

        set_error("min/max of empty sequence");
        return NULL;
    }

    PyObject *best = seq->u.seq.items[0];

    for (size_t i = 1; i < seq->u.seq.len; i++) {
        PyObject *item = seq->u.seq.items[i];

        if (is_min) {
            int less = obj_less(item, best);
            if (less < 0) {
                if (owned) Py_DecRef(seq);
                return NULL;
            }

            if (less)
                best = item;
        } else {
            int greater = obj_less(best, item);
            if (greater < 0) {
                if (owned) Py_DecRef(seq);
                return NULL;
            }

            if (greater)
                best = item;
        }
    }

    Py_IncRef(best);

    if (owned)
        Py_DecRef(seq);

    return best;
}

static PyObject *bi_min(PyObject *args) {
    return bi_minmax(args, 1);
}

static PyObject *bi_max(PyObject *args) {
    return bi_minmax(args, 0);
}

static PyObject *bi_sum(PyObject *args) {
    if (args_len(args) != 1) {
        set_error("sum() takes exactly one argument");
        return NULL;
    }

    PyObject *seq = iterable_to_list(arg(args, 0));
    if (!seq) return NULL;

    long long isum = 0;
    double fsum = 0.0;
    int is_float = 0;

    for (size_t i = 0; i < seq->u.seq.len; i++) {
        PyObject *item = seq->u.seq.items[i];

        if (!is_num(item)) {
            set_error("sum() requires numeric items");
            Py_DecRef(seq);
            return NULL;
        }

        if (item->type == PY_FLOAT) {
            if (!is_float) {
                fsum = (double)isum;
                is_float = 1;
            }
            fsum += item->u.f;
        } else {
            long long v = as_long(item);

            if (is_float)
                fsum += (double)v;
            else
                isum += v;
        }
    }

    Py_DecRef(seq);

    if (is_float)
        return new_float(fsum);

    return new_int(isum);
}

static PyObject *bi_input(PyObject *args) {
    if (args_len(args) > 0) {
        PyObject *prompt = obj_str(arg(args, 0));
        fputs(prompt->u.str.s, stdout);
        fflush(stdout);
        Py_DecRef(prompt);
    }

    static char buf[4096];

    if (!fgets(buf, sizeof(buf), stdin))
        return new_str("");

    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;

    return new_str(buf);
}

static PyObject *bi_ord(PyObject *args) {
    if (args_len(args) != 1) {
        set_error("ord() takes exactly one argument");
        return NULL;
    }

    PyObject *o = arg(args, 0);

    if (o->type != PY_STR || o->u.str.len != 1) {
        set_error("ord() expects a single character");
        return NULL;
    }

    return new_int((unsigned char)o->u.str.s[0]);
}

static PyObject *bi_chr(PyObject *args) {
    if (args_len(args) != 1 || !is_num(arg(args, 0))) {
        set_error("chr() takes one integer argument");
        return NULL;
    }

    long long v = as_long(arg(args, 0));

    if (v < 0 || v > 255) {
        set_error("chr() requires 0 <= x <= 255 in this mini build");
        return NULL;
    }

    char c = (char)v;
    return new_str_len(&c, 1);
}

static PyObject *bi_sorted(PyObject *args) {
    if (args_len(args) != 1) {
        set_error("sorted() takes exactly one argument");
        return NULL;
    }

    PyObject *l = iterable_to_list(arg(args, 0));
    if (!l) return NULL;

    if (sort_list(l) < 0) {
        Py_DecRef(l);
        return NULL;
    }

    return l;
}

static PyObject *bi_list(PyObject *args) {
    if (args_len(args) == 0)
        return new_list();

    if (args_len(args) != 1) {
        set_error("list() takes at most one argument");
        return NULL;
    }

    return iterable_to_list(arg(args, 0));
}

static PyObject *bi_dict(PyObject *args) {
    if (args_len(args) == 0)
        return new_dict();

    if (args_len(args) == 1 && arg(args, 0)->type == PY_DICT) {
        PyObject *src = arg(args, 0);
        PyObject *d = new_dict();

        for (size_t i = 0; i < src->u.dict.len; i++)
            dict_set(d, src->u.dict.keys[i], src->u.dict.vals[i]);

        return d;
    }

    set_error("dict() expects no arguments or another dict");
    return NULL;
}

static void reg_builtin(const char *name, PyObject *(*fn)(PyObject *)) {
    PyObject *b = new_builtin(name, fn);
    env_set_local(GLOBAL_ENV, name, b);
    Py_DecRef(b);
}

static void register_builtins(void) {
    reg_builtin("print", bi_print);
    reg_builtin("len", bi_len);
    reg_builtin("range", bi_range);
    reg_builtin("type", bi_type);
    reg_builtin("str", bi_str);
    reg_builtin("repr", bi_repr);
    reg_builtin("int", bi_int);
    reg_builtin("float", bi_float);
    reg_builtin("bool", bi_bool);
    reg_builtin("abs", bi_abs);
    reg_builtin("min", bi_min);
    reg_builtin("max", bi_max);
    reg_builtin("sum", bi_sum);
    reg_builtin("input", bi_input);
    reg_builtin("ord", bi_ord);
    reg_builtin("chr", bi_chr);
    reg_builtin("sorted", bi_sorted);
    reg_builtin("list", bi_list);
    reg_builtin("dict", bi_dict);
}

/* ---------------------------------------------------------------- */
/* Methods                                                           */
/* ---------------------------------------------------------------- */

static PyObject *call_method(PyObject *obj, const char *name, PyObject *args) {
    int n = args_len(args);

    if (obj->type == PY_LIST) {
        if (!strcmp(name, "append") && n == 1) {
            seq_append(obj, arg(args, 0));
            return Py_None;
        }

        if (!strcmp(name, "pop")) {
            long long len = (long long)obj->u.seq.len;
            long long idx;

            if (n == 0) {
                idx = len - 1;
            } else if (n == 1 && is_num(arg(args, 0))) {
                idx = as_long(arg(args, 0));
            } else {
                set_error("pop() takes optional integer index");
                return NULL;
            }

            if (idx < 0)
                idx += len;

            if (idx < 0 || idx >= len) {
                set_error("pop index out of range");
                return NULL;
            }

            PyObject *val = obj->u.seq.items[idx];

            memmove(obj->u.seq.items + idx,
                    obj->u.seq.items + idx + 1,
                    (size_t)(len - idx - 1) * sizeof(PyObject *));

            obj->u.seq.len--;

            return val;
        }

        if (!strcmp(name, "insert") && n == 2 && is_num(arg(args, 0))) {
            long long len = (long long)obj->u.seq.len;
            long long idx = as_long(arg(args, 0));

            if (idx < 0)
                idx += len;

            if (idx < 0)
                idx = 0;

            if (idx > len)
                idx = len;

            if (obj->u.seq.len == obj->u.seq.cap) {
                obj->u.seq.cap = obj->u.seq.cap ? obj->u.seq.cap * 2 : 4;
                obj->u.seq.items = xrealloc(obj->u.seq.items,
                                            obj->u.seq.cap * sizeof(PyObject *));
            }

            memmove(obj->u.seq.items + idx + 1,
                    obj->u.seq.items + idx,
                    (size_t)(len - idx) * sizeof(PyObject *));

            Py_IncRef(arg(args, 1));
            obj->u.seq.items[idx] = arg(args, 1);
            obj->u.seq.len++;

            return Py_None;
        }

        if (!strcmp(name, "sort") && n == 0) {
            if (sort_list(obj) < 0)
                return NULL;

            return Py_None;
        }

        if (!strcmp(name, "clear") && n == 0) {
            for (size_t i = 0; i < obj->u.seq.len; i++)
                Py_DecRef(obj->u.seq.items[i]);

            obj->u.seq.len = 0;
            return Py_None;
        }

        set_error("unsupported list method");
        return NULL;
    }

    if (obj->type == PY_DICT) {
        if (!strcmp(name, "get") && n >= 1) {
            int i = dict_find(obj, arg(args, 0));

            if (i >= 0) {
                Py_IncRef(obj->u.dict.vals[i]);
                return obj->u.dict.vals[i];
            }

            if (n >= 2) {
                Py_IncRef(arg(args, 1));
                return arg(args, 1);
            }

            return Py_None;
        }

        if (!strcmp(name, "keys") && n == 0) {
            PyObject *l = new_list();

            for (size_t i = 0; i < obj->u.dict.len; i++)
                seq_append(l, obj->u.dict.keys[i]);

            return l;
        }

        if (!strcmp(name, "values") && n == 0) {
            PyObject *l = new_list();

            for (size_t i = 0; i < obj->u.dict.len; i++)
                seq_append(l, obj->u.dict.vals[i]);

            return l;
        }

        if (!strcmp(name, "items") && n == 0) {
            PyObject *l = new_list();

            for (size_t i = 0; i < obj->u.dict.len; i++) {
                PyObject *t = new_seq(PY_TUPLE, 2);

                seq_append(t, obj->u.dict.keys[i]);
                seq_append(t, obj->u.dict.vals[i]);

                seq_append(l, t);
                Py_DecRef(t);
            }

            return l;
        }

        set_error("unsupported dict method");
        return NULL;
    }

    if (obj->type == PY_STR) {
        const char *s = obj->u.str.s;
        size_t len = obj->u.str.len;

        if (!strcmp(name, "upper") && n == 0) {
            char *tmp = xmalloc(len + 1);

            for (size_t i = 0; i < len; i++)
                tmp[i] = (char)toupper((unsigned char)s[i]);

            PyObject *r = new_str_len(tmp, len);
            free(tmp);

            return r;
        }

        if (!strcmp(name, "lower") && n == 0) {
            char *tmp = xmalloc(len + 1);

            for (size_t i = 0; i < len; i++)
                tmp[i] = (char)tolower((unsigned char)s[i]);

            PyObject *r = new_str_len(tmp, len);
            free(tmp);

            return r;
        }

        set_error("unsupported string method");
        return NULL;
    }

    set_error("unsupported method");
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */
/* ---------------------------------------------------------------- */

void Py_Initialize(void) {
    if (mp_initialized)
        return;

    Py_None = new_obj(PY_NONE);
    Py_True = new_obj(PY_BOOL);
    Py_True->u.b = 1;
    Py_False = new_obj(PY_BOOL);
    Py_False->u.b = 0;

    GLOBAL_ENV = env_new(NULL);
    register_builtins();

    mp_initialized = 1;
}

void Py_Finalize(void) {
    if (!mp_initialized)
        return;

    /* Break the def-cycle before tearing anything down.
     *
     * Every top-level `def` makes one: the global environment holds the
     * function object, and the function object holds its closure, which IS the
     * global environment. Two refcounts pointing at each other, so neither
     * ever reaches zero and env_decref below would drop the count from 2 to 1
     * and free nothing at all -- taking the whole global environment, every
     * function in it, and every string in both, with it.
     *
     * A tracing collector would find this. Without one, finalize is exactly
     * the right place to cut it by hand, because that is the moment nothing
     * can call these functions again.
     *
     * The count is adjusted in one go rather than calling env_decref inside
     * the loop, which could free the environment halfway through iterating it. */
    {
        int held = 0;

        for (int i = 0; i < GLOBAL_ENV->n; i++) {
            PyObject *v = GLOBAL_ENV->vars[i].val;

            if (v && v->type == PY_FUNC && v->u.func.closure == GLOBAL_ENV) {
                v->u.func.closure = NULL;   /* free_object env_decrefs this */
                held++;
            }
        }

        GLOBAL_ENV->refcnt -= held;
    }

    /* The environment first: it holds the function objects, and each of those
       holds a raw pointer into an AST. Dropping them before the trees they
       point at is the ordering that makes freeing the trees safe. */
    env_decref(GLOBAL_ENV);
    GLOBAL_ENV = NULL;

    mp_free_asts();
    clear_tokens();

    mp_initialized = 0;
}

int PyRun_SimpleString(const char *src) {
    if (!mp_initialized)
        Py_Initialize();

    clear_tokens();

    if (lex_src(src) < 0) {
        fprintf(stderr, "SyntaxError: %s\n", py_err);
        clear_tokens();
        return -1;
    }

    pos = 0;

    Node *prog = parse_program();
    if (!prog) {
        fprintf(stderr, "SyntaxError: %s\n", py_err);
        clear_tokens();
        return -1;
    }

    /* Handed to the interpreter to own. NOT freed after exec -- see
       node_free: a `def` in this script left a raw pointer to its body here. */
    mp_keep_ast(prog);

    PyObject *ret = NULL;
    int st = exec(prog, GLOBAL_ENV, &ret);

    clear_tokens();

    if (st == S_ERR) {
        fprintf(stderr, "Error: %s\n", py_err);
        return -1;
    }

    if (st != S_OK) {
        if (ret)
            Py_DecRef(ret);

        if (st != S_ERR)
            set_error("break/continue/return outside loop or function");

        fprintf(stderr, "Error: %s\n", py_err);
        return -1;
    }

    if (ret)
        Py_DecRef(ret);

    return 0;
}

/* ---------------------------------------------------------------- */
/* Mini C API wrappers                                               */
/* ---------------------------------------------------------------- */

PyObject *PyLong_FromLongLong(long long v) {
    return new_int(v);
}

long long PyLong_AsLongLong(PyObject *o) {
    if (!is_num(o))
        return 0;

    return as_long(o);
}

PyObject *PyFloat_FromDouble(double v) {
    return new_float(v);
}

double PyFloat_AsDouble(PyObject *o) {
    if (!is_num(o))
        return 0.0;

    return as_double(o);
}

PyObject *PyUnicode_FromString(const char *s) {
    return new_str(s);
}

const char *PyUnicode_AsUTF8(PyObject *o) {
    if (o && o->type == PY_STR)
        return o->u.str.s;

    return NULL;
}

PyObject *PyBool_FromLong(long v) {
    return new_bool(v != 0);
}

PyObject *PyList_New(size_t n) {
    PyObject *l = new_list();

    for (size_t i = 0; i < n; i++)
        seq_append(l, Py_None);

    return l;
}

int PyList_Append(PyObject *list, PyObject *item) {
    if (!list || list->type != PY_LIST)
        return -1;

    seq_append(list, item);
    return 0;
}

PyObject *PyDict_New(void) {
    return new_dict();
}

int PyDict_SetItem(PyObject *dict, PyObject *key, PyObject *value) {
    if (!dict || dict->type != PY_DICT)
        return -1;

    dict_set(dict, key, value);
    return 0;
}

PyObject *PyObject_Repr(PyObject *o) {
    return obj_repr(o);
}

PyObject *PyObject_Str(PyObject *o) {
    return obj_str(o);
}

PyObject *PyObject_CallObject(PyObject *callable, PyObject *args) {
    return call_object(callable, args);
}

#endif /* MINI_PY_IMPLEMENTATION */