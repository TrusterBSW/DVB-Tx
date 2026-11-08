# dvbtx engine: multiplexer + modulator + radio plugins
CC ?= gcc
PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib/dvbtx
CFLAGS ?= -O3 -march=x86-64-v2 -mtune=generic -g
VERSION := $(shell cat ../VERSION 2>/dev/null)
CFLAGS += $(if $(VERSION),-DVERSION_STR=\"$(VERSION)\")
CFLAGS += -std=gnu11 -fcx-limited-range -Wall -Wextra -Wno-unused-parameter -pthread -DPLUGIN_DIR=\"$(LIBDIR)\"
LDLIBS = -lfftw3f -lm -ldl -pthread
BUILD = build

SRC = src/main.c src/conf.c src/util.c src/input.c src/psi.c src/mux.c src/rf.c src/dvbt.c src/outer.c src/ofdm.c src/dvbc.c src/dvbs.c src/dvbs2.c src/dvb_fec.c src/dvbt2.c src/scgen.c src/fcbank.c src/tsfill.c
OBJ = $(SRC:src/%.c=$(BUILD)/%.o)
PLUGINS = $(BUILD)/dvbtx-sdr-null.so
HAVE_HACKRF := $(shell pkg-config --exists libhackrf && echo y)
HAVE_UHD := $(shell pkg-config --exists uhd && echo y)
HAVE_SOAPY := $(shell pkg-config --exists SoapySDR && echo y)
ifeq ($(HAVE_HACKRF),y)
PLUGINS += $(BUILD)/dvbtx-sdr-hackrf.so
endif
ifeq ($(HAVE_UHD),y)
PLUGINS += $(BUILD)/dvbtx-sdr-uhd.so
endif
ifeq ($(HAVE_SOAPY),y)
PLUGINS += $(BUILD)/dvbtx-sdr-soapy.so
endif

all: $(BUILD)/dvbtx-engine $(PLUGINS)

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: src/%.c src/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/dvbtx-engine: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/dvbtx-sdr-null.so: src/sdr_null.c src/sdr.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< -lm

$(BUILD)/dvbtx-sdr-hackrf.so: src/sdr_hackrf.c src/sdr.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared $(shell pkg-config --cflags libhackrf) -o $@ $< $(shell pkg-config --libs libhackrf) -lm

# only libuhd: the plugin uses its C API (uhd.pc also lists Boost libraries, not needed here and not
# always installed with libuhd-dev)
$(BUILD)/dvbtx-sdr-uhd.so: src/sdr_uhd.c src/sdr.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared $(shell pkg-config --cflags uhd) -o $@ $< $(shell pkg-config --libs-only-L uhd) -luhd -lm

$(BUILD)/dvbtx-sdr-soapy.so: src/sdr_soapy.c src/sdr.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared $(shell pkg-config --cflags SoapySDR) -o $@ $< $(shell pkg-config --libs SoapySDR) -lm

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(LIBDIR)
	install -m 755 $(BUILD)/dvbtx-engine $(DESTDIR)$(LIBDIR)/
	install -m 644 $(PLUGINS) $(DESTDIR)$(LIBDIR)/

clean:
	rm -rf $(BUILD)

.PHONY: all install clean
