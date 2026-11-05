CC      = gcc
CFLAGS  = -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wpedantic -Werror
LDFLAGS =
BUILD   = build

SRCS    = $(filter-out src/main.c,$(wildcard src/*.c))
OBJS    = $(SRCS:src/%.c=$(BUILD)/%.o)
UNIT    = $(wildcard tests/unit/test_*.c)
UNITBIN = $(UNIT:tests/unit/%.c=$(BUILD)/%)

.PHONY: all test unit integration asan clean

all: $(BUILD)/integrityd

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/integrityd.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libintegrityd.a: $(OBJS)
	ar rcs $@ $^

$(BUILD)/integrityd: $(BUILD)/main.o $(BUILD)/libintegrityd.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/test_%: tests/unit/test_%.c tests/unit/check.h $(BUILD)/libintegrityd.a
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ $< $(BUILD)/libintegrityd.a

unit: $(UNITBIN)
	@set -e; for t in $(UNITBIN); do ./$$t; done

integration: all
	INTEGRITYD=$(abspath $(BUILD)/integrityd) bash tests/run_integration.sh $(T)

test: all unit integration

asan:
	$(MAKE) BUILD=build-asan \
	  CFLAGS="$(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" \
	  LDFLAGS="-fsanitize=address,undefined" test

clean:
	rm -rf build build-asan
