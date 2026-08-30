#define MINI_PY_IMPLEMENTATION
#include "miniPython.h"

int main(void) {
    Py_Initialize();

    PyRun_SimpleString(
        "def fib(n):\n"
        "    if n < 2:\n"
        "        return n\n"
        "    return fib(n - 1) + fib(n - 2)\n"
        "\n"
        "xs = [fib(i) for i in range(10)] if False else []\n"
        "for i in range(10):\n"
        "    xs.append(fib(i))\n"
        "\n"
        "print(xs)\n"
        "print(sum(xs), len(xs))\n"
        "\n"
        "d = {'a': 1, 'b': 2}\n"
        "d['c'] = d['a'] + d['b']\n"
        "print(d)\n"
        "print(d.keys(), d.values(), d.items())\n"
        "\n"
        "s = 'hello'\n"
        "print(s.upper(), len(s), s[0], s[-1])\n"
    );

    Py_Finalize();
    return 0;
}