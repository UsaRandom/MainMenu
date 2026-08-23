PROJECT_NAME = N64FlashcartMenu

.DEFAULT_GOAL := all

SOURCE_DIR = src
ASSETS_DIR = assets
FILESYSTEM_DIR = filesystem
BUILD_DIR = build
OUTPUT_DIR = output

# Numbering restarts at the fork rather than continuing upstream's V0.3.2: the presentation
# layer is new code, and a V0.4.0 here would read as one of their releases. It stays at 0.x
# because nothing in this tree has run on a console -- see the banner at the top of README.md.
# tools/deploy.sh overrides this with a six-character code derived from the source tree, so the
# boot plate says which build is running. Four hardware runs in a row have now been explained by
# the ROM on the card not being the ROM that was built -- once because tools/regress.sh had
# overwritten output/sc64menu.n64 (AUDIT 1at), and once still unexplained. A version string that
# is the same for every build cannot tell them apart, and nothing else on the console can.
#
# It is in CONFIG_STAMP below, because it is baked into three objects by -D and make has no other
# way to know it changed.
MENU_VERSION ?= "v1.3.0"
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

# Real box art goes here, and is passed to mkfixture whenever the directory exists. Populate it
# yourself: any tree of <GAMECODE>/boxart_front.png, or the n64-flashcart-menu-metadata layout it
# mirrors. Nothing fetches it -- 1.77 GB of someone else's scans is not this repo's business.
#
# It matters more than it looks. Without this directory an automatic regenerate quietly swaps real
# cards for procedural gradients, and every decode measurement in AUDIT.md was taken against the
# real ones -- 259,633 us a card, dominated by one 2118 x 1457 scan that gradients do not contain.
# real-art.txt and jpeg-art.txt then test gradients while still going green. The trap is that both
# fixtures look fine; only the numbers change.
ARTCACHE_DIR = $(BUILD_DIR)/artcache
FIXTURE_ART  = $(if $(wildcard $(ARTCACHE_DIR)),--art-from $(ARTCACHE_DIR),)
DFS_ROOT_DIR = $(BUILD_DIR)/dfsroot

# ------------------------------------------------------------------- icon corpus --
#
# The faces a profile can wear: SVG text in the cartridge, rasterised on the console by
# src/libs/svg64. Vendored, in assets/icons -- 3,894 CC BY 3.0 SVGs from game-icons.net, 6.87 MB
# of source text by 36 authors, who are named in assets/icons/README.md beside the artwork and in
# docs/CREDITS.md, which is the copy the credits screen shows a player. A clone builds the same
# ROM as this tree does, with no corpus to fetch first.
#
# It was outside the repository until it wasn't. The argument for keeping it out was that git
# keeps a blob forever; the argument that won is that a licence which requires attribution is
# better served by shipping the artwork with the attribution attached than by a build that
# silently produces no picker when a corpus is missing. See assets/icons/README.md.
#
# Point ICON_DIR anywhere else and everything still works -- any tree of <author>/<name>.svg
# packs. Point it at nothing and the build still succeeds with no picker.
ICON_DIR ?= assets/icons

# The IP exclusions -- 286 of the corpus's 4180 icons whose subject is recognisably someone
# else's property, each reviewed individually; see tools/ip-blocklist.txt and svg64's
# docs/ICON-IP-REVIEW.md.
#
# They are already applied to assets/icons: the 286 were never copied in, so they are absent from
# this repository rather than filtered out of it on the way past. Running the exclusion pass over
# the vendored tree would drop 0 icons and print 286 "matched nothing" warnings, so the default is
# empty when ICON_DIR is the vendored tree and the blocklist when it is not -- which keeps the
# exclusions live for anyone pointing this at an unfiltered corpus of their own.
#
# `make ICON_EXCLUDE=` therefore no longer packs the full 4180 against the vendored tree; nothing
# can, because the other 286 are not here. tools/iconcheck.py, in the host suite, is what keeps
# the tree and the blocklist from drifting apart now that no build re-applies them.
#
# Exclusions apply BEFORE the limit below, so capping the count for a quick build cannot
# reintroduce one.
ICON_EXCLUDE ?= $(if $(filter assets/icons,$(ICON_DIR)),,tools/ip-blocklist.txt)

