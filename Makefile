CC ?= cc

CPPFLAGS := -D_POSIX_C_SOURCE=200809L -Iinclude -I../rictus/include -I../ABI/includes
CFLAGS := -std=c17 -Wall -Wextra -Wpedantic -fPIC
LDFLAGS := -shared
LDLIBS := -pthread

BUILD_DIR := build/linux
TARGET := $(BUILD_DIR)/investigation.so

SOURCES := \
	src/investigation.c \
	src/production.c \
	src/sha256.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(SOURCES) $(LDLIBS) -o $@

clean:
	rm -rf $(BUILD_DIR)
