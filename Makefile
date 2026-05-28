CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
WARNFLAGS ?= -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CPPFLAGS ?= -Isrc -include common/log.h
LDFLAGS ?=
LDLIBS ?= -lm
TARGET_HEADLESS ?= cloopy_headless
TARGET_SDL3 ?= cloopy_sdl3
BUILD_DIR ?= build
SDL3_CFLAGS ?= $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL3_LIBS ?= $(shell pkg-config --libs sdl3 2>/dev/null) -lSDL3
PYTHON ?= python3
WANWAN_SOUND_DIR ?= assets/wanwan_free_sounds
WANWAN_OKI_ROM ?= wanwan_synthetic_msm6653a_457.bin
WANWAN_OKI_ORDER ?= wanwan_synthetic_msm6653a_457_order.txt
WANWAN_OKI_BANK_C := src/sound/wanwan_oki_bank.c
WANWAN_OKI_BANK_H := src/sound/wanwan_oki_bank.h
WANWAN_OKI_SAMPLE_FILES := $(sort $(wildcard $(WANWAN_SOUND_DIR)/*.wav))

CSTD = -std=c11
ALL_SRC := $(shell find src -name '*.c' | sort)
CORE_SRC := $(filter-out src/main.c src/sdl3/main_sdl3.c,$(ALL_SRC))
HEADLESS_SRC := src/main.c
SDL3_SRC := src/sdl3/main_sdl3.c
HEADLESS_OBJ := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(CORE_SRC) $(HEADLESS_SRC))
SDL3_OBJ := $(patsubst src/%.c,$(BUILD_DIR)/%.sdl3.o,$(CORE_SRC) $(SDL3_SRC))
DEP := $(HEADLESS_OBJ:.o=.d) $(SDL3_OBJ:.o=.d)

.PHONY: all headless sdl3 sdl3-check wanwan-oki-rom test test-cpu test-bsc test-io test-mouse test-printer test-cmdlist clean run-smoke run-smoke-debug
all: wanwan-oki-rom headless
headless: wanwan-oki-rom $(TARGET_HEADLESS)
sdl3: wanwan-oki-rom $(TARGET_SDL3)
sdl3-check: wanwan-oki-rom $(SDL3_OBJ)
wanwan-oki-rom: $(WANWAN_OKI_ROM)

test: test-cpu test-bsc test-io test-mouse test-printer test-cmdlist


test-cpu: $(BUILD_DIR)/tests/sh7021_sign_extension_test
	./$(BUILD_DIR)/tests/sh7021_sign_extension_test

test-bsc: $(BUILD_DIR)/tests/sh7021_bsc_test
	./$(BUILD_DIR)/tests/sh7021_bsc_test

test-io: $(BUILD_DIR)/tests/io_controller_mode_test
	./$(BUILD_DIR)/tests/io_controller_mode_test

test-mouse: $(BUILD_DIR)/tests/io_mouse_counter_test
	./$(BUILD_DIR)/tests/io_mouse_counter_test

test-printer: $(BUILD_DIR)/tests/printer_png_test
	./$(BUILD_DIR)/tests/printer_png_test

test-cmdlist: $(BUILD_DIR)/tests/cmdlist_roundtrip_test
	./$(BUILD_DIR)/tests/cmdlist_roundtrip_test


$(BUILD_DIR)/tests/sh7021_sign_extension_test: tests/sh7021_sign_extension_test.c src/core/sh7021/sh7021_interpreter.c src/core/sh7021/sh7021_interpreter.h src/core/sh7021/sh7021_local.h src/core/sh7021/sh7021_bus.h
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) tests/sh7021_sign_extension_test.c src/core/sh7021/sh7021_interpreter.c -o $@

$(BUILD_DIR)/tests/sh7021_bsc_test: tests/sh7021_bsc_test.c src/core/sh7021/peripherals/sh7021_bsc.c src/core/sh7021/peripherals/sh7021_bsc.h
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) tests/sh7021_bsc_test.c src/core/sh7021/peripherals/sh7021_bsc.c -o $@

$(WANWAN_OKI_ROM) $(WANWAN_OKI_ORDER) $(WANWAN_OKI_BANK_C) $(WANWAN_OKI_BANK_H): tools/generate_wanwan_oki_bank.py $(WANWAN_OKI_SAMPLE_FILES)
	$(PYTHON) tools/generate_wanwan_oki_bank.py --sample-dir $(WANWAN_SOUND_DIR) --source-dir src/sound --bin $(WANWAN_OKI_ROM) --order $(WANWAN_OKI_ORDER)

$(BUILD_DIR)/sound/wanwan_oki_bank.o $(BUILD_DIR)/sound/wanwan_oki_bank.sdl3.o: $(WANWAN_OKI_ROM)

$(TARGET_HEADLESS): $(WANWAN_OKI_ROM) $(HEADLESS_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(HEADLESS_OBJ) $(LDLIBS)

$(TARGET_SDL3): $(WANWAN_OKI_ROM) $(SDL3_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(SDL3_OBJ) $(LDLIBS) $(SDL3_LIBS)

$(BUILD_DIR)/tests/io_controller_mode_test: tests/io_controller_mode_test.c src/core/loopy_io.c src/core/loopy_io.h src/input/input.h src/common/log.h
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) tests/io_controller_mode_test.c src/core/loopy_io.c -o $@

$(BUILD_DIR)/tests/io_mouse_counter_test: tests/io_mouse_counter_test.c src/core/loopy_io.c src/core/loopy_io.h src/common/log.h
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) tests/io_mouse_counter_test.c src/core/loopy_io.c -o $@

$(BUILD_DIR)/tests/printer_png_test: tests/printer_png_test.c src/core/loopy_io.c src/core/loopy_io.h src/common/log.h
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) tests/printer_png_test.c src/core/loopy_io.c -o $@

$(BUILD_DIR)/tests/cmdlist_roundtrip_test: tests/cmdlist_roundtrip_test.c src/frontend/cmdlist.c src/frontend/cmdlist.h
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) tests/cmdlist_roundtrip_test.c src/frontend/cmdlist.c -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.sdl3.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CPPFLAGS) $(SDL3_CFLAGS) -DLOOPY_SDL3_FRONTEND $(CFLAGS) $(WARNFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET_HEADLESS) $(TARGET_SDL3) output_*.bmp emudump.bin $(WANWAN_OKI_ROM) $(WANWAN_OKI_ORDER)

run-smoke: $(TARGET_HEADLESS)
	./$(TARGET_HEADLESS) "Anime Land.bin" loopy_bios.bin sound.bin --frames 2400

run-smoke-debug: $(TARGET_HEADLESS)
	./$(TARGET_HEADLESS) "Anime Land.bin" loopy_bios.bin sound.bin --frames 2400

-include $(DEP)