# The full pack is 6,561,304 bytes and takes the ROM from 1.6 MB to about 7.8. That is fine for a
# cartridge and slow for a regression run, which rebuilds the ROM once per input script -- so a
# fixture build caps it. 200 icons is 348,158 bytes and still spans 24 categories, which is
# enough to exercise paging, the cache and the position bar.
ICON_LIMIT ?= $(if $(FIXTURE),200,0)

ICON_PACK  = $(FILESYSTEM_DIR)/icons.pack
ICON_META  = $(FILESYSTEM_DIR)/icons.meta
# Named after the settings that decide the contents, so changing any of them rebuilds and
# changing none of them does not. The corpus tree itself is not a prerequisite: it is nearly four
# thousand files to stat on every build and it changes about never. Now that it is in the
# repository a branch switch can move it under a stamp that says nothing moved -- `make
# clean-icons` if a checkout ever touches assets/icons.
# 'noexcl' rather than the 'all' this said before: with the vendored tree, not running the
# exclusion pass no longer means packing all 4180, it means packing the 3,894 that are here.
ICON_STAMP = $(BUILD_DIR)/.icons-$(ICON_LIMIT)-$(if $(ICON_EXCLUDE),excl,noexcl)-$(shell echo '$(ICON_DIR)' | cksum | cut -d' ' -f1)
HAVE_ICONS = $(wildcard $(ICON_DIR))

# The cheat corpus is a release artifact built by tools/mkcheatdb.py and is never committed, so a
# clean checkout has none and the ROM simply ships without it -- the menu then reads the card's
# copy exactly as it always did. CC BY-SA 4.0, credited in docs/CREDITS.md; see cheatdb.h.
HAVE_CHEATDB = $(wildcard $(BUILD_DIR)/cheats.db)

# PLAIN_ART=1 builds the fixture from mkfixture's own procedural cards instead of the real corpus,
# into a directory of its own so the real fixture is left alone. It exists for one script: the
# real corpus cannot SETTLE. One card in it is 2118 x 1457 and decodes for 38 seconds, so a
# 40-card fixture is still mid-image 1,200 frames in -- and tools/inputs/idle.txt, which asserts
# that a settled frame allocates nothing, was reporting mallocs=0 from a run that never reached a
# settled frame. With procedural cards the whole library is resident in about five seconds and the
# gate can go red, which it promptly did. See AUDIT.md 1u.
ifdef PLAIN_ART
FIXTURE_DIR = $(BUILD_DIR)/fixture-plain
FIXTURE_ART =
endif

# DEMO=1 swaps the fixture for tools/mkdemo.py's tree of invented games with original box art.
# It is what the README screenshots and the demo video are made from, and it exists because the
# ordinary fixture is built from real game titles harvested out of rom_info.c and, when
# build/artcache is populated, real cover scans -- neither of which this repo can publish.
# It is NOT a substitute for the fixture in a regression run: every title in it misses the
# database on purpose, so a scan measured against it exercises only the miss path.
#
# DEMO_NO_PLAYSTATE=1 additionally leaves out the play history, into a tree of its own. The
# video take never changes tab, and with a play history the menu opens on Recent -- one row of
# four, which cannot be scrolled. Without one it opens on N64, which is 24 titles and six rows.
FIXTURE_GEN = tools/mkfixture.py
ifdef DEMO
FIXTURE_DIR = $(BUILD_DIR)/demo$(if $(DEMO_NO_PLAYSTATE),-fresh)
FIXTURE_ART = $(if $(DEMO_NO_PLAYSTATE),--no-playstate)
FIXTURE_GEN = tools/mkdemo.py
endif

