# RedBaboon

A 3D arcade dogfighting game for the original Xbox, written in C++20 with [nxdk](https://github.com/XboxDev/nxdk).

You are Captain "Red" Baboon, a flying ace in a crimson fighter. Gorilla sky pirates are raiding the banana
plantations of Isla Platano: shoot them down with banana machine guns and banana rockets, fly the canyon rings for
bonus points, and bring down their zeppelin, the Silverback.

![Red Baboon chasing a pirate fighter over the island, recorded on a real Xbox](docs/images/gameplay.jpg)

- One level, "Raid on Isla Platano": three waves of pirates, an optional canyon run and a zeppelin boss, about 8-12
  minutes.
- Everything is made by code at start-up: meshes, textures, music and sound effects. The game is a single
  `default.xbe` with no data files.
- Its own renderer on the NV2A through pbkit, at 60 fps in 480p (480i without the HDTV pack). Its own synthesizer and
  mixer drive the AC'97 on a console and play through an APU voice in xemu.
- Runs on a real Xbox and in [xemu](https://xemu.app).

## Controls

| Input | Action |
|---|---|
| Left stick | Pitch and turn (the plane banks into turns and levels its wings when released) |
| Right stick | Up: boost, down: brake; left/right: rudder |
| Right trigger | Banana machine guns |
| Left trigger | Banana rocket |
| A | Stunt: loop, Immelmann turn or barrel roll, by the left stick's direction |
| B | Next target |
| X (hold) | Target view |
| Y (hold) | Look back |
| White / Black | Chase / close chase camera |
| Start | Pause |
| Back | Objective and score |

The full list is in [docs/design.md](docs/design.md) §3; the game shows it too, before the mission starts.

## Building

You need:
- nxdk with its tools built (`make -C $NXDK_DIR tools`), and clang and lld on `PATH` (LLVM 18 is known to work);
- the SDL3 submodule: `git submodule update --init --recursive`;
- for the host tests, g++ or clang.

Tell the Makefile where nxdk is, in the environment or in a `local.mk` (not in git) next to the Makefile:

```make
NXDK_DIR = /path/to/nxdk
export PATH := /usr/lib/llvm-18/bin:$(PATH)
```

Then:

| Command | What it does |
|---|---|
| `make` | builds the game: `build/RedBaboon/default.xbe` |
| `make iso` | `build/RedBaboon.iso`, an XISO to load in xemu |
| `make test` | builds and runs the host tests: flight model, a headless simulation of whole missions, the seed parser |
| `make demos` | the renderer and audio test titles: `build/RenderDemo/`, `build/AudioDemo/` |
| `make preview` | renders the procedural meshes and textures to PNG files in `build/preview/` |
| `make audio_render` | renders every sound effect and music track to WAV files in `build/audio/` |
| `make audio_game` | runs the game's audio through a whole mission on the PC: `build/audio/game_path.wav` |
| `make clean` | deletes `build/` |

Everything a build writes goes under `build/`.

## Running

- **xemu:** load `build/RedBaboon.iso` (Machine > Load Disc). Keep xemu's "Use DSP" audio setting off (the default),
  or the game is silent.
- **Xbox:** copy the `build/RedBaboon` folder to the console's games folder (e.g. `F:\Games\RedBaboon`) and start it
  from your dashboard.
- **Development rigs:** `make xemu` boots the game in xemu on the [OXOS](https://github.com/kooscode/oxos) kernel
  with its serial log ([docs/xemu.md](docs/xemu.md)); `make hardware` installs, runs and records it on a real Xbox
  with a Modxo modchip ([docs/hardware.md](docs/hardware.md)). Both need an OXOS checkout (`OXOS_DIR`).
- **Self-play:** an `autoplay.txt` next to `default.xbe` makes the game fly itself, for testing and recordings;
  `seed=fixed` in it gives the same mission every run ([docs/design.md](docs/design.md) §9).

## Source layout

| Folder | What |
|---|---|
| `src/core/` | vector maths and the mesh and image types shared by everything |
| `src/render/` | the renderer: pbkit on the NV2A, with Cg shaders in `shaders/` |
| `src/audio/` | synthesizer, sound effects, music, mixer, and the AC'97 and APU output |
| `src/content/` | the procedural content: island, sea, planes, zeppelin, props, textures, HUD art |
| `src/game/` | flight model, AI, weapons, mission, camera, HUD and menus |
| `src/platform/` | controller input through SDL3 |
| `tests/host/` | host tests and audio checks, with null renderer, audio and input |
| `tests/render_demo/`, `tests/audio_demo/` | Xbox test titles for the renderer and the audio |
| `tools/preview/` | host preview of the procedural content (a small software rasteriser) |
| `tools/xemu/`, `tools/hardware/` | the xemu and real-Xbox test rigs |
| `docs/` | design, C++ style, the rigs, and the creative brief and reviews (`docs/creative/`) |
| `third_party/nxdk-sdl3/` | SDL3 for nxdk (submodule) |

The code follows [docs/cpp_style.md](docs/cpp_style.md).

## Credits and license

RedBaboon is GPL-2.0 ([`LICENSE`](LICENSE)). Built with [nxdk](https://github.com/XboxDev/nxdk) (pbkit, the
toolchain and runtime) and [nxdk-sdl3](https://github.com/Ryzee119/nxdk-sdl3) ([SDL3](https://libsdl.org) for nxdk).
