# KNAON label printer CUPS filter
#
# Builds the reconstructed rastertolabel filter against the system CUPS
# development files.

CC       ?= cc
CFLAGS   ?= -O2 -g
CPPFLAGS += -DHAVE_SIGACTION
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-deprecated-declarations
LDLIBS   += $(shell cups-config --libs) -lm

CUPS_FILTER_DIR ?= $(shell cups-config --serverbin 2>/dev/null)/filter
DESTDIR         ?=

TARGET      = build/rastertolabel
TEST_TARGET = build/rastertolabel-test
GEN         = build/raster_gen

all: $(TARGET)

$(TARGET): src/rastertolabel.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(shell cups-config --cflags) -o $@ $< $(LDLIBS)

# Test build with the CUPS device-URI model check disabled.  This lets the
# filter be exercised from the command line without a configured printer.
$(TEST_TARGET): src/rastertolabel.c | build
	$(CC) $(CPPFLAGS) -DKNANO_SKIP_DEVICE_CHECK $(CFLAGS) \
	      $(shell cups-config --cflags) -o $@ $< $(LDLIBS)

$(GEN): tests/raster_gen.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(shell cups-config --cflags) -o $@ $< $(LDLIBS)

build:
	mkdir -p build

install: $(TARGET)
	install -d "$(DESTDIR)$(CUPS_FILTER_DIR)/knaon/Filter"
	install -m 755 $(TARGET) "$(DESTDIR)$(CUPS_FILTER_DIR)/knaon/Filter/rastertolabel"
	install -d "$(DESTDIR)/usr/share/ppd"
	install -m 644 ppd/KnaonLabel.ppd "$(DESTDIR)/usr/share/ppd/KnaonLabel.ppd"

clean:
	rm -rf build

.PHONY: all install clean
