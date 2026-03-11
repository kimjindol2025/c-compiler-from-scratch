CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -g \
          -Wno-unused-parameter -Wno-unused-function \
          -D_POSIX_C_SOURCE=200809L

SRC_DIR   = src
INC_DIR   = include
TEST_DIR  = tests
BUILD_DIR = build

SRCS = $(SRC_DIR)/preprocessor.c \
       $(SRC_DIR)/lexer.c \
       $(SRC_DIR)/ast.c \
       $(SRC_DIR)/types.c \
       $(SRC_DIR)/symtable.c \
       $(SRC_DIR)/parser.c \
       $(SRC_DIR)/sema.c \
       $(SRC_DIR)/x86_encode.c \
       $(SRC_DIR)/codegen.c \
       $(SRC_DIR)/elf_writer.c \
       $(SRC_DIR)/main.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

TARGET = ccc

.PHONY: all test check clean

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c -o $@ $<

# ---- Tests ----
test: $(BUILD_DIR)/test_lexer $(BUILD_DIR)/test_parser
	@echo "=== Lexer tests ==="
	./$(BUILD_DIR)/test_lexer
	@echo ""
	@echo "=== Parser tests ==="
	./$(BUILD_DIR)/test_parser

$(BUILD_DIR)/test_lexer: $(TEST_DIR)/test_lexer.c \
                          $(BUILD_DIR)/lexer.o $(BUILD_DIR)/ast.o \
                          $(BUILD_DIR)/types.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) -o $@ $^

$(BUILD_DIR)/test_parser: $(TEST_DIR)/test_parser.c \
                           $(BUILD_DIR)/lexer.o $(BUILD_DIR)/ast.o \
                           $(BUILD_DIR)/types.o $(BUILD_DIR)/parser.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) -o $@ $^

# ---- Quick integration check ----
check: $(TARGET)
	@echo "--- Integration: compile a simple C file ---"
	@printf 'int main() { return 42; }\n' > /tmp/_ccc_test.c
	./$(TARGET) -c -o /tmp/_ccc_test.o /tmp/_ccc_test.c && \
	  echo "PASS: object file written" && \
	  file /tmp/_ccc_test.o || echo "FAIL"
	@rm -f /tmp/_ccc_test.c /tmp/_ccc_test.o

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
