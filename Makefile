TOOLCHAIN ?= /home/tomaszz/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot
CROSS     := $(TOOLCHAIN)/opt/ext-toolchain/bin/mips-mti-linux-gnu-
SYSROOT   := $(TOOLCHAIN)/mipsel-buildroot-linux-gnu/sysroot
CC        := $(CROSS)gcc

CFLAGS := -mips32r2 -march=mips32r2 -mtune=24kc -mfp32 -mhard-float -mlong-calls \
          -EL --sysroot=$(SYSROOT) -G0 -Os -Wall -Wextra -I$(SYSROOT)/usr/include
LDFLAGS := -mips32r2 -mhard-float -mfp32 -EL --sysroot=$(SYSROOT) -L$(SYSROOT)/usr/lib

FONT_SOURCE ?= ../picoarch/libpicofe/fonts.c
FROGUI_DIR  ?= ../FrogUI
TARGET ?= ../sf3000_treefrogui/sdcard/cubegm/cores/frogshell_libretro.so

SOURCES := frogshell.c devmode.c process.c terminal.c usbkbd.c

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SOURCES) $(FONT_SOURCE)
	$(CC) $(CFLAGS) -fPIC -I$(FROGUI_DIR) $(SOURCES) $(FONT_SOURCE) $(LDFLAGS) \
		-shared -Wl,--gc-sections -lm -o $@
	$(CROSS)strip $@

clean:
	rm -f $(TARGET)
