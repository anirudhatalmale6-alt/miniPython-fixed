# miniPython
#
#   make        build the demo and the test suite
#   make test   run the regression suite (36 checks)
#   make asan   run the suite under AddressSanitizer + UBSan

CC     ?= cc
CFLAGS ?= -O2 -Wall

all: minipy test

minipy: main.c miniPython.h
	$(CC) $(CFLAGS) -o $@ main.c -lm

test: test.c miniPython.h
	$(CC) $(CFLAGS) -o $@ test.c -lm

.PHONY: run check asan clean

run: minipy
	./minipy

check: test
	./test

# The suite is clean under both sanitizers. Worth keeping that way: this is a
# refcounted interpreter, and a refcount bug shows up as a leak or a
# use-after-free long before it shows up as a wrong answer.
asan: test.c miniPython.h
	$(CC) -g -O0 -fsanitize=address,undefined -o test-asan test.c -lm
	./test-asan

clean:
	rm -f minipy test test-asan
