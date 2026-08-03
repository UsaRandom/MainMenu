PROJECT_NAME = N64FlashcartMenu

.DEFAULT_GOAL := all

SOURCE_DIR = src
ASSETS_DIR = assets
FILESYSTEM_DIR = filesystem
BUILD_DIR = build
OUTPUT_DIR = output

MENU_VERSION ?= "Preview release"
BUILD_TIMESTAMP = "$(shell TZ='UTC' date "+%Y-%m-%d %H:%M:%S %:z")"

# A harness build stamps a fixed time instead. credits.o is force-rebuilt every make, so the
# real timestamp changes on every build and the credits screen hashes differently every run --
# which reads as a rendering regression when it is only the clock.
ifdef DEV_HARNESS
BUILD_TIMESTAMP = "1996-06-23 00:00:00 +00:00"
endif

# Use the prefix holding the pinned libdragon. ~/n64inst carries an older one whose stale
# libdragon.a wins the link through gcc's built-in search path -- see docs/AUDIT.md 2.1.
N64_INST ?= $(HOME)/n64inst-preview
export N64_INST   # some host tools read it from the environment, not the make variable

include $(N64_INST)/include/n64.mk

# Development harness. Neither of these is ever set for a release build.
#   FIXTURE=1      pack a synthetic SD tree into the DFS, so the UI has a library to show
#                  under ares, where storage_prefix is "rom:/" and there is no real card
#   DEV_HARNESS=1  compile src/dev/* and enable the ares emux macros (framebuffer dump,
#                  self-termination, frame-time reporting)
FIXTURE_DIR = $(BUILD_DIR)/fixture

# tools/getart.py leaves real box art here. Passed to mkfixture whenever it exists, because
# without it an automatic regenerate quietly swaps 21 MB of real cards for procedural gradients --
# and every decode measurement in AUDIT.md was taken against the real ones. The trap is that both
# fixtures look fine; only the numbers change.
ARTCACHE_DIR = $(BUILD_DIR)/artcache
FIXTURE_ART  = $(if $(wildcard $(ARTCACHE_DIR)),--art-from $(ARTCACHE_DIR),)
DFS_ROOT_DIR = $(BUILD_DIR)/dfsroot

ifdef FIXTURE
N64_MKDFS_ROOT = $(DFS_ROOT_DIR)
else
N64_MKDFS_ROOT = $(FILESYSTEM_DIR)
endif

# Setting FLAGS on the command line REPLACES the makefile's value, so `make FLAGS=-DFOO` also
# silently drops -DDEV_HARNESS and the run produces no dumps. Take the knob as its own variable.
ifdef FBSCALE
FLAGS += -DDBG_FBDUMP_SCALE=$(FBSCALE)
endif

# For sweeping a tunable across builds without editing the source between runs:
#   make TUNE='-DDECODE_BUDGET_IDLE_US=3000' ...
# Part of the config stamp, so changing it rebuilds rather than relinking the previous value.
FLAGS += $(TUNE)

ifdef DEV_HARNESS
FLAGS += -DDEV_HARNESS
DEV_SRCS = dev/debug_emux.c dev/inputscript.c dev/allocwatch.c
# --wrap catches allocations inside libdragon and libspng too. A hook installed from our own code
# would only see our own calls, and the interesting ones are not ours.
N64_LDFLAGS += --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free
GENERATED_HEADERS = $(BUILD_DIR)/inputscript_generated.h
endif

# Toggling FIXTURE or DEV_HARNESS changes what gets compiled and packed but touches no
# timestamps, so make would happily link objects from the other configuration and pack a DFS
# from the other tree. Name a stamp after the current setting and drop the outputs whenever it
# changes. This runs while the makefile is read, before any dependency is considered, because
# a rule would fire too late to affect decisions make has already made.
CONFIG_STAMP := $(BUILD_DIR)/.config-$(if $(FIXTURE),fixture,plain)-$(if $(DEV_HARNESS),dev-$(notdir $(basename $(INPUT_SCRIPT)))-$(FBSCALE),nodev)-$(shell echo '$(TUNE)' | tr -c 'A-Za-z0-9=' '_')
$(shell [ -f $(CONFIG_STAMP) ] || rm -rf $(BUILD_DIR)/menu $(BUILD_DIR)/dev $(BUILD_DIR)/ui $(BUILD_DIR)/screens $(BUILD_DIR)/library $(BUILD_DIR)/app.o $(BUILD_DIR)/inputscript_generated.h $(BUILD_DIR)/.config-* $(BUILD_DIR)/$(PROJECT_NAME).elf $(BUILD_DIR)/$(PROJECT_NAME).dfs)
$(shell mkdir -p $(BUILD_DIR) && touch $(CONFIG_STAMP))

