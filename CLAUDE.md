# fantatech — Kotetsu engine reimplementation

**fantatech** is a C + SDL3 reimplementation of the engine behind *Neon Genesis Evangelion: Girlfriend
of Steel — Special Edition* (CyberFront × Gainax, Windows XP, 2006). The internal engine name is
"Kotetsu" (鋼鉄). The original binary is `mana01.exe` (MSVC, C++, D3D9 + statically linked D3DX,
DirectSound, DirectInput8, SafeDisc). The engine runs on the player's own copy of the game data; no
original code or assets go in this repo.

## Layout

| path | what |
|---|---|
| `CMakeLists.txt`, `CMakePresets.json` | the one build, for every platform (presets `msvc`, `msvc-ninja`, `msys2`, `macos`, `linux`). The source lists live there |
| `.github/workflows/ci.yml` | CI: Windows MSVC + MSYS2 and macOS (universal), warnings as errors |
| `src/core`, `src/vfs`, `src/codec`, `src/script` | **ftcore** static lib, no SDL: helpers, archives, asset decoders, the ISO installer, the script interpreter. `src/game/assets.c`, `i18n.c`, `save.c` and `config.c` are in it too. `src/codec/gen_cp932.py` regenerates `cp932_table.c` |
| `src/game/` | the SDL runtime and our screens |
| `src/main.c` | the **fantatech** exe |
| `src/tools/` | **ftconv**, the data tool |
| `i18n/` | translation tables, one `<lang>.txt` per language (see its README) |
| `docs/` | `BUILDING.md` (builds, run options, tools) and the README screenshot |
| `thirdparty/SDL`, `thirdparty/SDL_ttf` | git submodules at `release-3.4.20` / `release-3.2.2`, built from source and linked statically (SDL_ttf with its vendored FreeType and HarfBuzz) |
| `mana01/` (gitignored) | the installed game, the engine's data dir |

## Build, run, test

`docs/BUILDING.md` has the full instructions. In short:

```
git submodule update --init --recursive
cmake --preset msvc
cmake --build --preset msvc-release        (or msvc-debug)
build\msvc\bin\Release\fantatech.exe mana01 [script]
```

CMake comes with Visual Studio (`C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`,
not on PATH here). `build\msvc\fantatech.sln` opens in Visual Studio; F5 runs fantatech with the
repo's `mana01\`. The exe is self-contained (SDL is linked in, no DLLs). A dev build finds `i18n/`
through `FT_SOURCE_DIR`. Git is `C:\msys64\usr\bin\git.exe` (not on PATH); anything that talks to
a remote over https (submodules) has to run in a UCRT64 login shell. MSYS2: `cmake --preset msys2`
(needs `mingw-w64-ucrt-x86_64-cmake`). When driving bash from PowerShell 5.1, write the commands to
a script file and run `bash -l script.sh` with `MSYSTEM=UCRT64` and `CHERE_INVOKING=1`; PowerShell
mangles quotes in inline `bash -lc '…'`.

Our code stays warning-free under MSVC `/W4` and gcc/clang `-Wall -Wextra`; `-DFANTATECH_WERROR=ON`
(CI) makes warnings errors. The flags apply to our targets only, not SDL's.

