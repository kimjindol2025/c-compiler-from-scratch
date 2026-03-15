CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g -Iinclude -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lm

SRC = src/vm_value.c \
      src/vm_bytecode.c \
      src/vm_gc.c \
      src/vm_closure.c \
      src/vm_exec.c

OBJ = $(SRC:.c=.o)

LIB = libvm.a

all: $(LIB) tests/test_vm

$(LIB): $(OBJ)
	ar rcs $@ $^

tests/test_vm: tests/test_vm.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. $(LIB) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: tests/test_vm
	./tests/test_vm

clean:
	rm -f $(OBJ) $(LIB) tests/test_vm

.PHONY: all test clean
