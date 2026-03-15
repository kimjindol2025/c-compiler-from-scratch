CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g -Iinclude -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lm

SRC = src/vm_value.c \
      src/vm_bytecode.c \
      src/vm_gc.c \
      src/vm_closure.c \
      src/vm_exec.c \
      src/vm_native.c

OBJ = $(SRC:.c=.o)

LIB = libvm.a

all: $(LIB) tests/test_vm tests/test_native tests/test_specialize

$(LIB): $(OBJ)
	ar rcs $@ $^

tests/test_vm: tests/test_vm.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. $(LIB) $(LDFLAGS)

tests/test_native: tests/test_native.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. $(LIB) $(LDFLAGS)

tests/test_specialize: tests/test_specialize.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. $(LIB) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: tests/test_vm
	./tests/test_vm

clean:
	rm -f $(OBJ) $(LIB) tests/test_vm tests/test_native tests/test_specialize

.PHONY: all test clean