**Smart App Control** is on on this PC and judges every freshly linked unsigned exe. With CMake's
default MSVC flags it blocked our exes often ("An Application Control policy has blocked this
file": 7 of 19 fresh links). `ft_target_options` therefore gives our targets the same flags the
old Visual Studio projects had (`UNICODE`/`_UNICODE`; Release `/Zi /GL /Gy /Oi`, `/DEBUG /OPT:REF
/OPT:ICF /LTCG`); after that 1 of about 50 fresh links was blocked, against 0 of 14 for the old
projects. Keep those flags. A blocked exe usually runs after a relink (delete it and build
again); the CodeIntegrity/Operational event log (event 3033/3118) shows the blocks.

**SDL_ttf and MSVC 14.51:** at `/O2` (Release, RelWithDebInfo) Visual Studio 2026's compiler
miscompiles SDL_ttf, and MS Gothic's small sizes (the 22 px menu labels) come out with wrong glyphs
("Music" as "Musio"). Bisected by unoptimizing one dependency at a time: only SDL_ttf matters, and
`/O1` fixes it, so CMakeLists.txt builds SDL_ttf with `/O1` under MSVC. Check the settings screen
(`fantatech mana01 system01 --shot`) after a compiler or SDL_ttf update; Debug and Release must render
it pixel-identically.

**Game data.** Install it from the disc image into `mana01\` at the repo root, which is what the
debugger and the commands here use:

```
build\msvc\bin\Release\fantatech.exe --install "IRON MAIDEN.iso" mana01
```

Without a folder it unpacks to `mana01\` next to the exe, and `fantatech.exe` with no game dir runs
from there. A lone `.iso` argument does the same, so dropping the ISO on the exe installs it.

On Windows `fantatech.exe` is a GUI-subsystem app (no console window when played).
`console_open()` in `main.c` attaches to the terminal it was started from, leaves redirected
output alone, and for an install with neither opens a console of its own, which waits for Enter
at the end. Startup errors go to a message box only when nobody would see the log, so scripted
`--shot` runs never block. `cmd` doesn't wait for GUI apps (use `start /wait` there); PowerShell
waits when the output is piped, as the commands here do. `src/vfs/install.c` finds
`data1.hdr`/`dataN.cab` in the ISO (`iso9660.c`, primary volume tree), reads the InstallShield
cabinet (`iscab.c`, layout from the unshield project), inflates the chunks (`codec/inflate.c`) and
checks each file's MD5 (`core/md5.c`). Only volume 2 on is installed: the 17 archive pairs, the movie
and `exec/`. Volume 1 is InstallShield's own support files. Every game file passes the header MD5;
the 5 installer files (`setup.*`) that wouldn't are stored loose on the disc, not in the cab.

File access goes through `ft_fopen`/`ft_remove`/`ft_rename`/`ft_mkdirs`, which take UTF-8 paths on
Windows (tested with the exe and ISO in Japanese-named folders).

**Regression checks:**
- `ftconv verify mana01` after any change to `vfs`/`codec`. It must print `PASS: 0 problem(s)`, and
  takes about 5 s over all 4.5 GB in a Release build.
- `ftconv script mana01` after parser changes: all 65 scripts parse, and only the 5 known targets
  don't resolve (see [Script language](#script-language)).
- `ftconv run mana01 start --choices 1 --random <seed> -q` after interpreter changes. Random
  playthroughs finish full routes (52–103 scripts) and reach all four endings (Ava041, Ava042, Ava043,
  ja003) with 0 label warnings and 0 variable errors. The save panel is left at once (`number`
  999); runs stop only at the settings and album screens, which need a real host.
- `fantatech.exe mana01 <script> --shot out.png --at N` autoplays (instant advance, first menu
  entry) until dialogue line N is showing, saves that frame and exits. `--after-ms T` instead plays
  in real time with no input for T ms (Ava011 shows 36 lines in 150 s). With
  `SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy` no window opens and nothing plays.
- Compare layout against the VM with `ffmpeg … hstack` of a VM frame and a `--shot`.

`ftconv decode-all mana01 <out> [archive...]` writes `<out>/<archive>/`: pseudo-BMPs become `.png`,
a ZDF becomes `.png` or a folder of `NNN.png` when it has more than one frame, scripts become UTF-8
`.txt` and audio `.wav`. The PNG writer (`src/tools/png_write.c`) does Paeth filtering and
fixed-Huffman deflate, so output is roughly 8–18% of raw size.

## Code conventions

- C17, warning-free under MSVC `/W4` and gcc `-Wall -Wextra`, `/utf-8` source.
- **Naming:** our code is "fantatech". Public symbols use the `ft_` prefix (macros `FT_`), files
  and targets use `ft*` (`ft_common.h`, `ftcore`, `ftconv`). The `kt_` prefix is reserved for
  Ghidra names of the *original* Kotetsu functions, which comments cite with their address, e.g.
  `kt_zdf_decode_keyframe (0x401620)`. Never use `kt_` for our own identifiers.
- Layering: `core`/`vfs`/`codec`/`script` must not depend on SDL (so tools can use them
  headless); `game` may.
- All multi-byte reads from game data are explicit little-endian; never cast structs over file
  bytes.
- Script text is Shift-JIS (CP932) after de-obfuscation. Convert to UTF-8 at load.

## Reverse engineering

- **Target:** `mana01_dump.exe`, a post-decryption memory dump of `mana01.exe` (kept outside the
  repo) that doesn't run standalone. The disc copy `mana01/exec/mana01.exe` is still
  SafeDisc-encrypted (≈67% of its bytes differ), so don't analyze it.
- **Tool:** Ghidra via the `ghidra-mcp` server, project `fantatech`, program `/mana01_dump.exe`
  (image base 0x400000, ~4500 functions). Inline Java scripts (`run_script_inline`) are enabled, so
  use them for bulk renames and comments. After a Ghidra restart, reopen `/mana01_dump.exe` with
  `open_program` first.
- **Naming:**
  - Original names come from `Class::method` debug strings. Put the function in a Ghidra class
    namespace and keep the name verbatim: `uububffer::create`, `mytext01::loadStringOne`,
    `dfs01BlockR::Load`, `uustring::Init`, `vvsoundfile::Init`.
  - Names we invent for original functions start with `kt_`, either free (`kt_archive_open`) or
    inside a proven class (`uudxFrame01::kt_frame_dispatch`). This is the Ghidra side only; our C
    code uses `ft_`.
  - Mark the evidence level in the function comment: `INFERRED:` / `OUR NAME:`.
  - Many `::` strings are resource/debug **tags** passed to a callee (for example
    `frmSelect02::bmpMessage01` names a member), not the referencing function's own name. Read the
    call before naming.
  - Strings referenced only from MSVC `Catch@…` funclets belong to the function just before the
    funclet.
- **Calling conventions:** MSVC C++ methods are `__thiscall`. Ghidra's decompiler shows them as
  `__thiscall`, or as `__fastcall` when ECX is the only argument (EDX is never used), but doesn't
  store them. Set `__thiscall` on the function and leave the signature source `DEFAULT` so the
  stack arguments keep being inferred.
- **Ground truth:** the original game running in the user's XP VM. A decoder that "looks right"
  isn't confirmed. Check orientation and colour against something unambiguous (text, faces), and
  check byte budgets match exactly. The binary is C++ and has real 3D (`.X` meshes via D3DX).

## Game data

- `exec/ktxp01.ini`: native 1024×768 @ 60 Hz, font `ＭＳ ゴシック`, EVENT_TEXTURE_SIZE 1/2/3 =
  740×400 / 1024×554 / 1894×1024 (event / eve1024 / eve2048), FACE_TEXTURE_SIZE 1/2/3 = 160×180 /
  228×256 / 455×512 (face / face512 / face1024). Defaults: 2 and 2.
- The data root (`mana01/`) holds 17 `.idx` + `.lb5`/`.bin` archive pairs, `movie0508_cinepak.avi`
  and `exec/` (exe, ini, `settool5.exe`).

### Asset formats (verified)

Each format was rewritten from Ghidra decompiles and is checked by `ftconv verify` over all 7044
entries: exact byte budgets, all frames, zero problems. Spot checks were visual (face, NERV text,
alpha UI atlas, last delta frame, DFS0). The header comment of each decoder has the full layout and
the original's function addresses.

| format | where | decoder |
|---|---|---|
| `.idx` + `.lb5` fixed 24-byte records / `.idx` + `.bin` variable records; lookup is case-insensitive (`__stricmp`) | all 17 pairs | `src/vfs/archive.c` |
| pseudo-BMP, opaque `0x88888888` (6333) / alpha `0x88888899` (79) | face*, bg02, frame, event*, stafroll, textbmp | `src/codec/bmp.c` |
| ZDF `DFS1` (131) / `DFS0` (3, 28-byte header) | event, film1024, film2048 | `src/codec/zdf.c` |
| RIFF WAVE PCM (433) | music, se, voice | `src/codec/wav.c` |
| obfuscated CP932 script text (65) | text, text3d | `src/codec/text.c`, `cp932.c` |
| AVI: Cinepak 740×400 @ 30 fps (1336 frames) + MS ADPCM stereo 44.1 kHz | `movie0508_cinepak.avi` (loose file) | `src/codec/avi.c`, `cinepak.c`, `msadpcm.c` |

Details that are easy to get wrong:
- ZDF delta modes 1 and 2 put the HIGH bit field on byte 0 and the LOW field on byte 2. In mode 1,
  the extra bit from the u16 goes to byte 0.
- `DFS0` exists (3 large event files) and differs from `DFS1` only in header size.
- ZDF keyframes use a 4-byte-padded stride and deltas an unpadded `w*3` stride. No shipped ZDF has
  `w % 4 != 0`, so the difference never shows.
- `textbmp` holds 3418 pre-rendered dialogue lines, named like the script's `$ID`s (`a21b015` =
  「むー！」). All 3369 dialogue `$id`s have one; the original draws dialogue from these images, not
  a font.
- `frame/*.bmp` are UI sprite atlases (album panel, button animation frames, digit strip).

## Script language (verified)

The parser is `src/script/script.c`. `ftconv script mana01 [name]` parses all 65 scripts and
reports what doesn't resolve, or dumps one script.

The original interpreter, in Ghidra:
- `kt_script_exec_line` (0x41a770) is a `strcmp` chain on the head token. It matches some commands
  exactly (`kt_script_head_is`) and some by prefix (`kt_script_head_starts`), and routes namespaces
  to `kt_cmd_*` sub-dispatchers (timer, cmnd, sound, voice, se1/se2, music, midi, bg/bg02/bg03,
  event, dd*, film, selspeed, sprite, face, select, albumpanel, saveload, …).
- The tokenizer and line table are around 0x431000–0x432800. Block and label tables are around
  0x42c000–0x42e000.

Language:
- **Not executed:** lines starting with `＊ ○ # . ; ；`. `；` (full-width) is also a comment.
  `.name[...]` are declarations handled at load time.
- **Labels:** `○` (1089), `＊` (229), `#` (240), and `☆` (190) save points. Lookup is
  case-insensitive. Of 1144 `goto`/`call` targets, 635 are `○`, 392 `＊` and 112 `#`. The 5 that
  don't resolve are in debug/test scripts, plus `call システムオープニング`, which lives in
  `saveload.txt` (merged in at run time).
- **Blocks are indentation-based**, resolved at load:
  - `if cond`: when false, skip the indented lines below (`kt_block_end_after`).
  - `loop [cond]`: jump back to the start of the indented run above while cond holds, or forever
    if there's no cond (`kt_block_start_before`).
- **Control:** `goto call return if loop load end on.goto on.call loadjump loadMerge sleep dump`.
  `call` uses a line-number stack (`LineNumberStack`).
- **Statements:**
  - `%legacy.cmd args` (160 distinct) and `object.cmd args` (`bg02.open`, `voice.play`,
    `dd.render`, `albumpanel.set`, …).
  - Bare commands: `select`, `print`, `OnKeyJump`, `cover.*`, …
  - Assignment `lvalue = expr`, with Japanese variable names and arrays (`heightStaffFont[c1]=40`).
  - Dialogue `speaker「$id text」n`.
  - Anything else is shown as text.
- **Arguments** are separated by top-level commas, and strings use `"..."`.
- **Dialogue** (`kt_script_dialogue_kind`): any non-command line whose first token contains `「` or
  `、`. That includes narration without brackets.
- **Fallback** (`kt_script_exec_fallback`): a head that names a label is an implicit `call`,
  `name="..."` is a string assignment, and anything else is an expression statement. Bare names
  declare variables, which is what `Ai10.txt` (`；＃変数`) is. Location names are declared there too
  and numbered by each chapter's script at its start (game02: `Ｖー１エレベーター = 11`, …), which
  is how `from==Ｖー１エレベーター` comparisons work; jumping past that start leaves them all 0.
- **Expressions** (`kt_expr_fold`) fold strictly LEFT TO RIGHT with no precedence: `+ - * / %`,
  `<= >= == != < >`, `&& ||`, and `( )` for grouping. `x/0` leaves `x` unchanged. Operands are
  `name[i]`, declared variables (case-insensitive) or integer literals. An undeclared name reads as
  0 and is an error. A bare word that isn't a variable is a literal string where a command expects
  text (`%face 2,シンジ,…`).
- **Boot:** the engine merges `Ai10.txt` (variables), keeps `saveload.txt` resident (its `システム` /
  `システムオープニング` labels are called from every scene's system verb), and runs `start.txt`:
  logo, `movie0508_cinepak.avi` through `dd.load movie`, then `load starmenu`.
- **Title menu:** `starmenu.txt` (`select02 "最初からはじめる",…`) navigates through `#` labels.
- **Engine-set variables:** `select.number`, `select02.number`, `number`, `savedatanum`,
  `savedataEmpty`, `spriteLoadPAINTCount` and `c1` (CheckInitNow / CheckMovieSkip /
  movieplay.CheckFile results).
- **`%midi.play n`** uses the table at `0x4efbe4`: 1 as, 2 db, 3 tbst, 4 mtw4, 5 bfit, 6 amwt,
  7 ssdt, 8 wifp, 9 (none), 10 samisi. 0 stops the music. The script comment
  `%midi.play 5 ;BAREFOOT IN THE PARK` = bfit confirms it. Loading a save restarts track 5
  (game01/02/04) or 6 (game07/19b/19c).

## Interpreter (`src/script/expr.c`, `interp.c`)

`ft_interp` handles flow, variables and the menu commands (`%select.*`, `select`/`select02`,
`%cmnd.*`). It hands dialogue, menu picks and every other command to an `ft_host`: the SDL runtime,
or `ftconv run`'s headless host. `ft_interp_boot` follows the engine's boot sequence, and
`ft_interp_resume` restores a saved call stack.

## Runtime (`src/game`)

- **`assets.c`** (in ftcore, no SDL) opens all 17 archives, parses every script and finds assets by
  role:
  - faces: face512 → face → face1024;
  - CG: eve1024 → event → eve2048 → bg02 → film1024;
  - `textbmp` dialogue lines;
  - sounds: voice → se;
  - music by track stem.
- **`runtime.c`** is the SDL `ft_host`. It draws the picture planes, sprites, selSpeed pans, films,
  8 face slots and the dialogue line, runs the menus and `%timer*` waits, and plays audio. Commands
  it doesn't handle are logged once each.
- **`font.c`** rasterizes menu and translated text with SDL_ttf in MS Gothic, the font
  `ktxp01.ini` names, from the player's `%WINDIR%\Fonts\msgothic.ttc` (face 0). Elsewhere it tries
  `msgothic.ttc`, then Hiragino Kaku Gothic W3 (macOS), IPA Gothic and Noto Sans CJK. `FT_FONT=path`
  overrides it; without a font, menus are blank and play goes on.

### Frames and the script clock (verified)

- **One frame per 14 ms tick.** The script thread runs the scene script (`FUN_00418430`). A render
  thread (`FUN_00418480`) runs a text3d script (`dd01`/`black`/`movie`: `dd.render`, `dd.wait`,
  `loop`), and `dd.wait` blocks on the event `kt_timer_tick` sets every 14 ms. So fades, sprite
  steps, selSpeed and films all count frames (`FRAME_MS`).
  - Display commands (`bg*`, `event.*`, `%s`/`%event`, `%face`, `%film`, `%sprite`, `%selSpeed`,
    `dd.*`, dialogue lines) are posted to the render thread, and the script waits until it takes
    them; it takes one after each line of its loop. We charge 2 per frame
    (`DISPLAY_PER_FRAME`), fitted to the VM's Ava011 street scene (~95 sprite commands; 2.79 s
    against the VM's 2.78).
  - Still unexplained against the VM: about 1.0 s more before Ava011's street scene (16 sprite
    loads, probably a load stall; sound loads aren't it, 008 at 4.6 MB costs nothing).
  - Frames don't catch up after a stall, since the original also loads on the render thread.
- **Script clock:** `kt_timer_init` calls `timeSetEvent(14 ms, periodic)` with `kt_timer_tick`
  (0x456550), which adds 1.40 units per tick, so **1 unit = 10 ms**.
  - `%timer.start n` sets the clock to n (not 0 when n is given; 22 uses).
  - `%timer.wait N` blocks until the clock reaches N, so it's absolute.
  - `%timer.delta N` waits for the last target + N, and `%timer.adjast n` sets the last target.
  - `%timer.loopcut.wait N` runs sprite loops until the clock reaches N.
  - `%timer N` is a separate N × 10 ms countdown.
- **Waits can't be skipped by the player.** The skip flag `0x5c2058` is set only by `OnKeyJump`.
  With it armed, a key jumps the clock to the target and execution jumps to the label (one-shot).
  Otherwise clicks only end dialogue holds and pick menu entries. Ctrl fast-forward (`--dev`
  only) is our addition, and it jumps the clock like the original's skip so later waits stay
  aligned; it's a testing aid, not good enough to give players.
- **Dialogue hold:** after each line, `kt_script_exec_dialogue` → `FUN_00456c40` holds the script
  for N × 10 timer ticks of 14 ms, where N is the number after `」` (`kt_dialogue_parse` 0x427320;
  no number or 0 means 100 ticks = 1.4 s).
  - **But** it only looks for `」` in token 1 (the first token after `speaker「$id`), and the
    tokenizer (`FUN_00437290`) splits at ASCII spaces, commas and
    `! & < = > | % * + - / ; ( ) [ ] " '`. So `アスカ「$a11010 Vielen Dank!」1` holds 1.4 s, not
    140 ms: that is the pause between "Vielen" (009a) and "Dank" (009b) in the VM. A narration line
    (no `「`/`」` in tokens 0/1) keeps the previous hold. `dialogue_hold_ticks()` in runtime.c; 9 of
    3373 lines differ from the naive reading.
  - A click ends the hold early. The text stays up until the next line or a `%faceclose` /
    `%facecloseAll`, which take it down with the face (kt_cmd_face 0x427292/0x4272c0 →
    `FUN_00415eb0`; matches every clear in the VM recording of Ava011/Ava021, including one delayed
    by a `」30` hold).
  - There's no separate "wait for click" state. `」1` (voiced lines, followed by `%timer.wait`)
    holds 140 ms; `」20` (unvoiced adventure lines) holds 2.8 s.
- **Checked against a VM recording of Ava011 / Ava021:** scene cuts on the script clock, `%sprite`
  steps (39 ms at Timer 5), `%selSpeed` pans (one step per 14 ms frame), the Bure shake (42 ms
  beats, 46 beats), film frames (98 ms at rate 2, 49 ms at the post-`new` rate 4).

### Pictures

- **Picture planes** (struct `plane`): bg, event and ten bg02 layers, each with two pages.
  - The script thread rewrites its short forms (`kt_script_exec_line` 0x41b151…): `%s` → `bg.open`,
    `%s.clear` → `bg.close`, `%event`/`%eventclose` → `event.open`/`close`,
    `%black`/`%qblack`/`%white`/`%qwhite` → `bg.black`/`bg.white` (a solid page, not an overlay).
  - Fades step alpha once per frame (`FUN_0043e0f0`): `fadein`/`fadeout` by 5 (51 frames, 0.7 s),
    the quick forms by 60, `overlapTimeStep name,n` by n. `overlap` crossfades by fading the old
    page out on top. `event.open` fades in quickly, and `event.close` fades out quickly.
  - `bg02.current n` picks the layer that `bg02.*` acts on. `FSystem 1`/`3` frees all bg02 layers
    (and bg03); that's how the title menu's layers go away when a game starts.
  - A ZDF shown on a plane plays once and holds its last frame.
- **Draw order** (frame update 0x442ba0): bg, selSpeed, sprite, film, event, bg02 0–3, bg02 4–9,
  then faces and the dialogue line. bg02 covers sprites and pans, and scripts black them out with
  `bg02.black` and reveal them with `bg02.fadeout` (ava023b).
- **Picture scale by archive** (`FUN_00432bf0` / `FUN_00432d10`, used by `FUN_0043f900`): a sprite,
  selSpeed or film picture found in eve1024/film1024 is drawn ×1, in `event` (740-wide art)
  ×1024/740, in eve2048/film2048 ×1024/1894. Some films exist only in `event` (the ava022b
  sync-ratio bars C040X1–3), so their ZDF x/y and size are scaled too. `ft_blob.arc` +
  `ft_assets_scale()`.
- **`%sprite`** (kt_cmd_sprite 0x426410, cell class 0x435b20–0x435f80):
  - Each load adds a picture: `loadCOPY` is drawn as is (opaque, but the alpha pseudo-BMPs keep
    their transparency: Ava022/Ava026's train over the scrolling sky, Ava036's binocular masks),
    `loadAND` multiplies (black shape on white), `loadPAINT` adds (shape on black),
    `loadPaintAndMask p,m` uses the mask's darkness as alpha.
  - Each `draw01.setCell*` adds a cell (the `*Rec` forms re-set cell `id`), and cell k moves
    picture k. Script coordinates are 740-wide event units, scaled ×1024/740 (`FUN_00432dd0`;
    `setCellx1894` uses ×1024/1894).
  - Positions are in tenths of a pixel. A loop starts each cell at x,y; every step adds dx,dy.
    Modes: 1 moves `count` steps, 2 blinks, 3 shows a frame window, 4 wraps, 5 eases over 28 steps
    (table 0x4f1188).
  - Step timing (`FUN_00426020`): every frame an accumulator gains `Timer`/14, and each time it
    reaches 1 the cells step. So a step lasts 14/n frames (196/n ms, 39 ms at Timer 5). The step
    after the last counted one ends the loop, and a `%timer.loopcut.wait` target zeroes the count
    (one-shot, `FUN_00456b30`).
  - Each step tick draws every cell into the sprite target *then* steps it (`FUN_00426020` →
    `FUN_00436810`, `FUN_00435e10`), and the ending tick draws nothing. So the screen trails by one
    step: a loop of N steps shows N − 1 moves, and cells switched on between loops appear at the
    next loop's first draw (or `DrawNow`). Asuka's slide-in at ava021b ends flush with the CG bottom
    only this way.
  - `loadPAINT2 name,n` / `loadCOPY2`: n (digits 1–4) only sets texture-edge insets of 0.005 UV
    (`FUN_0043d940`), not a position. eve1024 pictures are drawn at native size × 1.0, event ones
    × 1024/740 (`FUN_00432d10`).
  - `draw01.loop`/`ContinueLoop` block until the loop ends; `ThreadLoop` doesn't block. Pictures
    show from the first loop until `DeleteDDB`/`delete`, clipped to the CG area.
  - Checked: the Ava011 jets over the moon, and Asuka sliding in at ava021b.
- **`%selSpeed`** (kt_cmd_selspeed 0x4258a0, player 0x4358a0, stepped per frame by `FUN_00435500`
  from the frame update 0x442ba0):
  - `Create` registers pictures. `Scroll.Set w,h,dx,dy,x0,y0` fills a parameter block that
    `Scroll [n]` starts: a pan of the view every `Scroll.Timer` frames, stopping at the picture edge
    or after n steps. `SetE100mode` eases it along a quarter sine.
  - `Bure` shakes `Bure.Number` by the `Bure.Set` amplitude every `Bure.Timer` frames, decaying over
    46 beats in mode 0. `Draw n` shows a still, and `Loop n` cycles pictures.
- **`%film`** (kt_cmd_film 0x424d10): clips advance `rate`/14 frames per frame. `%film.new` resets
  the rate to 4 (`FUN_00405630`), and `%film.Timer`/`SpeedThreadTimer` set it.
  - `SpeedPlay` and `SpeedPlayLoop n` hold the script (`FUN_00419240` → `FUN_00424b60`) until the
    first clip has played through, n passes are done, or a loopcut fires (`FUN_004053c0`).
  - `SpeedThreadPicON`/`SpeedThreadPictuer` don't wait; the clips play once and hold the last frame
    (Ava011's c007a in the VM).
  - `SpeedThreadPicRepartON [still]` starts the loaded film looping without waiting, over `still`
    as the background: the NERV HQ walkways (GFRONT7A over GFRONT7B, the escalator strip gfront06)
    and the V-1 elevator's floor indicator (gfront11, after `SpeedThreadPictuer`), until
    `%film.delete` / `SpeedThreadPicOFF`. Read from the scripts, not traced in Ghidra.
- **Accepted but not done:** `cover.*`, `bg*.Set*` transforms (user matrix, rotation, resize,
  scroll, picture anime mode), `bg03.*`, `select02.*` styling, `%HoldScreen`.

### Screen layout (matched to the VM recording within 1–2 px)

- No message window and no speaker name. The `textbmp` line (light text on black, so luminance
  becomes alpha) is drawn ×1.30, centred, with its top at y 635. A translated line is drawn with the
  font instead (28 px, wrapped at 960, centred).
- Faces are 160×180 portraits (`FACE_SCALE=3`) at the formation x with their top at y 432. The
  formation is centred on x 496 in the original; we shift it 16 px right (`FACE_SHIFT_X`) so the
  centre face sits over the screen-centred text. The original frames them with `frmface1`
  (frame.lb5) drawn 188×199 from (x−18, y−9); we draw a plain border instead.

### Audio

- One SDL device opened at start, with a persistent stream per channel. A new sound clears and
  refills its stream. Opening a device per sound delayed every clip and cut lines like Asuka's
  "Vielen Dank" (`009a`, which `009b` replaces at 1.6 s).
- **Channels follow the original:** `%sound.play`/`playrepeat`/`stop` share player 1, so a new
  sound replaces the last; `voice.*`, `se1.*`, `se2.*` and music/`%midi` each have their own. The
  player (0x458a40 → 0x459da0) stops and restarts its buffer for each new file.
- Sounds become audible 0.2–0.4 s after `%sound.play` in the VM while the script runs on (async
  start); we start them at once.
- A channel's gain is the script's (`music.volume`) × (master × group)² from the settings. A
  sound's group comes from its archive (voice or se), since `%sound.play` carries most voiced lines.
- `FT_AUDIO_TRACE=1` logs sound starts, replacements (with how much of the old clip was still
  queued) and line times against the script clock.

## Our additions

The menus and panels are our own designs, drawn with `game/ui.c` (plain boxes on a dark veil, no
animation). Their UI text is `ft_ui_strings` (`i18n.c`), which `ftconv i18n-template` lists under
`## engine`.

- **Title menu** (`titlemenu.c`): starmenu.txt's `select02` / `selectCancel` menus (the main menu
  and its quit prompt) as a column of boxes below the logo. 続きからはじめる is greyed out until a
  save exists, and アルバム is left out until the album is done (`testalbum2` would hang on its
  unhandled UI); `title_map` turns a shown entry back into the script's number.
- **Title art:** the original places the title's bg02 layers in 3D (`SetUserMatrix`, a slow
  rotation, `SetResize`, a zoom-in), which isn't done. Drawn centred, everything lines up except
  the 特別編 stamp (BG02tl02), so `draw_page` gives it a fixed spot under the logo's right end.
- **In-game menus** (`choicemenu.c`): the `%cmnd` command menu and story choices (`%select.wait`,
  `select`) as a centred column of boxes over the CG. Look / talk / think entries show just the
  action (`下駄箱・見る` → 見る). Up/Down, 1–9, Enter or a click on an entry picks; a click elsewhere
  does nothing.
- **Save / load** (`savepanel.c`; files in `save.c`, ftcore):
  - `saveload.txt`'s `SaveLoadMain` opens it: `savepanel.open c1` (0 from a scene's
    `%cmnd.system.call システム`, save + load; 1 from the title's 続きから, load only), then
    `savepanel.select` waits for it. The panel saves, deletes and loads by itself, so the script only
    ever gets `number` 999 (back); its own confirmation prompts and `saveload.*` calls go unused.
  - Saves are taken at save points, so the state is the call stack minus saveload's frames
    (the scene's return address after `%cmnd.wait`), every variable, and what's on screen and
    playing (bg, event, bg02 pictures, faces, music, looping sounds). Loading resumes there and
    re-runs the menu block. Save points: a command menu (the game's own), the start of a story
    script (`Ava*`/`game*`/`ja*` entered at line 0; the Ava chapters have no command menus) and
    a load. The runtime keeps the last one as `rt->checkpoint` (save, variable copy, thumbnail);
    saving anywhere else writes that, with the notice 前のセーブポイントでセーブしました. Going
    back to the title (`start` / `starmenu`) forgets it.
  - Slots are `NN.sav` (UTF-8 text, format in `save.h`) + `NN.png` (screenshot), 48 of them (6 pages
    of 8), in `SDL_GetPrefPath("fantatech", "kotetsu")/save/` or `FT_SAVE_DIR`. A save is named after
    the nearest `☆` save point above it (`☆下駄箱`), else the nearest `○`.
- **Settings** (`settingspanel.c`; file in `config.c`, ftcore): two columns: languages
  (every `i18n/*.txt` by its `@name:`, as a radio list, `jp.txt` first: an empty table, the original
  Japanese; Enter or a click picks, moving the cursor doesn't) and four volume sliders (master,
  music, voice, sound effects) in 5% steps. Changes apply at once. It replaces the original's `system01` (resolution and texture sizes):
  entering that script opens ours, and closing goes back with `load starmenu`. During play, the save
  panel's 設定 button and the Esc menu open it. Stored in `settings.txt` beside the saves
  (`FT_CONFIG` overrides); `--lang` / `FT_LANG` override the language for one run.
- **Esc menu** (`pause_*` in runtime.c, drawn with `titlemenu.c`): セーブ, ロード, 設定, 不具合を報告,
  終了する (asks first), ゲームに戻る. It pauses the game: `game_ms()` stands still (every timer, fade, hold
  and the script clock run on it) and the audio device stops, so a voiced scene carries on exactly
  where it was. Save is on whenever a game is going: at a command menu it saves there (resuming
  after its `%cmnd.wait`, like the system verb's), elsewhere the last save point (see above);
  loading and settings work anywhere, the title screen included. The panels' back returns to it.
  Esc, Backspace or a right-click backs out, and the quit prompt starts on メニューに戻る.
- **Held keys** (`ft_runtime_event`): only cursor keys auto-repeat. A held Enter, Space or Esc acts
  once, or its repeats would pick through whatever menu comes up next (the quit prompt, a story
  choice).
- **Intro movie** (`game/movie.c`): start.txt's `movieplay.load` / `movieplay.wait` play
  `movie0508_cinepak.avi` centred on black at the screen width (×1024/740, like event art; where the
  original draws it isn't traced). Video follows `game_ms()`, the audio is decoded whole at load and
  queued on its own stream at `movieplay.volume` × the music volume. Any key or click skips it;
  autoplay skips it too. `ftconv movie mana01 --raw` matches ffmpeg byte for byte on every frame and
  sample (`ffmpeg -i … -f rawvideo -pix_fmt rgb24` / `-f s16le`).
- **Fullscreen** (our addition): `display.fullscreen` in settings.txt, on by default. Borderless
  desktop fullscreen (`SDL_SetWindowFullscreen`), letterboxed to 4:3. `main.c` creates the window
  hidden and shows it after `ft_runtime_apply_display`, so it doesn't flash windowed; `--shot` runs
  stay a 1024×768 window. F11 toggles it from anywhere and saves at once; the settings panel has it
  under 画面.
- **Screenshots:** S, F2 or Print Screen draws the frame off screen at 1024×768
  (`take_screenshot`) and saves `screenshots/fantatech_<date>_<time>.png` beside `save/`
  (`FT_SHOT_DIR` overrides).
- **Bug reports** (the Esc menu's 不具合を報告, `write_report`): writes
  `reports/fantatech_report_<date>_<time>.txt` beside `save/` (`FT_REPORT_DIR` overrides): version,
  platform, renderer, font (`ft_font_describe`), settings, then the dev overlay's state report (`dbg_report`: script, line,
  label, waits, layers, audio, the last 12 lines), and a `.png` of the scene with the game's own
  menus but not the Esc menu. The screen after it says where they went and offers GitHubで報告する
  (opens a new, empty issue) and フォルダを開く.
- **Translations** (`i18n/<lang>.txt`; format in `i18n/README.md` and `src/game/i18n.h`): keys are
  the dialogue `$id`, or an FNV-1a hash of the Japanese for menus. `ftconv i18n-template mana01 -o
  i18n\xx.txt [--from old] [--bare]` writes every key; `--bare` drops the Japanese comments for
  tables committed to the repo.
- **Developer tools** are off unless the exe gets `--dev` (or `FT_DEV=1`):
  `ft_runtime_set_dev` enables the dev overlay and Ctrl fast-forward.
- **Dev overlay** (` key, `--dev` only): the info page shows the script, line, label, clock and what it's
  waiting for, every layer by asset code, faces, film/sprite/selSpeed state, audio, unhandled
  commands and the last 12 lines run. `C` copies that as a bug report to the clipboard, `R` replays
  from the label above the current line, and `S` opens a scene picker (all scripts and their labels;
  Enter jumps there with a cleared scene). `FT_DEV_UI=1`/`2` opens the info page or picker at
  start.
- **`--label NAME`** starts the given script at a label (without its glyph, e.g.
  `ava021b --label 同・屋上`), like the scene picker.

Controls: Space, Enter or a click advances, Up/Down picks, Esc opens the menu, F11 toggles
fullscreen, S or F2 takes a screenshot. With `--dev`: Ctrl fast-forwards and ` opens the dev overlay.
