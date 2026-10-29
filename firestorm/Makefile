# firestorm-shim: a 32-bit ddraw.dll for Firestorm's MW4.exe.
# Needs MSYS2's i686 toolchain (pacman -S mingw-w64-i686-gcc).
CC      := /c/msys64/mingw32/bin/gcc
CFLAGS  := -O2 -Wall -Wno-unused-parameter
BUILD   := build
# Where `make install` copies the DLL: a working copy of the game, never the original.
GAME    ?= ../firestorm-run/MW4

# Windows toolchains inherit a TMP that may not be writable; keep scratch local.
export TMPDIR := $(CURDIR)/$(BUILD)/tmp
export TMP    := $(TMPDIR)
export TEMP   := $(TMPDIR)

all: $(BUILD)/ddraw.dll $(BUILD)/fsrun.exe

$(BUILD)/ddraw.dll: src/ddraw.c src/ddraw.def | $(BUILD)/tmp
	$(CC) $(CFLAGS) -shared -static-libgcc -o $@ src/ddraw.c src/ddraw.def -ldxguid -lgdi32 -luser32 -lws2_32

$(BUILD)/fsrun.exe: src/fsrun.c | $(BUILD)/tmp
	$(CC) $(CFLAGS) -static-libgcc -o $@ src/fsrun.c

$(BUILD)/tmp:
	mkdir -p $(BUILD)/tmp

install: $(BUILD)/ddraw.dll $(BUILD)/fsrun.exe
	cp $(BUILD)/ddraw.dll $(BUILD)/fsrun.exe "$(GAME)/"

uninstall:
	rm -f "$(GAME)/ddraw.dll" "$(GAME)/fsrun.exe"

clean:
	rm -rf $(BUILD)

.PHONY: all install uninstall clean