# SAMPLE=1 is the third tree: 115 invented games with mkdemo's original art, enough of them that
# every tab is a full grid, and covers drawn at a stated spread of ASPECTS rather than all at the
# right one. It is for looking at layout -- specifically at whether a tile's shape can be read off
# its cover instead of out of a per-system table -- and it is not a regression tree: the mix is
# chosen to contain failures, so measuring anything against it measures the mix.
#
#   make SAMPLE=1 sc64                 the default realistic mix
#   make SAMPLE=1 SAMPLE_MIX=true      the control: every cover the right shape
#   make SAMPLE=1 SAMPLE_MIX=hostile   half the card wrong
SAMPLE_MIX ?= realistic
# A percentage of the 115-title card, so a build can be a card of any size. 435 is about 500
# titles, which is the number DESIGN.md sizes for and the number the 4 MB memory budget has to
# survive -- and a budget extrapolated from 48 and 115 titles is arithmetic, not a measurement.
# The directory carries the scale so two sizes do not overwrite each other's fixture.
SAMPLE_SCALE ?=
ifdef SAMPLE
FIXTURE_DIR = $(BUILD_DIR)/sample-$(SAMPLE_MIX)$(if $(SAMPLE_SCALE),-$(SAMPLE_SCALE),)
FIXTURE_ART = --mix $(SAMPLE_MIX) $(if $(SAMPLE_SCALE),--scale $(SAMPLE_SCALE),)
FIXTURE_GEN = tools/mksample.py
FIXTURE = 1
endif

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
DEV_SRCS = dev/debug_emux.c dev/inputscript.c dev/allocwatch.c dev/hooktest.c
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
# (moved below SRCS -- the stamp is now keyed on the source list too)

N64_ROM_SAVETYPE = none
N64_ROM_RTC = 1
N64_ROM_REGIONFREE = 1
N64_ROM_REGION = E

N64_CFLAGS += -iquote $(SOURCE_DIR) -iquote $(ASSETS_DIR) -I $(SOURCE_DIR)/libs -isystem $(SOURCE_DIR)/libs/miniz -I $(SOURCE_DIR)/libs/midi64/include -flto=auto $(FLAGS)
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
	libs/midi64/bank.c \
	libs/midi64/midi64.c \
	libs/midi64/mixer_glue.c \
	libs/midi64/patches.c \
	libs/midi64/seq.c \
	libs/midi64/smf.c \
	libs/midi64/synth.c \
	libs/picojpeg/picojpeg.c \
	libs/miniz/miniz_tdef.c \
	libs/miniz/miniz_tinfl.c \
	libs/miniz/miniz_zip.c \
	libs/miniz/miniz.c \
	cheats/cheatdb.c \
	cheats/usercheats.c \
	cheats/cheatstate.c \
	library/boxart.c \
	library/cache.c \
	library/libindex.c \
	library/locks.c \
	library/library.c \
	library/playstate.c \
	library/thumbcache.c \
	library/thumbstore.c \
	menu/cart_load.c \
	menu/cardstat.c \
	menu/memprofile.c \
	menu/ini_parser.c \
	menu/fonts.c \
	menu/parental.c \
	menu/path.c \
	menu/cheatcheck.c \
	menu/enginetest.c \
	menu/launchlog.c \
	menu/paths.c \
	menu/profile.c \
	menu/image_decoder.c \
	menu/image_probe.c \
	menu/music.c \
	menu/diagreport.c \
	menu/rom_info.c \
	menu/romcrc.c \
	menu/rompatch.c \
	menu/rompatch_find.c \
	menu/settings.c \
	menu/sound.c \
	screens/boot_plate.c \
	screens/screen_cheatedit.c \
	screens/screen_cheats.c \
	screens/screen_code.c \
	screens/screen_credits.c \
	screens/screen_appearance.c \
	screens/screen_keyboard.c \
	screens/screen_detail.c \
	screens/screen_grid.c \
	screens/screen_launch.c \
	screens/screen_locks.c \
	screens/screen_profiles.c \
	screens/screen_clock.c \
	screens/screen_parental.c \
	screens/screen_settings.c \
	screens/screen_sysinfo.c \
	screens/screen_fault.c \
	libs/svg64/svg64.c \
	libs/svg64/path.c \
	libs/svg64/raster.c \
	ui/draw.c \
	ui/icon.c \
	ui/input.c \
	ui/theme.c \
	ui/tween.c \
	utils/fs.c

