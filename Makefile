# EoWRecomp build (derived from LAS Recomp, MIT)
# Windows: MSYS2 UCRT64 shell (see docs/BUILD-windows.md), clang + Vulkan renderer.
UNAME := $(shell uname -s)
comma := ,
ifeq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(UNAME) $(OS)),)
$(error Use the Windows MSYS2 UCRT64 shell)
endif
PLATFORM := windows
GEN      ?= generated
# Select a build directory and an optional dependency prefix.
B         ?= build
DEPS      ?=
CC       := clang
CXX      := clang++
IMGUI    := third_party/imgui
GENFLAGS := -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime
# FNPROF=1: game code with the function-entry profiler's hook (EOWRECOMP_FNPROF; runtime/cpu.h FN_ENTER)
GENFLAGS += $(if $(FNPROF),-DEOWRECOMP_FNPROF)

# The game code (made by `make gen` from your own game files) is its own library next to the app (runtime/gamelib.h);
# the app itself holds none of it
# the game's modules and their guest bases (runtime/modules.h): name:base pairs
GAME_MODULES := $(shell sed -n 's/^ *X(\([a-z0-9]*\), *\(0x[0-9A-Fa-f]*\)ull).*/\1:\2/p' runtime/modules.h)
GEN_SRC  := $(foreach m,$(GAME_MODULES),$(wildcard $(GEN)/$(firstword $(subst :, ,$(m)))/*.c))
GEN_OBJ  := $(patsubst %.c,$(B)/obj/%.o,$(patsubst $(GEN)/%,%,$(GEN_SRC))) $(B)/obj/tools/gamelib_entry.o
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) \
            $(wildcard runtime/nvn/*.c) $(wildcard runtime/nvn/maxwell/*.c) runtime/platform/windows.c \
            runtime/platform/discord_presence.c
RT_HDRS  := $(wildcard runtime/*.h) $(wildcard runtime/platform/*.h) $(wildcard runtime/services/*.h) $(wildcard runtime/nvn/*.h) \
            $(wildcard runtime/nvn/maxwell/*.h) $(wildcard runtime/nvn/vulkan/*.h) $(wildcard runtime/ui/*.h)
UI_SRC   := $(wildcard runtime/ui/*.mm)
UI_OBJ   := $(patsubst runtime/%.mm,$(B)/rt/%.o,$(UI_SRC))
IMGUI_OBJ := $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o $(B)/imgui/imgui_widgets.o \
             $(B)/imgui/imgui_impl_sdl3.o

EXE      := $(B)/eowrecomp.exe
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS -D_CRT_RAND_S -DWIN32_LEAN_AND_MEAN -DNOMINMAX
GENARCH  ?= -march=x86-64-v3  # AVX2 + FMA for the lifted FP/SIMD code (simd.h); override for older CPUs
GENFLAGS += -D_FILE_OFFSET_BITS=64 $(GENARCH)
LDFLAGS  := -fuse-ld=lld -Wl,--stack,16777216 $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lvulkan-1 -lwinmm -lole32 -lshell32 -lcomdlg32 -luuid -lshaderc_shared \
            $(if $(RELEASE),-Wl$(comma)--subsystem$(comma)windows)  # release: no console window (log -> eowrecomp.log)
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
BACKEND_OBJ += $(B)/rt/platform/windows_rc.o
WINDRES  ?= windres
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++ $(OSFLAGS)  # the .mm UI sources are plain C++ apart from __APPLE__ blocks
GAMELIB  := $(B)/eowgame.dll
# the runtime functions it calls (RT_API in runtime/cpu.h): exported by the .exe, imported through an import library
# made from tools/windows/eowrecomp.def
IMPLIB   := $(B)/eowrecomp.lib
IMPLIB_DLL := $(B)/eowrecomp.dll.a  # (runtime/gamebuild.c: the import library zig cc links the game code against)
GAMELIB_LDFLAGS := -shared -fuse-ld=lld $(IMPLIB)
GAMELIB_SHARED := -shared  # (+ the import library: runtime/gamebuild.c)
# MSYS converts command arguments but leaves response files untouched.
GAMELIB_RSP_ROOT := $(shell cygpath -am '$(B)')

# GAMELIB_ID: what the game code library is built from (the generator, the headers it is compiled against, the
# flags). A library with another id is rebuilt by the start screen, never loaded.
GAMELIB_SRCS := tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py \
                tools/simd_c.py tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
                runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h runtime/modules.h
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GENFLAGS)') | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16)

RT_OBJ   := $(patsubst runtime/%.c,$(B)/rt/%.o,$(RT_SRC))
RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -DEOW_RUNTIME -Iruntime -Iruntime/nvn $(DEPINC) $(OSFLAGS)
CXXFLAGS := -O2 -g -std=c++17 -DEOW_RUNTIME -I$(IMGUI) -I$(IMGUI)/backends -Iruntime -Iruntime/nvn $(DEPINC)

all: $(EXE) $(if $(strip $(GEN_SRC)),$(GAMELIB)) $(if $(filter windows,$(PLATFORM)),$(IMPLIB_DLL))

$(B)/obj/%.o: $(GEN)/%.c runtime/cpu.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@

$(B)/obj/tools/gamelib_entry.o: tools/gamelib_entry.c $(GAMELIB_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_ID)"' -c $< -o $@
$(B)/rt/gamelib.o: $(GAMELIB_SRCS)
$(B)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'
# the start screen's build of it (runtime/gamebuild.c) uses the same flags
$(B)/rt/gamebuild.o: $(GAMELIB_SRCS) Makefile
$(B)/rt/gamebuild.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"' -DGAMELIB_CFLAGS='"$(strip $(filter-out -Iruntime,$(GENFLAGS)))"' \
                                -DGAMELIB_SHARED='"$(GAMELIB_SHARED)"'

VK_CACHE_SRCS := Makefile $(wildcard runtime/nvn/vulkan/*.c runtime/nvn/vulkan/*.h runtime/nvn/maxwell/*.c runtime/nvn/maxwell/*.h)
VK_CACHE_BUILD_ID = $(shell (cat $(VK_CACHE_SRCS); echo '$(GAMELIB_ID) $(PLATFORM) $(ARCH)') | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16)
SHADERC_BUILD_ID ?= $(or $(shell pkg-config --modversion shaderc 2>/dev/null),unknown)
$(B)/rt/nvn/vulkan/device.o: $(VK_CACHE_SRCS) $(GAMELIB_SRCS)
$(B)/rt/nvn/vulkan/device.o: RTFLAGS += -DEOWRECOMP_VK_CACHE_BUILD_ID='"$(VK_CACHE_BUILD_ID)"'
$(B)/rt/nvn/vulkan/shaders.o: Makefile
$(B)/rt/nvn/vulkan/shaders.o: RTFLAGS += -DEOWRECOMP_SHADERC_BUILD_ID='"$(SHADERC_BUILD_ID)"'

$(B)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

# SMAA (third_party/smaa, MIT): the reference shader as a C string for runtime/nvn/smaa_data.c (both backends build
# their passes from it); the non-ASCII bytes in its comments become '?', and its CRLF line ends plain LF (a CR left in
# the string literals ends them early on macOS clang)
$(B)/inc/smaa_hlsl.inc: third_party/smaa/SMAA.hlsl
	@mkdir -p $(dir $@)
	LC_ALL=C tr -d '\r' < $< | LC_ALL=C tr '\200-\377' '?' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' > $@
$(B)/rt/nvn/smaa_data.o: $(B)/inc/smaa_hlsl.inc
$(B)/rt/nvn/smaa_data.o: RTFLAGS += -I$(B)/inc -Ithird_party/smaa/Textures

$(B)/rt/%.o: runtime/%.cpp $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OSFLAGS) -c $< -o $@

# linked to a temp file, then renamed over: a copy of the game still running keeps its (old) file - relinking in
# place gets running copies killed by macOS (code signature of the changed pages). The temp file keeps the real name
# (in a folder of its own): on Windows the import library records the name the game code library loads it by.
$(EXE): $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(IMGUI_OBJ)
	@mkdir -p $(dir $@)link
	$(CXX) -o $(dir $@)link/$(notdir $@) $^ $(LDFLAGS)
	mv -f $(dir $@)link/$(notdir $@) $@

ifeq ($(PLATFORM),windows)
$(IMPLIB): tools/windows/eowrecomp.def
	@mkdir -p $(dir $@)
	$(or $(ZIG),zig) dlltool -m i386:x86-64 -D eowrecomp.exe -d $< -l $@
$(IMPLIB_DLL): $(IMPLIB)
	cp $< $@
$(GAMELIB): $(IMPLIB) $(IMPLIB_DLL)
endif

# its id file (<library>.id) says what it was built from without loading it (runtime/gamelib.c).
# Windows needs a response file to fit the command-line limit.
$(GAMELIB): $(GEN_OBJ) | $(EXE)
	$(if $(findstring n,$(filter-out --%,$(firstword $(MAKEFLAGS)))),,$(file >$@.rsp,$(foreach obj,$(patsubst $(B)/%,%,$(GEN_OBJ)),"$(GAMELIB_RSP_ROOT)/$(obj)")))
	$(CC) $(GAMELIB_LDFLAGS) -o $@.tmp "-Wl,@$(GAMELIB_RSP_ROOT)/$(notdir $@).rsp"
	mv -f $@.tmp $@
	echo $(GAMELIB_ID) > $@.id

$(B)/rt/ui/%.o: runtime/ui/%.mm $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UIFLAGS) -c $< -o $@

$(B)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/imgui/imgui_impl_%.o: $(IMGUI)/backends/imgui_impl_%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Python environment installed by tools/setup_windows.ps1.
PY := tools/venv/Scripts/python.exe

gen:
	for mb in $(GAME_MODULES); do m=$${mb%%:*}; b=$${mb#*:}; \
	    (cd tools && ../$(PY) gen.py $$m ../extracted/exefs/$$m $$b ../$(GEN)/$$m --hle ../runtime/hle/hle_symbols.txt) || exit 1; \
	done

gen-objs: $(GEN_OBJ)

# Runtime objects only (no generated code, no link): quick compile check
runtime-objs: $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ)

$(B)/rt/platform/windows_rc.o: runtime/platform/windows.rc runtime/platform/windows.manifest $(wildcard docs/icon/icon.ico)
	@mkdir -p $(dir $@)
	$(WINDRES) --preprocessor=clang --preprocessor-arg=-E --preprocessor-arg=-xc-header --preprocessor-arg=-DRC_INVOKED \
	    -I runtime/platform $(if $(wildcard docs/icon/icon.ico),-I docs/icon -D EOW_WINDOWS_ICON) -O coff $< -o $@

# The game code generator as a standalone helper (PyInstaller, tools/eowgen.spec): build/helper/eowgen/, for the start
# screen's setup on the player's machine (runtime/gamebuild.c).
GEN_PY  ?= py -3.12
GEN_VENV ?= build/gen-venv
GEN_VPY := $(GEN_VENV)/Scripts/python.exe

gen-helper: $(GEN_VENV)/.ok
	$(if $(filter windows,$(PLATFORM)),USERPROFILE="$$(cygpath -w ~ 2>/dev/null || echo $$HOME)") \
	$(GEN_VPY) -m PyInstaller --noconfirm --clean --log-level WARN --distpath build/helper --workpath build/pyinstaller \
	    tools/eowgen.spec
	$(if $(filter windows,$(PLATFORM)),$(GEN_VPY) tools/pe_no_cfg.py build/helper/eowgen/eowgen.exe)
	@du -sh build/helper/eowgen

$(GEN_VENV)/.ok:
	$(GEN_PY) -m venv $(GEN_VENV)
	$(GEN_VPY) -m pip install -q capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5 pyinstaller==6.22.3
	touch $@

.PHONY: all gen gen-helper gen-objs runtime-objs
