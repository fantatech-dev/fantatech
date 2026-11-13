# Building Fantatech

Fantatech is written in C17 and uses [SDL3](https://libsdl.org) and SDL3_ttf. Both are git submodules in `thirdparty/`, built from source with the game and linked statically (SDL3_ttf with its own FreeType and HarfBuzz). So there's nothing to install besides a compiler and CMake, and the game is a single executable with no DLLs or libraries next to it.

The build is [CMake](https://cmake.org) 3.24 or newer, with presets for each platform in `CMakePresets.json`. It produces two programs: `fantatech`, the game, and `ftconv`, a command-line tool for the game data (see [Tools](#tools)). Our code must stay warning-free under MSVC `/W4` and gcc/clang `-Wall -Wextra`; CI builds with `-DFANTATECH_WERROR=ON`, which makes warnings errors.

First, fetch the submodules:

```
git clone --recursive https://github.com/fantatech-dev/fantatech.git
```

or, in an existing clone, `git submodule update --init --recursive`.

## Windows: Visual Studio

You need Visual Studio 2026 with the C++ desktop workload, which includes CMake and Ninja.

```
cmake --preset msvc
cmake --build --preset msvc-release
```

(Use the CMake from a Developer PowerShell, or the one inside Visual Studio's install folder.) This generates `build\msvc\fantatech.sln`, which you can also open and build in Visual Studio; there, F5 starts the game with the repo's `mana01\` as the game folder. The programs end up in `build\msvc\bin\Release\` (or `Debug\` with `msvc-debug`).

The `msvc-ninja` preset builds with Ninja instead, and works with any Visual Studio version: run it from a developer command prompt. Visual Studio's *Open Folder* can use it too.

## Windows: MSYS2

In a UCRT64 shell, with `pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja}`:

```
cmake --preset msys2
cmake --build --preset msys2-release
```

## macOS

With the Xcode command line tools and `brew install cmake ninja`:

```
cmake --preset macos
cmake --build --preset macos-release
```

This builds a universal binary (Apple Silicon and Intel) for macOS 11 and later, in `build/macos/bin/Release/`.

## Linux

You need CMake, Ninja, a C/C++ compiler, and the development packages SDL needs for its video and audio backends (X11 and/or Wayland, ALSA/PulseAudio/PipeWire; see SDL's `docs/README-linux.md`). Then:

```
cmake --preset linux
cmake --build --preset linux-release
```

CI builds the Linux release in Valve's Steam Runtime 3 "sniper" SDK container (glibc 2.31) and packages it as `fantatech-linux-x86_64.tar.gz`. SDL is linked statically and loads its X11/Wayland/audio backends at runtime, and the C++ runtime is linked in, so the binary needs nothing but glibc and runs on most current distributions and the Steam Deck. As on Windows, the game data goes in `mana01/` next to the binary. Reports from Linux are welcome.

## Notes

- The source lists are in `CMakeLists.txt`. `ftcore` is a static library with everything that doesn't need SDL (archives, decoders, the installer, the script interpreter, saves); `ftconv` uses only that.
- Ninja presets write `compile_commands.json` in their build folder, which clangd and most editors pick up.
- A development build finds `i18n/` in the source tree, wherever its build folder is.

## Releases

Pushing a tag like `v0.1.0` makes CI publish a GitHub Release: `fantatech-<tag>-windows-x64.zip` and `fantatech-<tag>-linux-x86_64.tar.gz`, staged by `packaging/stage-release.sh` with the programs, `i18n/`, `LICENSE`, `THIRD-PARTY.txt` and the licenses of SDL, SDL_ttf, FreeType and HarfBuzz; `SHA256SUMS.txt`; and a build-provenance attestation per archive. The notes are `.github/release-notes.md` plus GitHub's generated change list. The tag also becomes the version shown in the title bar and by `fantatech --version` (other builds say `dev`). It must match `project(VERSION …)` in `CMakeLists.txt` (`v0.1.0` for `0.1.0`), or the configure step stops, so bump that first.

```
git tag -a v0.1.0 -m "fantatech v0.1.0"
git push origin v0.1.0
```

## Running a build

The game needs the original game data, unpacked from the disc image:

```
fantatech --install "IRON MAIDEN.iso" [folder]
```

Without a folder, this unpacks into `mana01/` next to the executable, which is where `fantatech` looks when started without arguments. It reads the InstallShield cabinets straight from the ISO and checks every file against its MD5. Otherwise, pass the folder:

```
fantatech <game folder> [script] [options]
```

`script` starts somewhere other than the boot script (`start`), for example `starmenu` for the title screen or `Ava022` for a chapter.

On Windows the game opens no console window. Started from a terminal it still prints there, but `cmd` doesn't wait for it to exit; use `start /wait fantatech ...` there, or PowerShell.

| option | what |
|---|---|
| `--label NAME` | start the script at a label (without its `○`/`☆` mark), e.g. `ava021b --label 同・屋上` |
| `--lang xx` | use the translation `i18n/xx.txt` for this run (also `FT_LANG=xx`) |
| `--dev` | enable the developer tools: the dev overlay and Ctrl fast-forward (also `FT_DEV=1`) |
| `--shot out.png --at N` | auto-play until dialogue line N is on screen, save that frame and exit |
| `--shot out.png --after-ms T` | play in real time without input for T ms, save the frame and exit |

Environment variables:

| variable | what |
|---|---|
| `FT_FONT` | font file for menus and translated text, instead of MS Gothic / Hiragino / IPA / Noto |
| `FT_SAVE_DIR` | folder for saves, instead of the per-user data folder |
| `FT_CONFIG` | settings file, instead of `settings.txt` in the per-user data folder |
| `FT_SHOT_DIR` | folder for screenshots (S / F2) |
| `FT_DEV=1` | same as `--dev` |
| `FT_DEV_UI=1` / `2` | with `--dev`, open the dev overlay's info page / scene picker at start |
| `FT_AUDIO_TRACE=1` | log every sound start and dialogue line against the script clock |
| `SDL_AUDIO_DRIVER=dummy` | run without sound (handy with `--shot`) |

Setting `FT_SAVE_DIR`, `FT_CONFIG` and `FT_SHOT_DIR` to a scratch folder keeps test runs away from your real saves.

### Dev overlay

With `--dev`, press `` ` `` (backtick) in the game to open it. It doesn't exist in the original, and
players without `--dev` don't get it. `--dev` also lets you hold Ctrl to fast-forward.

- **Info page:** the current script, line, label and clock, what the script is waiting for, every picture layer, faces, films, sprites, audio, commands we don't handle, and the last lines run.
  - `C` copies all of that to the clipboard as a bug report.
  - `R` replays from the label above the current line.
- **Scene picker** (`S`): every script and its labels. Enter jumps there with a cleared scene.

## Tools

### ftconv

`ftconv` works on an installed copy of the game (the folder holding `text.idx`, `voice.bin`, `exec/` and so on). Run it without arguments for the usage summary.

| command | what |
|---|---|
| `ftconv list <game> [archive]` | list the 17 archives, or one archive's entries |
| `ftconv extract <game> <archive> [entry] [-o dir]` | write entries out as stored (still in the game's formats) |
| `ftconv decode <game> <archive> <entry> [-o path] [-f N \| --all-frames]` | convert one entry: pictures to PNG, sounds to WAV, scripts to UTF-8 text |
| `ftconv decode-all <game> <out> [archive...]` | convert everything into `<out>/<archive>/` |
| `ftconv verify <game> [archive...]` | decode every entry and check its sizes; must print `PASS: 0 problem(s)` |
| `ftconv script <game> [script]` | parse all scripts and report what doesn't resolve, or dump one script parsed |
| `ftconv run <game> <script> [--choices 1,2,..] [--random seed] [--steps N] [-q]` | play the scripts without graphics or sound |
| `ftconv i18n-template <game> [-o out.txt] [--from old.txt] [--bare]` | write a translation table with every key |
| `ftconv movie <game \| file.avi> [-o base] [-f N \| --raw]` | decode the intro movie: `base.wav` plus frame N as `base.png`, or every frame as raw RGB24 in `base.rgb` |

Notes:

- **Archive names** are given without an extension, e.g. `face512`, `event`, `film1024`. Entry names are case-insensitive, like in the original.
- **`decode`:** multi-frame ZDF animations become one PNG per frame with `--all-frames`, or a single frame with `-f N`. `decode-all` writes a folder of `NNN.png` for those.
- **`verify`** is the regression test for the archive and decoder code. Run it after changing anything in `src/vfs` or `src/codec`. It takes a few seconds over all 4.5 GB in a Release build.
- **`script`** with no script name is a quick check that every script still parses and every `goto`/`call` target exists.
- **`run`** answers menus from `--choices` in order, or at random with `--random seed`, and stops at the first screen it can't drive (settings or the album). `--steps` caps the number of lines; `-q` prints only the summary. A random run normally plays a whole route to an ending and should report 0 label warnings and 0 variable errors.
- **`movie`** is the check for the AVI, Cinepak and MS ADPCM decoders: `--raw` output must match
  ffmpeg's byte for byte (`ffmpeg -i movie0508_cinepak.avi -f rawvideo -pix_fmt rgb24 ref.rgb`, and
  `-f s16le ref.pcm` against `base.wav` minus its 44-byte header).
- **`i18n-template`** lists every dialogue `$id` and every menu string. With `--from`, the existing translations are kept, so it also refreshes a table after changes. `--bare` leaves out the Japanese comments, which is how tables in this repo are stored. The format is in [i18n/README.md](../i18n/README.md).

### gen_cp932.py

`python3 src/codec/gen_cp932.py > src/codec/cp932_table.c` regenerates the Shift-JIS (CP932) to Unicode table from Python's own codec. It needs only Python 3.
