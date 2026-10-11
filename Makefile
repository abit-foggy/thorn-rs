# thorn-rs - zero-cargo #![no_std] Rust static libraries for thorn
# 
CC     ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic

SRC = src/main.c src/rust.c src/scan.c src/plan.c \
      src/emit_ninja.c src/emit_make.c src/verify.c
HDR = src/include/rust.h src/include/scan.h \
      src/include/plan.h src/include/emit.h
BIN = bin/thorn-rs

.PHONY: all test check clean

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	@mkdir -p bin
	$(CC) $(CFLAGS) -I src -o $@ $(SRC)

test check: $(BIN)
	./tests/verify.sh

clean:
	rm -rf bin out out-ninja out-make tests/out
