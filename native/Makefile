CC       ?= cc
CFLAGS   ?= -O2 -g -Wall -Wextra -std=c11
LDLIBS    = $(shell pkg-config --libs libusb-1.0) -lpthread
CPPFLAGS += $(shell pkg-config --cflags libusb-1.0)
# Only i2600-scand needs it: pages go into the PDF as JPEG.
JPEGLIBS  = $(shell pkg-config --libs libjpeg)
JPEGFLAGS = $(shell pkg-config --cflags libjpeg)

PREFIX    ?= /usr
BINDIR    ?= $(PREFIX)/bin
SANEDIR   ?= $(PREFIX)/lib/sane
DLLDIR    ?= /etc/sane.d/dll.d
UNITDIR   ?= $(PREFIX)/lib/systemd/user

CORE      = i2600-core.c i2600-firmware.c
HEADERS   = i2600-core.h i2600-firmware.h sequence.h sequence-duplex.h \
            sequence-color.h sequence-color-duplex.h

SIMPLEX_CAPTURE = ../captures/pcapng/b-simplex-lineart-200.pcapng
DUPLEX_CAPTURE  = ../captures/pcapng/c-duplex-lineart-200.pcapng

all: i2600-grab i2600-status i2600-nvram i2600-buttons i2600-lcd i2600-power \
     i2600-calibrate \
     i2600-scand libsane-i2600.so.1

i2600-grab: i2600-grab.c $(CORE) $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ i2600-grab.c $(CORE) $(LDLIBS)

i2600-buttons: i2600-buttons.c $(CORE) $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ i2600-buttons.c $(CORE) $(LDLIBS)

i2600-lcd: i2600-lcd.c $(CORE) $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ i2600-lcd.c $(CORE) $(LDLIBS)

i2600-scand: i2600-scand.c i2600-pdf.c $(CORE) $(HEADERS) i2600-pdf.h panel-bitmaps.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(JPEGFLAGS) -o $@ i2600-scand.c i2600-pdf.c \
	      $(CORE) $(LDLIBS) $(JPEGLIBS)

i2600-status: i2600-status.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(shell pkg-config --libs libusb-1.0)

i2600-calibrate: i2600-calibrate.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(shell pkg-config --libs libusb-1.0)

i2600-nvram: i2600-nvram.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(shell pkg-config --libs libusb-1.0)

i2600-power: i2600-power.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(shell pkg-config --libs libusb-1.0)

libsane-i2600.so.1: i2600-sane.c $(CORE) $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -fPIC -shared -o $@ i2600-sane.c $(CORE) $(LDLIBS)

# The sequence headers are checked in; regenerate only when a capture changes.
# The rules exist only where the captures do, so that a copy of the sources
# without captures/ -- a deployment, say -- still builds from the checked-in
# headers instead of failing on a missing prerequisite.
ifneq ($(wildcard $(SIMPLEX_CAPTURE)),)
sequence.h: gen-sequence.py $(SIMPLEX_CAPTURE)
	python3 gen-sequence.py $(SIMPLEX_CAPTURE) > $@
endif

ifneq ($(wildcard $(DUPLEX_CAPTURE)),)
sequence-duplex.h: gen-duplex.py $(DUPLEX_CAPTURE)
	python3 gen-duplex.py $(DUPLEX_CAPTURE) > $@
endif

# The panel bitmaps are checked in too; regenerating needs PIL and a font.
ifneq ($(wildcard ../tools/lcd-text.py),)
panel-bitmaps.h: gen-panel.py ../tools/lcd-text.py
	python3 gen-panel.py > $@
endif

# Try the backend without installing it.
check: libsane-i2600.so.1
	@mkdir -p .sane.d && echo i2600 > .sane.d/dll.conf
	SANE_CONFIG_DIR=$(PWD)/.sane.d LD_LIBRARY_PATH=$(PWD) scanimage -L

install: all
	install -Dm755 libsane-i2600.so.1 $(DESTDIR)$(SANEDIR)/libsane-i2600.so.1
	ln -sf libsane-i2600.so.1 $(DESTDIR)$(SANEDIR)/libsane-i2600.so
	install -d $(DESTDIR)$(DLLDIR)
	echo i2600 > $(DESTDIR)$(DLLDIR)/i2600
	install -Dm755 i2600-scand $(DESTDIR)$(BINDIR)/i2600-scand
	install -Dm644 i2600-scand.service $(DESTDIR)$(UNITDIR)/i2600-scand.service

uninstall:
	rm -f $(DESTDIR)$(SANEDIR)/libsane-i2600.so.1 \
	      $(DESTDIR)$(SANEDIR)/libsane-i2600.so \
	      $(DESTDIR)$(DLLDIR)/i2600 \
	      $(DESTDIR)$(BINDIR)/i2600-scand \
	      $(DESTDIR)$(UNITDIR)/i2600-scand.service

clean:
	rm -f i2600-grab i2600-status i2600-nvram i2600-buttons i2600-lcd i2600-calibrate \
	      i2600-power i2600-scand libsane-i2600.so.1
	rm -rf .sane.d

.PHONY: all check install uninstall clean