N64_ROM_SAVETYPE = none
N64_ROM_RTC = 1
N64_ROM_REGIONFREE = 1
N64_ROM_REGION = E

N64_CFLAGS += -iquote $(SOURCE_DIR) -iquote $(ASSETS_DIR) -I $(SOURCE_DIR)/libs -isystem $(SOURCE_DIR)/libs/miniz -flto=auto $(FLAGS)
# The input script is generated, so it lives in the build tree rather than in src/.
N64_CFLAGS += -iquote $(BUILD_DIR)

# Deferred until the launch path is rebuilt on app_t: cart_load.c and usb_comm.c still take a
# menu_t, and bookkeeping.c is superseded by library playstate. disk_info.c (64DD), hdmi.c
# (PixelFX game ID) and cpakfs_utils.c come back with the screens that use them.
SRCS = \
	main.c \
	app.c \
	boot/boot.c \
	boot/cheats.c \
	boot/cic.c \
	boot/reboot.S \
	flashcart/flashcart_utils.c \
	flashcart/flashcart.c \
	flashcart/sc64/sc64_ll.c \
	flashcart/sc64/sc64.c \
	libs/libspng/spng/spng.c \
	libs/miniz/miniz_tdef.c \
	libs/miniz/miniz_tinfl.c \
	libs/miniz/miniz_zip.c \
	libs/miniz/miniz.c \
	cheats/cheatdb.c \
	library/library.c \
	library/thumbcache.c \
	menu/cart_load.c \
	menu/ini_parser.c \
	menu/fonts.c \
	menu/path.c \
	menu/png_decoder.c \
	menu/rom_info.c \
	menu/settings.c \
	menu/sound.c \
	screens/boot_plate.c \
	screens/screen_cheats.c \
	screens/screen_detail.c \
	screens/screen_grid.c \
	screens/screen_launch.c \
	screens/screen_settings.c \
	screens/screen_fault.c \
	ui/draw.c \
	ui/input.c \
	ui/theme.c \
	ui/tween.c \
	utils/fs.c

# FirpleBoot.ttf has no file of its own. It is the same face as Firple-Bold baked at a
# different size, and its rule below names that source explicitly; it is listed here only
# because this is what derives the filesystem entry.
FONTS = \
	Firple-Bold.ttf \
	FirpleBoot.ttf

IMAGES = \
	sc64_logo.png

SOUNDS = \
	cursorsound.wav \
	back.wav \
	bgm.wav \
	enter.wav \
	error.wav \
	settings.wav

OBJS = $(addprefix $(BUILD_DIR)/, $(addsuffix .o,$(basename $(SRCS) $(DEV_SRCS))))

# Generate the input script table before anything that includes it is compiled. With no
# INPUT_SCRIPT named the generator still emits an empty program, so a DEV_HARNESS build
# without a script compiles and runs interactively.
$(BUILD_DIR)/inputscript_generated.h: tools/mkinput.py $(INPUT_SCRIPT)
	@mkdir -p $(BUILD_DIR)
	@python3 tools/mkinput.py $(INPUT_SCRIPT) -o $@

$(OBJS): $(GENERATED_HEADERS)
MINIZ_OBJS = $(filter $(BUILD_DIR)/libs/miniz/%.o,$(OBJS))
SPNG_OBJS = $(filter $(BUILD_DIR)/libs/libspng/%.o,$(OBJS))
DEPS = $(OBJS:.o=.d)

