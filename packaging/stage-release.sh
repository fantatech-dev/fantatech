#!/bin/sh
# Stages a release folder: the programs, translations, our license and the licenses of every
# library linked into the binary. CI runs it on Windows (Git Bash) and Linux before archiving.
#
#   packaging/stage-release.sh <out dir> <dir with the built programs> [.exe]
set -eu
out=$1
bin=$2
ext=${3:-}
root=$(cd "$(dirname "$0")/.." && pwd)
ft=$root/thirdparty/SDL_ttf/external/freetype

rm -rf "$out"
mkdir -p "$out/licenses"
cp "$bin/fantatech$ext" "$bin/ftconv$ext" "$out/"
cp -R "$root/i18n" "$out/"
cp "$root/README.md" "$root/LICENSE" "$root/packaging/THIRD-PARTY.txt" "$out/"
cp "$root/thirdparty/SDL/LICENSE.txt" "$out/licenses/SDL3.txt"
cp "$root/thirdparty/SDL_ttf/LICENSE.txt" "$out/licenses/SDL3_ttf.txt"
cp "$ft/LICENSE.TXT" "$out/licenses/FreeType.txt"
cp "$ft/docs/FTL.TXT" "$out/licenses/FreeType-FTL.txt"
cp "$ft/src/bdf/README" "$out/licenses/FreeType-BDF.txt" # the BDF and PCF drivers' X-style notices
cp "$ft/src/pcf/README" "$out/licenses/FreeType-PCF.txt"
cp "$root/thirdparty/SDL_ttf/external/harfbuzz/COPYING" "$out/licenses/HarfBuzz.txt"
echo "staged $out"