# Drop the build whenever the CONFIGURATION or the SOURCE LIST changes.
#
# Two different traps, one mechanism. FIXTURE and DEV_HARNESS change what is compiled without
# touching any timestamp, so make would happily link objects from the other configuration and
# pack a DFS from the other tree.
#
# The source list is in the key for a second reason, learned the hard way twice: adding a file
# leaves every existing .d referring to a header set that no longer matches, and *removing* one
# leaves .d files naming headers that are simply gone -- at which point make refuses to build
# anything at all with "No rule to make target src/cheats/cheatstate.h". The old cleanup list was
# also hand-maintained and had already fallen behind: $(BUILD_DIR)/cheats was never in it.
#
# So the cleanup now finds every .o and .d rather than naming directories, which cannot fall
# behind. This runs while the makefile is read, before any dependency is considered, because a
# rule would fire too late to affect decisions make has already made.
CONFIG_STAMP := $(BUILD_DIR)/.config-$(if $(FIXTURE),fixture,plain)-$(if $(DEV_HARNESS),dev-$(notdir $(basename $(INPUT_SCRIPT)))-$(FBSCALE),nodev)-$(shell echo '$(TUNE)' | tr -c 'A-Za-z0-9=' '_')-$(shell echo '$(SRCS)$(MENU_VERSION)' | cksum | cut -d' ' -f1)
$(shell [ -f $(CONFIG_STAMP) ] || { find $(BUILD_DIR) \( -name '*.o' -o -name '*.d' \) -delete 2>/dev/null; rm -rf $(BUILD_DIR)/inputscript_generated.h $(BUILD_DIR)/.config-* $(BUILD_DIR)/$(PROJECT_NAME).elf $(BUILD_DIR)/$(PROJECT_NAME).dfs; })
$(shell mkdir -p $(BUILD_DIR) && touch $(CONFIG_STAMP))

# FirpleBoot.ttf has no file of its own. It is the same face as Firple-Bold baked at a
# different size, and its rule below names that source explicitly; it is listed here only
# because this is what derives the filesystem entry.
FONTS = \
	Firple-Bold.ttf \
	FirpleBoot.ttf \
	FirpleSmall.ttf \
	FirpleKey.ttf \
	FirpleField.ttf

IMAGES = \
	sc64_logo.png

SOUNDS = \
	cursorsound.wav \
	back.wav \
	enter.wav \
	error.wav \
	settings.wav

