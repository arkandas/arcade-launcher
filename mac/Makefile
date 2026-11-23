# Builds the menu of the arcade launcher for this Mac:  make
#
# The menu's source files come from the cabinet project (CABINET). main.cpp and images.cpp
# get copied to build/src with the changes a Mac needs. gfx.cpp, text.cpp and sound.cpp
# compile as they are. shim/GLES2/gl2.h stands in for OpenGL ES 2, see there.
#
# make art   draws the menu's pictures and sounds in the cabinet project again (its tools/build_assets.py,
#            which needs a Python with Pillow, numpy and scipy in ARCADE_PYTHON)

CABINET ?= ..
ARCADE_PYTHON ?= python3
FRONTEND = $(CABINET)/frontend
RUN_DIR = $(CURDIR)/run/

CXX = c++
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra
CPPFLAGS = -Ishim -I$(FRONTEND) $(shell sdl2-config --cflags) $(shell pkg-config --cflags freetype2 libpng) \
           -DLAUNCHER_RUN_DIR='"$(RUN_DIR)"'
LDLIBS = $(filter-out -lSDL2main,$(shell sdl2-config --libs)) $(shell pkg-config --libs freetype2 libpng) -framework OpenGL
HEADERS = $(FRONTEND)/brand.h $(FRONTEND)/version.h $(FRONTEND)/gfx.h $(FRONTEND)/text.h $(FRONTEND)/images.h \
          $(FRONTEND)/sound.h shim/GLES2/gl2.h

all: build/launcher

build/launcher: build/main.o build/gfx.o build/text.o build/images.o build/sound.o build/mac.o
	$(CXX) -o $@ $^ $(LDLIBS)

# The Pi keeps the menu's small files in /dev/shm, which the Mac lacks. Here they go to run/.
# The menu also writes to the terminal that started it, to empty the Pi's text console
# behind its picture. Here that goes to /dev/null.
build/src/main.cpp: $(FRONTEND)/main.cpp | build/src
	sed -e 's#"/dev/shm/#LAUNCHER_RUN_DIR "#g' -e 's#"/dev/tty"#"/dev/null"#' $< > $@
	grep -q 'LAUNCHER_RUN_DIR "launcher-action"' $@

# The helper threads that read pictures lower their own priority by a Linux thread number,
# which the Mac lacks. Here they keep the priority they get.
build/src/images.cpp: $(FRONTEND)/images.cpp | build/src
	sed 's#setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);#/* the Mac build leaves the priority of this thread alone */#' $< > $@
	! grep -q SYS_gettid $@

build/main.o: build/src/main.cpp $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

build/images.o: build/src/images.cpp $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

build/gfx.o: $(FRONTEND)/gfx.cpp $(HEADERS) | build/src
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

build/text.o: $(FRONTEND)/text.cpp $(HEADERS) | build/src
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

build/sound.o: $(FRONTEND)/sound.cpp $(HEADERS) | build/src
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

build/mac.o: src/mac.cpp shim/GLES2/gl2.h | build/src
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

build/src:
	mkdir -p $@

art:
	cd $(CABINET) && $(ARCADE_PYTHON) -I tools/build_assets.py

clean:
	rm -rf build

.PHONY: all art clean
