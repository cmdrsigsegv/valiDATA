SRC_DIR := ./src
TST_DIR := ./tests
HDR_DIR := ./include

# For Linux and other POSIX systems (termios); on Windows build it in WSL.
BIN := jobcheck
TEST := test

HDR := $(wildcard $(HDR_DIR)/*.h)
SRC := $(wildcard $(SRC_DIR)/*.c)
# The tests take everything but the program's main and its terminal code.
TST := $(wildcard $(TST_DIR)/*.c) $(filter-out $(SRC_DIR)/main.c, $(SRC))

CC := cc
OPT := -O2
CFLAGS := -std=c99 -Wall -Wextra -pedantic -Wmissing-prototypes -Wfloat-equal
PREFIX := $(HOME)/.local

.PHONY: test install clean

$(BIN): $(SRC) $(HDR)
	$(CC) $(OPT) $(CFLAGS) -I$(HDR_DIR) $(SRC) -o $@

test: $(TST) $(HDR)
	$(CC) $(OPT) $(CFLAGS) -I$(HDR_DIR) $(TST) -o $(TEST)
	./$(TEST)

install: $(BIN)
	mkdir -p $(PREFIX)/bin
	cp $(BIN) $(PREFIX)/bin/

clean:
	rm -f $(BIN) $(TEST)