FILESYSTEM = \
	$(addprefix $(FILESYSTEM_DIR)/, $(notdir $(FONTS:%.ttf=%.font64))) \
	$(addprefix $(FILESYSTEM_DIR)/, $(notdir $(SOUNDS:%.wav=%.wav64))) \
	$(addprefix $(FILESYSTEM_DIR)/, $(notdir $(IMAGES:%.png=%.sprite)))

$(MINIZ_OBJS): N64_CFLAGS+=-Wno-unused-function -fcompare-debug-second
$(SPNG_OBJS): N64_CFLAGS+=-DSPNG_USE_MINIZ -fcompare-debug-second
$(FILESYSTEM_DIR)/Firple-Bold.font64: MKFONT_FLAGS+=--compress 1 --outline 1 --size 20 --charset $(ASSETS_DIR)/fonts/charset.txt --ellipsis 2026,1
# The boot plate is the only 32 px type in the product (docs/design/README.md 4.1), and it says
# four fixed strings. Baking the full 7,931-character charset at that size would cost megabytes
# for glyphs nothing renders; the boot charset is 41 characters.
$(FILESYSTEM_DIR)/FirpleBoot.font64: MKFONT_FLAGS+=--compress 1 --size 32 --charset $(ASSETS_DIR)/fonts/charset-boot.txt --ellipsis 2E,3
$(FILESYSTEM_DIR)/%.wav64: AUDIOCONV_FLAGS=--wav-compress 1

$(@info $(shell mkdir -p ./$(FILESYSTEM_DIR) &> /dev/null))

$(FILESYSTEM_DIR)/%.font64: $(ASSETS_DIR)/fonts/%.ttf
	@echo "    [FONT] $@"
	@$(N64_MKFONT) $(MKFONT_FLAGS) -o $(FILESYSTEM_DIR) "$<"

# The boot plate is the same typeface as everything else, so it builds from the same .ttf.
# mkfont names its output after its input and offers no way to override that (-o takes a
# directory), so this bake goes to a scratch directory and gets renamed. Two extra lines,
# against the alternative of an 8.4 MB byte-identical second copy of the font in the
# repository -- and git history keeps a blob forever, so that copy would be permanent.
# An explicit rule beats the pattern rule above, which is what stops make looking for a
# FirpleBoot.ttf that does not exist.
$(FILESYSTEM_DIR)/FirpleBoot.font64: $(ASSETS_DIR)/fonts/Firple-Bold.ttf
	@echo "    [FONT] $@"
	@mkdir -p $(BUILD_DIR)/fontbake
	@$(N64_MKFONT) $(MKFONT_FLAGS) -o $(BUILD_DIR)/fontbake "$<"
	@mv $(BUILD_DIR)/fontbake/Firple-Bold.font64 $@

$(FILESYSTEM_DIR)/%.wav64: $(ASSETS_DIR)/sounds/%.wav
	@echo "    [AUDIO] $@"
	@$(N64_AUDIOCONV) $(AUDIOCONV_FLAGS) -o $(FILESYSTEM_DIR) "$<"

$(FILESYSTEM_DIR)/%.sprite: $(ASSETS_DIR)/images/%.png
	@echo "    [SPRITE] $@"
	@$(N64_MKSPRITE) $(MKSPRITE_FLAGS) -o $(dir $@) "$<"

$(BUILD_DIR)/$(PROJECT_NAME).dfs: $(FILESYSTEM) $(if $(FIXTURE),dfsroot)

# Stage the real in-ROM assets and the synthetic SD tree into one directory for mkdfs. The
# fixture is generated rather than committed, so a missing build/fixture is a normal first run,
# not an error.
dfsroot: $(FILESYSTEM)
	@rm -rf $(DFS_ROOT_DIR)
	@mkdir -p $(DFS_ROOT_DIR)
	@cp -R $(FILESYSTEM_DIR)/. $(DFS_ROOT_DIR)/
	@if [ ! -d $(FIXTURE_DIR) ]; then \
		echo "    [FIXTURE] generating $(FIXTURE_DIR)"; \
		python3 tools/mkfixture.py -o $(FIXTURE_DIR) $(FIXTURE_ART) >/dev/null; \
	fi
	@cp -R $(FIXTURE_DIR)/. $(DFS_ROOT_DIR)/
	@# cheats.db is a release artifact built by tools/mkcheatdb.py, never committed. Staged when
	@# it happens to be there so the cheats screen has something to show under ares.
	@if [ -f $(BUILD_DIR)/cheats.db ]; then \
		mkdir -p $(DFS_ROOT_DIR)/menu; \
		cp $(BUILD_DIR)/cheats.db $(DFS_ROOT_DIR)/menu/cheats.db; \
		echo "    [FIXTURE] staged cheats.db ($$(du -h $(BUILD_DIR)/cheats.db | cut -f1))"; \
	fi
	@echo "    [FIXTURE] staged $$(find $(DFS_ROOT_DIR) -type f | wc -l | tr -d ' ') files into the DFS"
