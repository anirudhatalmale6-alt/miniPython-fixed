# What was wrong, and what changed

`gcc main.c -lm` built fine and then segfaulted on every run, on macOS and on
Linux both.

## 1. The crash: the parser called itself in a circle

AddressSanitizer named it in one run — not a wild pointer, a **stack
overflow**:

```
ERROR: AddressSanitizer: stack-overflow
    #1 tk_is        miniPython.h:1222
    #2 parse_not    miniPython.h:1554
    #3 parse_expr   miniPython.h:1891
    #4 parse_or     miniPython.h:1526
    #5 parse_and    miniPython.h:1540
    #6 parse_not    miniPython.h:1561
    ...
```

The precedence chain ran:

```
parse_expr -> parse_not -> parse_and -> parse_or -> parse_expr   (round again)
```

`parse_or` called `parse_expr`, which is the **top** of the chain, instead of
the next level down. No token is consumed going round that loop, so it recurses
until the stack runs out. It fires on the very first expression, which is why
nothing ran at all.

Two things were wrong, not one:

- **The cycle.** `parse_or` went back to the top.
- **The order was inverted.** Python's precedence is `or` loosest, then `and`,
  then `not`, then comparisons. The chain had `not` outermost and `or`
  innermost — the reverse. Even without the cycle, `a or b and c` would have
  parsed as `(a or b) and c`.

And a third thing fell out of it: **`parse_comparison` had no callers at all.**
It was written, and then nothing in the chain reached it, so `<`, `==`, `in`
and the rest were unreachable code.

The fix is the standard chain, which reconnects the comparison level as a side
effect:

```
parse_expr -> parse_or -> parse_and -> parse_not -> parse_comparison -> parse_add
```

## 2. Three features `main.c` used that the parser did not have

With the crash gone, `main.c` stopped with `SyntaxError: expected ]`. Its first
line uses two things that were not implemented, and slicing turned out to be
missing too:

- **conditional expressions** — `a if c else b`. `if` existed only as a
  statement. Added below `or` in precedence, where Python puts it, and the
  untaken branch is not evaluated, so `xs[0] if xs else 'empty'` is safe.
- **slicing** — `xs[1:3]`, `xs[:2]`, `xs[3:]`, `xs[:]`, `xs[-2:]`, and on
  strings. Slice bounds **clamp** where index bounds are an error:
  `[1,2,3][0:99]` is the whole list.
- **list comprehensions** — `[e for x in it]` and `[e for x in it if c]`, over
  lists, tuples, strings and `range`.

`main.c` now runs unmodified, and its output matches CPython value for value.

## 3. The AST was never freed — and the obvious fix was worse

`PyRun_SimpleString` parses a program and drops the tree on the floor. Every
call leaks the whole AST.

The obvious fix — free it after `exec` — is a **use-after-free**, because
`new_func` stores the function body as a raw pointer into that tree:

```c
o->u.func.body = body;      /* borrowed, not owned */
```

Free the AST when the script ends and every function it defined is left
dangling; calling one from a later `PyRun_SimpleString` reads freed memory.
That is worse than the leak.

So the trees are kept and freed in `Py_Finalize`, when nothing can call into
them any more. That makes the memory **bounded and reclaimed** rather than
lost. It does not make a long-running REPL free each script as it goes — that
needs the function object to own or refcount its body, which is a bigger change
and is noted rather than smuggled in.

## 4. A reference cycle that made `Py_Finalize` free nothing

Every top-level `def` makes one: the global environment holds the function
object, and the function object's closure **is** the global environment. Two
refcounts pointing at each other, so `env_decref(GLOBAL_ENV)` dropped the count
from 2 to 1 and freed nothing — taking the environment, every function in it,
and every string in both with it.

A tracing collector would find this. Without one, `Py_Finalize` is the right
place to cut the cycle by hand, because it is the moment nothing can call those
functions again.

Measured, on the 36-case suite:

```
before   2,506 bytes leaked in 64 allocations
after            0 bytes leaked
```

(and the "before" could not be measured on the original at all — it
stack-overflowed first.)

## Verifying it

`test.c` is 36 checks, and the expected strings were taken from **CPython**,
not from what this interpreter happened to print — otherwise the suite would
only assert that behaviour has not changed, which is a much weaker claim than
that it is right.

```
make check     # 36 passed, 0 failed
make asan      # clean under AddressSanitizer and UBSan
```

The suite is also checked to be capable of failing: built against the original
header it exits 139, a segfault, before printing anything.

## Still not implemented

Not bugs — things a small interpreter reasonably does not have. Listed so the
next person does not go looking:

- no `elif` chains beyond `else: if`, no `try`/`except`, no classes
- no generators, no `yield`, no `lambda`
- no dict or set comprehensions (list comprehensions only)
- no slice step (`xs[::2]`)
- no multiple assignment or tuple unpacking (`a, b = 1, 2`)
- `d.keys()` returns a plain list, not a view — deliberate, and Python 2's
  behaviour