# Background music, as Standard MIDI Files synthesised at runtime by src/libs/midi64.
#
# Wildcarded rather than listed, because the list is 28 long and the only thing a listing would
# add is a second place to forget. src/menu/music.c carries the display names and is the file
# that decides what appears in the menu -- dropping a .mid in here without adding it there packs
# a track nothing can select.
#
# For scale: all 28 come to 295,760 bytes. The single bgm.wav64 this replaced was 536,219 for one
# song that no line of code ever opened.
MUSIC = $(wildcard $(ASSETS_DIR)/music/*.mid)
MUSIC_FS = $(patsubst $(ASSETS_DIR)/music/%,$(FILESYSTEM_DIR)/music/%,$(MUSIC))

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
PJPG_OBJS = $(filter $(BUILD_DIR)/libs/picojpeg/%.o,$(OBJS))
DEPS = $(OBJS:.o=.d)

FILESYSTEM = \
	$(addprefix $(FILESYSTEM_DIR)/, $(notdir $(FONTS:%.ttf=%.font64))) \
	$(FILESYSTEM_DIR)/FirpleBody4M.font64 \
	$(addprefix $(FILESYSTEM_DIR)/, $(notdir $(SOUNDS:%.wav=%.wav64))) \
	$(addprefix $(FILESYSTEM_DIR)/, $(notdir $(IMAGES:%.png=%.sprite))) \
	$(FILESYSTEM_DIR)/credits.txt \
	$(if $(HAVE_ICONS),$(ICON_PACK) $(ICON_META),) \
	$(if $(HAVE_CHEATDB),$(FILESYSTEM_DIR)/cheats.db,) \
	$(MUSIC_FS)

$(MINIZ_OBJS): N64_CFLAGS+=-Wno-unused-function -fcompare-debug-second
$(SPNG_OBJS): N64_CFLAGS+=-DSPNG_USE_MINIZ -fcompare-debug-second
# picojpeg is vendored with one patch, marked in the source: four lines accumulated chroma with
# *pDst++ = f(pDst[0], x), which has no sequence point between the read and the increment, so
# whether the colour landed on this pixel or the next one was the compiler's choice. Note that
# -Wsequence-point is deliberately NOT silenced here -- if someone re-pulls the file verbatim the
# build fails rather than quietly decoding wrong colours. The two that are silenced are cosmetic:
# unused progressive-scan header fields, which it parses but cannot decode, and an unused helper.
$(PJPG_OBJS): N64_CFLAGS+=-Wno-unused-but-set-variable -Wno-unused-function
$(FILESYSTEM_DIR)/Firple-Bold.font64: MKFONT_FLAGS+=--compress 1 --outline 1 --size 20 --charset $(ASSETS_DIR)/fonts/charset.txt --ellipsis 2026,1
# The boot plate is the only 32 px type in the product (docs/design/README.md 4.1), and it says
# four fixed strings. Baking the full 7,931-character charset at that size would cost megabytes
# for glyphs nothing renders; the boot charset is 41 characters.
$(FILESYSTEM_DIR)/FirpleBoot.font64: MKFONT_FLAGS+=--compress 1 --size 32 --charset $(ASSETS_DIR)/fonts/charset-boot.txt --ellipsis 2E,3

# Three sizes for the profile picker and the keyboard, all on the 84-glyph charset-ui.txt rather
# than the 7,931-glyph one the body font uses.
#
# The reason is arithmetic. A glyph atlas scales with the square of the type size, and the full
# charset measures 681,040 bytes at 20 px -- so 24, 32 and 40 px of it would be roughly 1.0, 1.7
# and 2.7 MB, about 5.4 MB of glyphs for text that is letters, digits and a handful of marks. On
# the restricted charset the same three come to about 48 KB together.
#
# The cost is that a character outside charset-ui.txt draws as a hole and nothing says so at build
# time. tools/charsetcheck.py is what turns that into a build failure; anything drawn through
# ui_text_font() has to stay inside it.
# The body font again, on the Latin part of the same charset, for a console with no Expansion Pak.
#
# Measured: the full-charset body font is 1,284,208 bytes of RDRAM -- MORE than the two
# framebuffers beside it -- against a 4 MB heap of 2,221,744. It does not fit with anything else,
# and it does not fit with nothing else either. 2,187 of its 2,697 characters are CJK ideographs.
#
# Both faces ship. Only one is ever loaded, by fonts.c, on the answer to one hardware probe -- so
# an 8 MB console reads exactly the font it always did and pays nothing for this but ROM.
$(FILESYSTEM_DIR)/FirpleBody4M.font64: MKFONT_FLAGS+=--compress 1 --outline 1 --size 20 --charset $(BUILD_DIR)/charset-latin.txt --ellipsis 2026,1
$(FILESYSTEM_DIR)/FirpleSmall.font64: MKFONT_FLAGS+=--compress 1 --size 24 --charset $(ASSETS_DIR)/fonts/charset-ui.txt --ellipsis 2E,3
$(FILESYSTEM_DIR)/FirpleKey.font64:   MKFONT_FLAGS+=--compress 1 --size 32 --charset $(ASSETS_DIR)/fonts/charset-ui.txt --ellipsis 2E,3
$(FILESYSTEM_DIR)/FirpleField.font64: MKFONT_FLAGS+=--compress 1 --size 40 --charset $(ASSETS_DIR)/fonts/charset-ui.txt --ellipsis 2E,3
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
$(BUILD_DIR)/charset-latin.txt: $(ASSETS_DIR)/fonts/charset.txt tools/mkcharset-latin.py
	@mkdir -p $(BUILD_DIR)
	@python3 tools/mkcharset-latin.py -i $< -o $@

$(FILESYSTEM_DIR)/FirpleBody4M.font64: $(BUILD_DIR)/charset-latin.txt

$(FILESYSTEM_DIR)/FirpleBoot.font64 \
$(FILESYSTEM_DIR)/FirpleBody4M.font64 \
$(FILESYSTEM_DIR)/FirpleSmall.font64 \
$(FILESYSTEM_DIR)/FirpleKey.font64 \
$(FILESYSTEM_DIR)/FirpleField.font64: $(ASSETS_DIR)/fonts/Firple-Bold.ttf $(ASSETS_DIR)/fonts/charset-ui.txt
	@echo "    [FONT] $@"
	@# A scratch directory PER TARGET, not one shared. mkfont names its output after its input,
	@# so all four of these bakes write build/fontbake/Firple-Bold.font64 -- and under `make -j8`
	@# they run at once, race for that one path, and three of the four `mv`s win while the fourth
	@# finds nothing. It presented as FirpleSmall.font64 simply not existing after a build that
	@# printed [FONT] for it, which reads as a missing rule rather than as a race.
	@mkdir -p $(BUILD_DIR)/fontbake/$(notdir $@)
	@$(N64_MKFONT) $(MKFONT_FLAGS) -o $(BUILD_DIR)/fontbake/$(notdir $@) "$<"
	@mv $(BUILD_DIR)/fontbake/$(notdir $@)/Firple-Bold.font64 $@

# The credits screen renders this file, so the licence text ships as data rather than as string
# literals. That puts it outside the reach of any charset check on the source, which is why
# mkcredits.py does the check itself and writes nothing when it fails: an em dash pasted into the
# Markdown is a hole in the attribution on a console nobody is going to re-read.
# The pack, and the category index built FROM the pack rather than beside it. That direction
# matters: ICON_LIMIT and ICON_EXCLUDE both change what ships, and counts baked from the full
# metadata against a capped pack are a lie the page counter prints. See tools/mkiconmeta.py.
$(ICON_STAMP):
	@rm -f $(BUILD_DIR)/.icons-*
	@mkdir -p $(BUILD_DIR)
	@touch $@

$(ICON_PACK): tools/mkpack.py $(ICON_EXCLUDE) $(ICON_STAMP)
	@mkdir -p $(FILESYSTEM_DIR)
	@python3 tools/mkpack.py "$(ICON_DIR)" -o $@ \
		$(if $(filter-out 0,$(ICON_LIMIT)),--limit $(ICON_LIMIT),) \
		$(if $(ICON_EXCLUDE),--exclude "$(ICON_EXCLUDE)",)

$(ICON_META): $(ICON_PACK) tools/mkiconmeta.py tools/icon-meta.jsonl
	@python3 tools/mkiconmeta.py $(ICON_PACK) -m tools/icon-meta.jsonl -o $@

$(FILESYSTEM_DIR)/credits.txt: docs/CREDITS.md tools/mkcredits.py $(ASSETS_DIR)/fonts/charset.txt
	@python3 tools/mkcredits.py docs/CREDITS.md -o $@ --charset $(ASSETS_DIR)/fonts/charset.txt

$(FILESYSTEM_DIR)/%.wav64: $(ASSETS_DIR)/sounds/%.wav
	@echo "    [AUDIO] $@"
	@$(N64_AUDIOCONV) $(AUDIOCONV_FLAGS) -o $(FILESYSTEM_DIR) "$<"

# Copied, not converted. midi64 parses the Standard MIDI File itself, so there is no .mid64 and
# no build tool in the way -- which is also what makes replacing a track a one-file operation.
$(FILESYSTEM_DIR)/music/%.mid: $(ASSETS_DIR)/music/%.mid
	@mkdir -p $(dir $@)
	@cp "$<" "$@"

# Copied, not converted, and deliberately not compressed: cheatdb reads an 18.7 KB index at boot
# and then one fseek + one fread per game, so a packed blob would have to be inflated whole to
# use any of it. Card copies win over this one -- see cheatdb_open() -- so a newer corpus is
# still a file swap and not a reflash.
$(FILESYSTEM_DIR)/cheats.db: $(BUILD_DIR)/cheats.db
	@mkdir -p $(dir $@)
	@echo "    [CHEATS] $@ ($$(du -h $< | cut -f1))"
	@cp "$<" "$@"

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
		python3 $(FIXTURE_GEN) -o $(FIXTURE_DIR) $(FIXTURE_ART) >/dev/null; \
	fi
	@cp -R $(FIXTURE_DIR)/. $(DFS_ROOT_DIR)/
	@# A hand-built playstate.dat, which is the ONLY way to reach the cache READ path under ares:
	@# the DFS is read-only so nothing can ever write one, but the menu will happily load one that
	@# is already there. Without it Recent and Favourites are unreachable and the grid always
	@# falls back to N64 -- so tabs.txt silently stops testing "open on the first non-empty tab",
	@# which is exactly what happened after a `make clean` removed a copy that had been generated
	@# by hand. Generated here so it cannot go missing again.
	@# Names must exist in the generated tree or the records load and match nothing, which looks
	@# identical to the file being absent. mkfixture.py's SNES titles are three of the four below.
	@# The Game Boy title is there to make Recent a MIXED tab: its box is square and the SNES ones
	@# are portrait, which is the only case in the program where two tile shapes share a grid.
	@# Not for the demo tree, which decides its own play history -- and whose --no-playstate form
	@# needs there to be NO history at all, so that Recent and Favourites stay hidden and the menu
	@# opens on N64. Writing the fixture's here would name titles the demo tree does not contain:
	@# harmless in effect, since nothing matches, but it puts a file where the absence is the point.
	@if [ -z "$(DEMO)" ] && [ ! -f $(DFS_ROOT_DIR)/mainmenu/cache/playstate.dat ]; then \
		python3 tools/mkplaystate.py -o $(DFS_ROOT_DIR)/mainmenu/cache/playstate.dat \
			--played "Chrono Drift.sfc" --played "Star Relic.sfc" \
			--played "Pocket Racer.gb" \
			--favorite "Pixel Knights.sfc" >/dev/null; \
	fi
	@# cheats.db is a release artifact built by tools/mkcheatdb.py, never committed. Staged when
	@# it happens to be there so the cheats screen has something to show under ares. Same rule as
	@# the playstate: a tree that carries its own keeps it.
	@if [ -f $(BUILD_DIR)/cheats.db ] && [ ! -f $(DFS_ROOT_DIR)/mainmenu/cheats.db ]; then \
		mkdir -p $(DFS_ROOT_DIR)/mainmenu; \
		cp $(BUILD_DIR)/cheats.db $(DFS_ROOT_DIR)/mainmenu/cheats.db; \
		echo "    [FIXTURE] staged cheats.db ($$(du -h $(BUILD_DIR)/cheats.db | cut -f1))"; \
	fi
	@echo "    [FIXTURE] staged $$(find $(DFS_ROOT_DIR) -type f | wc -l | tr -d ' ') files into the DFS"
.PHONY: dfsroot

fixture:
	@rm -rf $(FIXTURE_DIR)
	@python3 $(FIXTURE_GEN) -o $(FIXTURE_DIR) $(FIXTURE_ART)
.PHONY: fixture

$(BUILD_DIR)/app.o: .FORCE
$(BUILD_DIR)/screens/screen_grid.o: FLAGS+=-DMENU_VERSION=\"$(MENU_VERSION)\"
$(BUILD_DIR)/screens/screen_grid.o: .FORCE
# The picker draws the boot plate too, on a card with more than one player -- it is the first
# screen there is, so it is the one the plate lifts off. Both it and the grid therefore need the
# version string the plate prints along its bottom edge.
$(BUILD_DIR)/screens/screen_profiles.o: FLAGS+=-DMENU_VERSION=\"$(MENU_VERSION)\"
$(BUILD_DIR)/screens/screen_profiles.o: .FORCE
$(BUILD_DIR)/screens/screen_settings.o: FLAGS+=-DMENU_VERSION=\"$(MENU_VERSION)\" -DBUILD_TIMESTAMP=\"$(BUILD_TIMESTAMP)\"
$(BUILD_DIR)/screens/screen_settings.o: .FORCE
$(BUILD_DIR)/screens/screen_sysinfo.o: FLAGS+=-DMENU_VERSION=\"$(MENU_VERSION)\" -DBUILD_TIMESTAMP=\"$(BUILD_TIMESTAMP)\"
$(BUILD_DIR)/screens/screen_sysinfo.o: .FORCE
# The cheat diagnostic page opens with the version code, so a photographed report can never be
# read against the wrong build's source -- which has already happened, twice.
$(BUILD_DIR)/screens/screen_launch.o: FLAGS+=-DMENU_VERSION=\"$(MENU_VERSION)\"
$(BUILD_DIR)/screens/screen_launch.o: .FORCE
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

# Everything under build/ EXCEPT the three things nothing can cheaply put back. `rm -rf build`
# also took out the fetched cheat corpus -- a download of 1,345 files -- and build/artcache, which
# has to be populated by hand and which nothing fetches at all. Losing artcache silently swaps
# every fixture cover for a procedural gradient and quietly changes every decode number in
# AUDIT.md, while the art scripts go on passing; see AUDIT.md 1w. A clean target that destroys
# something irreplaceable is a trap, not a convenience.
CLEAN_KEEP := cht cheats.db artcache

clean:
	@rm -f ./$(FILESYSTEM)
	@find ./$(FILESYSTEM_DIR) -type d -empty -delete
	@rm -rf ./$(OUTPUT_DIR)
	@if [ -d ./$(BUILD_DIR) ]; then \
		find ./$(BUILD_DIR) -mindepth 1 -maxdepth 1 \
			$(foreach k,$(CLEAN_KEEP),! -name '$(k)') -exec rm -rf {} + ; \
	fi
.PHONY: clean

# Repack the icons without rebuilding anything else. The pack is keyed on a stamp named after the
# settings that decide its contents, not on the 3,894 files themselves -- statting those on every
# build costs more than it catches, given the tree changes about never. It does change when a
# branch switch moves assets/icons under a stamp that says nothing moved, which is the one case
# this exists for.
clean-icons:
	@rm -f $(BUILD_DIR)/.icons-* $(ICON_PACK) $(ICON_META)
.PHONY: clean-icons

# The old behaviour, when it really is wanted. Named so nobody reaches it by reflex.
distclean:
	@rm -f ./$(FILESYSTEM)
	@find ./$(FILESYSTEM_DIR) -type d -empty -delete
	@rm -rf ./$(BUILD_DIR) ./$(OUTPUT_DIR)
.PHONY: distclean

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
