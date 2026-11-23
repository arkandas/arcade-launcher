# Builds the programs of the arcade launcher. Run it on the Pi: make -C frontend -j4
#
#   launcher        the menu
#   launcher-boot   the picture on the screen while the Pi starts. It links libc into
#                   itself, so that it can run before the card's file system is there
#   launcher-load   keeps the bar under LOADING moving while a game starts
#
# brand.h holds the name and colours of the brand. tools/build_assets.py writes it from
# brand/brand.json. version.h holds the version number that the Settings screen shows.
#
# Needs g++, make, pkg-config and the headers of SDL2, OpenGL ES 2, FreeType and libpng,
# which RetroPie installs to build its own programs.

CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
CPPFLAGS += $(shell sdl2-config --cflags) $(shell pkg-config --cflags freetype2 libpng)
LDLIBS += $(shell sdl2-config --libs) $(shell pkg-config --libs freetype2 libpng) -lGLESv2 -lpthread

OBJECTS = main.o gfx.o text.o images.o sound.o

all: launcher launcher-boot launcher-load

launcher: $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

%.o: %.cpp gfx.h text.h images.h sound.h brand.h version.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

launcher-boot: boot.c looks.c looks.h brand.h
	$(CC) -O2 -Wall -Wextra -static -o $@ boot.c looks.c -lm

launcher-load: loading.c
	$(CC) -O2 -Wall -Wextra -o $@ loading.c -lm

clean:
	rm -f $(OBJECTS) launcher launcher-boot launcher-load

.PHONY: all clean
