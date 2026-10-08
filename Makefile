CXX ?= g++

# Optional machine-local paths for the ARM64 hook backend.
-include .build/dobby-config.mk

EXTRA_FLAGS =
LUA_PKG ?= $(shell pkg-config --exists 'lua5.4 >= 5.4' && echo lua5.4 || echo lua)
VERSION_HEADER = .build/PluginVersion.hpp
VERSION_SCRIPT = scripts/generate-plugin-version.sh

# OUT lets test builds land somewhere other than the path the desktop loads,
# e.g. `make OUT=.build/dev/spatialoverview.so`.
OUT ?= spatialoverview.so
OBJDIR = .build/obj

SOURCES = main.cpp BarrelShader.cpp Config.cpp DropIndicator.cpp Experiments.cpp Hud.cpp Icons.cpp Memory.cpp Navigator.cpp OverviewGesture.cpp OverviewManager.cpp \
          OverviewPassElement.cpp OverviewRender.cpp Popups.cpp Cursor.cpp Tuning.cpp Window.cpp scrollOverview.cpp
OBJECTS = $(SOURCES:%.cpp=$(OBJDIR)/%.o)
PKGS    = pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon '$(LUA_PKG) >= 5.4'
CXXFLAGS_ALL = -fPIC $(EXTRA_FLAGS) -I.build -g -std=c++2b -Wno-narrowing `pkg-config --cflags $(PKGS)`

# ARM64 requires an external hook backend; the installed compositor only offers
# x86_64 function trampolines. Pass both paths for a local ARM64 build.
DOBBY_INCLUDE ?=
DOBBY_LIBRARY ?=
ifneq ($(findstring aarch64,$(shell $(CXX) -dumpmachine)),)
  ifneq ($(filter-out clean test-tools safe-unload,$(or $(MAKECMDGOALS),all)),)
    ifeq ($(wildcard $(DOBBY_INCLUDE)/dobby.h),)
        $(error ARM64 builds require DOBBY_INCLUDE pointing to dobby.h. See docs/arm64.md)
    endif
    ifeq ($(filter %.a,$(DOBBY_LIBRARY)),)
        $(error ARM64 builds require DOBBY_LIBRARY pointing to a PIC libdobby.a archive. See docs/arm64.md)
    endif
    ifeq ($(wildcard $(DOBBY_LIBRARY)),)
        $(error DOBBY_LIBRARY does not exist. See docs/arm64.md)
    endif
    CXXFLAGS_ALL += -I$(DOBBY_INCLUDE)
    HOOK_LIBS = -Wl,--exclude-libs,ALL $(DOBBY_LIBRARY)
  endif
endif

ifeq ($(CXX),g++)
    EXTRA_FLAGS += -fno-gnu-unique
endif

.PHONY: all clean safe-unload test-tools FORCE

all: $(OBJECTS)
	@mkdir -p $(dir $(OUT))
	$(CXX) -shared -fPIC $(EXTRA_FLAGS) $(OBJECTS) -o $(OUT).next -g `pkg-config --libs pangocairo hyprgraphics` $(HOOK_LIBS)
	mv -f $(OUT).next $(OUT)

# The lens shader is compiled into the plugin (#embed in BarrelShader.cpp).
$(OBJDIR)/BarrelShader.o: shaders/barrel.frag

$(OBJDIR)/%.o: %.cpp $(VERSION_HEADER) $(wildcard *.hpp)
	@mkdir -p $(OBJDIR)
	$(CXX) -c $(CXXFLAGS_ALL) $< -o $@

$(VERSION_HEADER): FORCE $(VERSION_SCRIPT)
	sh $(VERSION_SCRIPT) $@ .

FORCE:

# A scripted virtual mouse for the nested tests (tests/tools/vpointer.c).
VPOINTER = .build/vpointer
VPOINTER_PROTOCOL = tests/tools/wlr-virtual-pointer-unstable-v1.xml

X11MENU = .build/x11-menu
X11GAME = .build/x11-game

test-tools: $(VPOINTER) $(X11MENU) $(X11GAME)

$(X11MENU): tests/tools/x11-menu.c
	@mkdir -p .build
	$(CC) -O2 $< -lX11 -o $@

$(X11GAME): tests/tools/x11-game.c
	@mkdir -p .build
	$(CC) -O2 $< -lX11 -lXrandr -o $@

$(VPOINTER): tests/tools/vpointer.c $(VPOINTER_PROTOCOL)
	@mkdir -p .build/vpointer-gen
	wayland-scanner client-header $(VPOINTER_PROTOCOL) .build/vpointer-gen/wlr-virtual-pointer-unstable-v1-client-protocol.h
	wayland-scanner private-code $(VPOINTER_PROTOCOL) .build/vpointer-gen/wlr-virtual-pointer-unstable-v1-protocol.c
	$(CC) -O2 -I.build/vpointer-gen $< .build/vpointer-gen/wlr-virtual-pointer-unstable-v1-protocol.c -lwayland-client -lm -o $@

# Helper that install-live.sh loads to unload the running build safely.
SAFE_UNLOAD = .build/safe-unload.so

safe-unload: $(SAFE_UNLOAD)

$(SAFE_UNLOAD): scripts/safe-unload.cpp
	@mkdir -p $(dir $@)
	$(CXX) -shared $(CXXFLAGS_ALL) $< -o $@.next
	mv -f $@.next $@

clean:
	rm -rf $(OBJDIR) ./scrolloverview.so ./spatialoverview.so ./spatialoverview.so.next $(VERSION_HEADER) $(SAFE_UNLOAD)
