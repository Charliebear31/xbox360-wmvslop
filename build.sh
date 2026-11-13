#!/bin/bash
# Builds WMVSlop.xex (run through tools/build.ps1, in the bundled MSYS2).
# Toolchain: xsdk's GCC (/opt/xsdk-gcc), its import generator and its .xex packer (buildtools/xsdk-src/tools).
# The plugin links no library of xsdk's: only its kernel.h declarations are used.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
XSDK="${XSDK:-$ROOT/buildtools/xsdk-src}"
TC="${TC:-/opt/xsdk-gcc/bin/powerpc-xsdk-elf-}"
PYTHON="${PYTHON:-python3}"
OUT="$HERE/build"
# The address the plugin loads at. System DLLs sit below the dashboard's 0x92000000; it must not collide with
# another plugin (a collision only makes DashLaunch fail to load this one).
BASE="${BASE:-0x91E40000}"

mkdir -p "$OUT/imports"
"$PYTHON" -I "$XSDK/tools/gen_imports.py" "$HERE/imports.txt" "$OUT/imports"

CFLAGS="-mcpu=cell -mno-altivec -fno-pic -std=gnu17 -O2 -ffreestanding -fno-tree-vectorize -fno-stack-protector \
	-Wall -Wextra -Wno-missing-field-initializers -I$XSDK/include -I$HERE/src"
OBJECTS=""
for source in "$HERE"/src/*.c; do
	object="$OUT/$(basename "${source%.c}").o"
	"${TC}gcc" $CFLAGS -c "$source" -o "$object"
	OBJECTS="$OBJECTS $object"
done
for source in "$HERE"/src/*.S "$OUT"/imports/*.S; do
	object="$OUT/$(basename "${source%.S}").o"
	"${TC}gcc" -mcpu=cell -Wa,-mregnames -c "$source" -o "$object"
	OBJECTS="$OBJECTS $object"
done

"${TC}gcc" -static -nostdlib -Wl,--defsym=BASE="$BASE" -T "$HERE/plugin.ld" -Wl,-Map,"$OUT/WMVSlop.map" \
	-o "$OUT/WMVSlop.elf" $OBJECTS -lgcc
"$PYTHON" -I "$HERE/tools/mkxex.py" "$XSDK/tools" "$BASE" "$OUT/WMVSlop.elf" "$OUT/WMVSlop.xex"
"${TC}objdump" -h "$OUT/WMVSlop.elf" | grep -E '^ +[0-9]+ \.'
ls -l "$OUT/WMVSlop.xex"
