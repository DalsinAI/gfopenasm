CC ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter
SRCS = src/main.c src/util.c src/expr.c src/asm.c src/mot.c src/m68k.c src/output.c
OBJS = $(SRCS:.c=.o)

all: gfasm

gfasm: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lm

src/%.o: src/%.c src/gfasm.h
	$(CC) $(CFLAGS) -c -o $@ $<

test: gfasm
	sh tests/run.sh

clean:
	rm -f $(OBJS) gfasm

.PHONY: all test clean
