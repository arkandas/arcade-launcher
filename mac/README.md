# Mac preview

`run.sh` runs the launcher menu on macOS with the game list and pictures of a Pi on the network.

## Requirements

- Homebrew packages `sdl2`, `freetype`, `libpng`, `pkg-config` and `librsvg`
- Python 3 with Pillow, numpy and scipy
- SSH access to a Pi with the launcher

## Usage

```
./run.sh
```

| Key | Button |
| --- | --- |
| Arrow keys | Stick |
| Enter | A |
| Esc | B |
| Right Shift | Select |

`run.sh` rebuilds the changed sources at each start. Before the menu starts, `tools/sync.sh` copies the game list and pictures from the Pi into `home/`. Games and RetroPie tools run on the Pi. The preview prints the chosen game or tool in the terminal.

| Variable | Use |
| --- | --- |
| `ARCADE_HOST` | SSH address of the Pi, `pi@retropie.local` by default |
| `ARCADE_KEY` | SSH key file |
| `LAUNCHER_MAC_SIZE` | Window size, for example `1280x720` |
| `LAUNCHER_LOAD_DEMO` | Seconds of the loading screen after choosing a game, 3.4 by default |
| `LAUNCHER_DEV` | `1` reads presses from `tools/keys.sh` and takes screenshots for `tools/shot.sh` |

After a card change, delete `home/known_hosts`, since SSH host keys differ from card to card. Measure frame times on the Pi.

## Build

`shim/GLES2/gl2.h` maps the OpenGL ES 2 calls of the menu to OpenGL 2.1 on macOS. `src/mac.cpp` rewrites the shaders for the GLSL version of macOS. The Makefile compiles copies of `main.cpp` and `images.cpp` in `build/src`, with `run/` in place of `/dev/shm`. `home/art` and `home/fonts` link to `../build/launcher`, the output folder of `make art`.
