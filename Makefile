#---------------------------------------------------------------------------------
# N64 - emulateur Nintendo 64 pour PS3 (PSL1GHT) - etape 5
#   make        -> n64.self
#   make pkg    -> n64.pkg  (a installer via Package Manager / HEN / CFW)
#   make host   -> banc d'essai PC (Linux/macOS), captures dans shots/
#---------------------------------------------------------------------------------
ifeq ($(MAKECMDGOALS),host)
else ifeq ($(MAKECMDGOALS),soak)
else ifeq ($(MAKECMDGOALS),bench)
else ifeq ($(MAKECMDGOALS),romrun)
else
ifeq ($(strip $(PSL1GHT)),)
$(error "Definissez PSL1GHT dans votre environnement : export PSL1GHT=<chemin vers PSL1GHT>")
endif
include $(PSL1GHT)/ppu_rules
endif

TARGET    := n64
BUILD     := build
SOURCES   := source
DATA      := data
INCLUDES  := include

TITLE     := N64 - Emulateur PS3
APPID     := NSIX00001
CONTENTID := UP0001-$(APPID)_00-0000000000000000
ICON0     := $(CURDIR)/ICON0.PNG
SFOXML    := $(CURDIR)/sfo.xml
PKGFILES  := $(CURDIR)/pkgfiles

CFLAGS    = -O2 -Wall -mcpu=cell -std=gnu99 $(MACHDEP) $(INCLUDE)
CXXFLAGS  = $(CFLAGS)
LDFLAGS   = $(MACHDEP) -Wl,-Map,$(notdir $@).map
LIBS      := -lrsx -lgcm_sys -lio -laudio -lsysutil -lrt -llv2 -lm

LIBDIRS   :=

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------
export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export BUILDDIR := $(CURDIR)/$(BUILD)

CFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD := $(CC)
export OFILES   := $(CFILES:.c=.o)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   $(LIBPSL1GHT_INC) -I$(CURDIR)/$(BUILD) -I$(PORTLIBS)/include
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) $(LIBPSL1GHT_LIB) -L$(PORTLIBS)/lib

.PHONY: $(BUILD) clean pkg run host soak bench romrun

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo nettoyage ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).pkg build_host

pkg: $(BUILD) $(OUTPUT).pkg

run:
	ps3load $(OUTPUT).self

HOSTSRC := $(filter-out source/main_ps3.c,$(wildcard source/*.c))
host:
	@mkdir -p build_host shots
	cc -std=gnu99 -O2 -Wall -o build_host/n64host $(HOSTSRC) tools/host_main.c -lm
	./build_host/n64host

soak:
	@mkdir -p build_host
	cc -std=gnu99 -O2 -Wall -o build_host/romtests source/n64rom.c tools/rom_tests.c
	./build_host/romtests
	cc -std=gnu99 -O2 -Wall -o build_host/cputests source/vr4300.c tools/cpu_tests.c -lm
	./build_host/cputests
	cc -std=gnu99 -O2 -Wall -o build_host/gfxtests source/rdp.c source/gsp.c tools/gfx_tests.c -lm
	mkdir -p shots && ./build_host/gfxtests
	cc -std=gnu99 -O2 -Wall -o build_host/audiotests source/aspd.c tools/audio_tests.c -lm
	./build_host/audiotests
	cc -std=gnu99 -O2 -Wall -o build_host/demotest source/n64.c source/vr4300.c source/n64rom.c source/rdp.c source/gsp.c source/aspd.c source/jit.c source/demo_rom.c tools/demo_test.c -lm
	./build_host/demotest
	cc -std=gnu99 -O2 -Wall -o build_host/jittests source/jit.c source/vr4300.c tools/ppc_interp.c tools/jit_backend_pc.c tools/jit_tests.c -lm
	./build_host/jittests
	@if command -v python3 >/dev/null; then python3 tools/jit_encoding_check.py; else echo "python3 absent : verification de l'encodage sautee"; fi

# fait tourner une vraie ROM sur PC : make romrun ROM=chemin/jeu.z64 FRAMES=3000
# options : JIT=1 (recompilateur, code PowerPC interprete), THREAD=1 (dessin sur un 2e fil), PACE=16683 (temps reel)
romrun:
	@mkdir -p build_host shots
	cc -std=gnu99 -O2 -pthread -o build_host/romrun source/n64.c source/vr4300.c source/n64rom.c source/rdp.c source/gsp.c source/aspd.c source/jit.c tools/ppc_interp.c tools/jit_backend_pc.c tools/worker_pthread.c tools/romrun.c -lm
	./build_host/romrun "$(ROM)" $(or $(FRAMES),3000) "$(SCRIPT)"

bench:
	@mkdir -p build_host
	cc -std=gnu99 -O2 -o build_host/bench source/n64.c source/vr4300.c source/n64rom.c source/rdp.c source/gsp.c source/aspd.c source/jit.c source/demo_rom.c tools/bench.c -lm
	./build_host/bench 300
	cc -std=gnu99 -O2 -o build_host/rdpbench source/rdp.c tools/rdp_bench.c -lm
	./build_host/rdpbench 60

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:  $(OFILES)

-include $(DEPENDS)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
