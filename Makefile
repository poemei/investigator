CC ?= cc

CPPFLAGS := -D_POSIX_C_SOURCE=200809L -Iinclude -I../rictus/include -I../ABI/includes
CFLAGS := -std=c17 -Wall -Wextra -Wpedantic -fPIC
LDFLAGS := -shared
LDLIBS := -lcrypto -pthread

BUILD_DIR := build/linux
TARGET := $(BUILD_DIR)/investigation.so
PREFIX ?= /usr/local
MODULEDIR ?= $(PREFIX)/lib/rictus/modules/investigation
DESTDIR ?=

SOURCES := \
	src/investigation.c \
	src/production.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SOURCES) include/investigation.h include/production.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(SOURCES) $(LDLIBS) -o $@

install: all
	install -d "$(DESTDIR)$(MODULEDIR)"
	install -m 0755 "$(TARGET)" "$(DESTDIR)$(MODULEDIR)/investigation.so"
	install -m 0644 module.conf "$(DESTDIR)$(MODULEDIR)/module.conf"

clean:
	rm -rf $(BUILD_DIR)
