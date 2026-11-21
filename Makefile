# Open LRU Fingerprint Library (openlru-lib)
#
#   make            build libopenlru.a, libopenlru.so and the GTK examples
#   make lib        build only the library
#   make install    install under PREFIX (default /usr/local)
#   make clean
#
# The library itself needs only a C99 compiler and the Linux kernel headers.
# The examples need GTK 3 development files (libgtk-3-dev).

CC       ?= gcc
CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu99 -Wall -Wextra -Wpedantic -Iinclude -fPIC
PREFIX   ?= /usr/local

LIB_SRC  := src/openlru.c src/olru_packet.c src/olru_link.c \
            src/olru_usb.c src/olru_image.c
LIB_OBJ  := $(LIB_SRC:src/%.c=build/%.o)

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)

EXAMPLES := build/gtk_probe build/gtk_capture

all: lib examples

lib: build/libopenlru.a build/libopenlru.so

examples: $(EXAMPLES)

build/%.o: src/%.c src/olru_internal.h include/openlru.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/libopenlru.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

build/libopenlru.so: $(LIB_OBJ)
	$(CC) -shared -o $@ $^

build/gtk_%: examples/gtk_%.c build/libopenlru.a
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $< build/libopenlru.a -o $@ $(GTK_LIBS)

install: lib
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m 644 build/libopenlru.a build/libopenlru.so $(DESTDIR)$(PREFIX)/lib
	install -m 644 include/openlru.h $(DESTDIR)$(PREFIX)/include

clean:
	rm -rf build

.PHONY: all lib examples install clean