.PHONY: dfsroot

fixture:
	@rm -rf $(FIXTURE_DIR)
	@python3 tools/mkfixture.py -o $(FIXTURE_DIR) $(FIXTURE_ART)
.PHONY: fixture

$(BUILD_DIR)/app.o: .FORCE
$(BUILD_DIR)/screens/screen_grid.o: FLAGS+=-DMENU_VERSION=\"$(MENU_VERSION)\"
$(BUILD_DIR)/screens/screen_grid.o: .FORCE
$(BUILD_DIR)/screens/screen_settings.o: FLAGS+=-DMENU_VERSION=\"$(MENU_VERSION)\" -DBUILD_TIMESTAMP=\"$(BUILD_TIMESTAMP)\"
$(BUILD_DIR)/screens/screen_settings.o: .FORCE
$(BUILD_DIR)/app.o: FLAGS+=-DMENU_VERSION=\"$(MENU_VERSION)\" -DBUILD_TIMESTAMP=\"$(BUILD_TIMESTAMP)\"

$(BUILD_DIR)/$(PROJECT_NAME).elf: $(OBJS)

disassembly: $(BUILD_DIR)/$(PROJECT_NAME).elf
	@$(N64_OBJDUMP) -S $< > $(BUILD_DIR)/$(PROJECT_NAME).lst
.PHONY: disassembly

$(PROJECT_NAME).z64: N64_ROM_TITLE=$(PROJECT_NAME)
$(PROJECT_NAME).z64: $(BUILD_DIR)/$(PROJECT_NAME).dfs

$(@info $(shell mkdir -p ./$(OUTPUT_DIR) &> /dev/null))

$(OUTPUT_DIR)/$(PROJECT_NAME).n64: $(PROJECT_NAME).z64
	@mv $< $@

sc64: $(OUTPUT_DIR)/$(PROJECT_NAME).n64
	@cp $< $(OUTPUT_DIR)/sc64menu.n64
.PHONY: sc64

all: sc64
.PHONY: all

clean:
	@rm -f ./$(FILESYSTEM)
	@find ./$(FILESYSTEM_DIR) -type d -empty -delete
	@rm -rf ./$(BUILD_DIR) ./$(OUTPUT_DIR)
.PHONY: clean

format:
	@find ./$(SOURCE_DIR) \
		-path \./$(SOURCE_DIR)/libs -prune \
		-o -iname *.c -print \
		-o -iname *.h -print \
		| xargs clang-format -i

run: $(OUTPUT_DIR)/$(PROJECT_NAME).n64
ifeq ($(OS),Windows_NT)
	./localdeploy.bat
else
	./remotedeploy.sh
endif
.PHONY: run

run-debug: $(OUTPUT_DIR)/$(PROJECT_NAME).n64
ifeq ($(OS),Windows_NT)
	./localdeploy.bat /d
else
	./remotedeploy.sh -d
endif
.PHONY: run-debug

run-debug-reboot: $(OUTPUT_DIR)/$(PROJECT_NAME).n64
ifeq ($(OS),Windows_NT)
	./localdeploy.bat /dr
else
	./remotedeploy.sh -dr
endif
.PHONY: run-debug-reboot

run-debug-upload: $(OUTPUT_DIR)/$(PROJECT_NAME).n64
ifeq ($(OS),Windows_NT)
	./localdeploy.bat /dur
else
	./remotedeploy.sh -dur
endif
.PHONY: run-debug-upload

# test:
#   TODO: run tests

.FORCE:

-include $(DEPS)
