PROG      = velo
HEADLESS  = headless
PROXYCHECK = proxycheck
VELORAPI  = velo-rapi
VELOSTATE = velo-state
BUILD     = build
ROM      ?= rom/nk.bin
CE2_ROM  ?= rom/nk-ce2.bin
VELO_TOOLCHAIN ?= ../velo-toolchain

.DEFAULT_GOAL := all

CFLAGS  += -Isrc -I$(BUILD) -Wall -Wextra -O2 -std=c11 -fno-common -MMD -MP
CFLAGS  += $(shell pkg-config --cflags sdl3)
THREAD_LIBS = -lpthread
LDFLAGS += $(shell pkg-config --libs sdl3) -lm -lz $(THREAD_LIBS)

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
MENU     ?= macos
else
MENU     ?= bar
CFLAGS   += -D_GNU_SOURCE
endif
ifeq ($(MENU),macos)
SRC_MENU  = src/app/menu_macos.m src/app/dialog_macos.m
LDFLAGS  += -framework Cocoa
else ifeq ($(MENU),android)
SRC_MENU  = src/app/menu_android.c
else
SRC_MENU  = src/app/menu_bar.c src/vendor/truetype.c
endif

ifeq ($(shell pkg-config --exists slirp && echo yes),yes)
SRC_NET  = src/net/net_gateway.c
CFLAGS  += $(shell pkg-config --cflags slirp)
NET_LIBS = $(shell pkg-config --libs slirp)
ifeq ($(shell pkg-config --exists libcurl && echo yes),yes)
SRC_NET  += src/net/web_proxy.c src/net/web_image.c src/vendor/image.c src/vendor/svg.c
CFLAGS   += $(shell pkg-config --cflags libcurl)
NET_LIBS += $(shell pkg-config --libs libcurl)
else
SRC_NET  += src/net/web_proxy_none.c
endif
LDFLAGS += $(NET_LIBS)
else
SRC_NET  = src/net/net_gateway_none.c src/net/web_proxy_none.c
endif

SRC_MACHINE = src/core/mips.c src/core/machine.c src/core/ce.c src/core/gdb.c src/core/mailbox.c src/core/agent.c src/core/screen.c src/core/vdisk.c src/core/pccard.c src/core/uart.c src/core/key_text.c src/util/options.c src/util/file.c
SRC_RAPI    = src/rapi/rapi.c src/rapi/rapi_load.c src/rapi/rapi_setup.c src/rapi/rapi_sync.c
SRC_APP     = $(SRC_MACHINE) $(SRC_NET) $(SRC_RAPI) src/app/desktop.c src/core/lcd.c src/util/png.c src/app/typer.c src/app/view.c src/app/profiles.c src/app/main.c $(SRC_MENU)

OBJ_APP      = $(patsubst %.m,$(BUILD)/%.o,$(SRC_APP:%.c=$(BUILD)/%.o))
OBJ_HEADLESS = $(SRC_MACHINE:%.c=$(BUILD)/%.o) $(SRC_NET:%.c=$(BUILD)/%.o) $(BUILD)/src/core/lcd.o $(BUILD)/src/util/png.o $(BUILD)/tools/headless.o

all: $(PROG) $(VELORAPI) $(VELOSTATE)

$(PROG): $(OBJ_APP)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILD)/libmain.so: $(OBJ_APP)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

$(HEADLESS): $(OBJ_HEADLESS)
	$(CC) -o $@ $^ -lm -lz $(NET_LIBS) $(THREAD_LIBS)

$(PROXYCHECK): $(SRC_NET:%.c=$(BUILD)/%.o) $(BUILD)/tools/proxy_check.o
	$(CC) -o $@ $^ -lm -lz $(NET_LIBS) $(THREAD_LIBS)

$(VELORAPI): $(SRC_RAPI:%.c=$(BUILD)/%.o) $(BUILD)/src/util/options.o $(BUILD)/tools/velo_rapi.o
	$(CC) -o $@ $^

$(VELOSTATE): $(BUILD)/src/util/options.o $(BUILD)/tools/velo_state.o
	$(CC) -o $@ $^ -lz

ICON_TOOL  = $(BUILD)/icon
ICON_SIZES = 16 32 64 128 256 512 1024

$(ICON_TOOL): $(BUILD)/tools/icon.o $(BUILD)/src/util/png.o $(BUILD)/src/vendor/svg.o
	$(CC) -o $@ $^ -lm -lz

icons: $(ICON_TOOL) assets/velo.svg
	@mkdir -p $(BUILD)/icons
	@for size in $(ICON_SIZES); do $(ICON_TOOL) assets/velo.svg $$size $(BUILD)/icons/velo-$$size.png || exit 1; done

$(BUILD)/src/vendor/%.o: CFLAGS += -w

VERSION ?= $(shell git describe --always --dirty 2>/dev/null || echo unknown)

$(BUILD)/version.h: FORCE
	@mkdir -p $(BUILD)
	@printf '#define VELO_VERSION "%s"\n' "$(VERSION)" > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

$(BUILD)/src/util/options.o: $(BUILD)/version.h

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.m
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -std=c11,$(CFLAGS)) -fobjc-arc -c -o $@ $<

run: $(PROG)
	./$(PROG) $(ROM)

app: $(PROG) $(VELORAPI) $(VELOSTATE) icons
	ICONS=$(BUILD)/icons sh tools/mkapp.sh Velo.app

apk: icons
	ICONS=$(BUILD)/icons sh tools/mkapk.sh

apk-push:
	sh tools/mkapk.sh push

GUEST_COMPONENTS = $(patsubst guest/%/CMakeLists.txt,%,$(wildcard guest/*/CMakeLists.txt))

guest:
	for component in $(GUEST_COMPONENTS); do \
		for version in 1 2; do \
			cmake -S guest/$$component -B $(BUILD)/guest/$$component/ce$$version -DCMAKE_TOOLCHAIN_FILE=$(abspath $(VELO_TOOLCHAIN))/cmake/velo-ce.cmake -DVELO_CE_VERSION=$$version && \
			cmake --build $(BUILD)/guest/$$component/ce$$version || exit 1; \
			cp guest/$$component/$$component.reg $(BUILD)/guest/$$component/ce$$version/ || exit 1; \
		done; \
	done

vdisk: guest

clean:
	rm -rf $(BUILD) $(PROG) $(HEADLESS) $(PROXYCHECK) $(VELORAPI) $(VELOSTATE) Velo.app

.PHONY: all run clean test check app apk apk-push icons guest vdisk FORCE

-include $(OBJ_APP:.o=.d) $(OBJ_HEADLESS:.o=.d) $(BUILD)/tools/proxy_check.d $(BUILD)/tools/velo_rapi.d $(BUILD)/tools/velo_state.d $(BUILD)/tools/icon.d

check: $(PROG) $(HEADLESS) $(PROXYCHECK) $(VELORAPI) $(VELOSTATE)
	sh tests/check.sh

test: $(HEADLESS) $(PROXYCHECK) $(VELORAPI) $(VELOSTATE)
	sh tests/boot.sh $(ROM) $(CE2_ROM)
