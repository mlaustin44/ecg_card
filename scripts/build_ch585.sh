#!/bin/sh
# Command-line build of the CH585 radio firmware (CH585F/).
#
# MounRiver Studio is the build of record. This script only mirrors what
# MounRiver 2.x generates from CH585F/.cproject, so the firmware can also be
# compile- and link-checked on a machine with the WCH toolchain but no IDE.
# It reads the defines, include paths, library paths, libraries, linker
# script and source folders (with their exclusions) from CH585F/.cproject;
# the target and optimisation flags below are the ones MounRiver derives
# from that file's toolchain options (GCC12 = riscv-wch-elf-gcc; rv32i + M +
# C + B + XW, ilp32, code model "any" = medany, small data 8, no
# save-restore, -Os, signed char, function/data sections, no-common,
# highcode sections, -g; gnu99; nano + nosys specs, -nostartfiles,
# --gc-sections). If you change the project in MounRiver, keep this in step.
#
# On top of the exact MounRiver build it compiles APP/ and Profile/ a second
# time with -Wall -Wextra -Werror (objects discarded), so warnings in our own
# sources fail the script.
#
#   scripts/build_ch585.sh            build into build/ch585/
#   WCH_TOOLCHAIN=/path scripts/build_ch585.sh
#
# Output: build/ch585/CH585F.elf, .hex, .map (build*/ is git-ignored).
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROJ="$ROOT/CH585F"
OUT="$ROOT/build/ch585"
TC="${WCH_TOOLCHAIN:-/opt/wch/Toolchain}"
BIN="$TC/RISC-V Embedded GCC12/bin"
CC="$BIN/riscv-wch-elf-gcc"
OBJCOPY="$BIN/riscv-wch-elf-objcopy"
SIZE="$BIN/riscv-wch-elf-size"

[ -x "$CC" ] || { echo "error: $CC not found (set WCH_TOOLCHAIN)" >&2; exit 1; }

TARGET="-march=rv32imc_zba_zbb_zbc_zbs_xw -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20"
OPT="-Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common --param=highcode-gen-section-name=1 -g"

# Settings from .cproject, one "kind value" per line.
SETTINGS=$(python3 -I - "$PROJ/.cproject" <<'EOF'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
def proj(v):
    v = v.replace('&quot;', '').strip('"')
    return v.replace('${workspace_loc:/${ProjName}}', '.').replace('${workspace_loc:/${ProjName}/', '').rstrip('}')
want = {
    'c.compiler.include.paths': 'inc', 'c.compiler.defs': 'def',
    'c.linker.paths': 'libpath', 'c.linker.libs': 'lib', 'c.linker.scriptfile': 'ld',
    'c.linker.flags': 'ldflag',
}
for opt in root.iter('option'):
    sc = opt.get('superClass', '')
    for key, kind in want.items():
        if sc == 'ilg.gnumcueclipse.managedbuild.cross.riscv.option.' + key:
            for v in opt.iter('listOptionValue'):
                print(kind, proj(v.get('value')))
for e in root.iter('entry'):
    if e.get('kind') == 'sourcePath':
        print('src', e.get('name') or '.', e.get('excluding') or '')
EOF
)

get() { printf '%s\n' "$SETTINGS" | awk -v k="$1" '$1 == k { $1 = ""; sub(/^ /, ""); print }'; }

INCS=""; for d in $(get inc); do INCS="$INCS -I$PROJ/$d"; done
DEFS=""; for d in $(get def); do DEFS="$DEFS -D$d"; done
LIBPATHS=""; for d in $(get libpath); do
    case "$d" in ../) d=. ;; esac
    LIBPATHS="$LIBPATHS -L$PROJ/$d"; done
LIBS=""; for l in $(get lib); do LIBS="$LIBS -l$l"; done
LDSCRIPT="$PROJ/$(get ld)"
LDFLAGS=""; for f in $(get ldflag); do LDFLAGS="$LDFLAGS -Xlinker $f"; done

rm -rf "$OUT"
mkdir -p "$OUT/obj" "$OUT/strict"

echo "compiler: $("$CC" --version | head -1)"
echo "defines: $DEFS"

OBJS=""
get src | while read -r dir excl; do
    find "$PROJ/$dir" -maxdepth 1 -type f \( -name '*.c' -o -name '*.S' \) | sort | while read -r f; do
        base=$(basename "$f")
        if [ -n "$excl" ] && printf '%s\n' "$excl" | tr '|' '\n' | grep -qx "$base"; then
            echo "  skip $dir/$base (excluded in .cproject)"
            continue
        fi
        o="$OUT/obj/$dir/${base%.*}.o"
        mkdir -p "$(dirname "$o")"
        case "$f" in
        *.S) "$CC" $TARGET $OPT -x assembler-with-cpp -c -o "$o" "$f" ;;
        *.c) "$CC" $TARGET $OPT $INCS $DEFS -std=gnu99 -c -o "$o" "$f" ;;
        esac
        echo "$o" >> "$OUT/objs.txt"
        case "$dir" in
        APP|Profile)
            "$CC" $TARGET $OPT $INCS $DEFS -std=gnu99 -Wall -Wextra -Werror \
                -c -o "$OUT/strict/${base%.*}.o" "$f" ;;
        esac
    done
done

OBJS=$(cat "$OUT/objs.txt")
cd "$OUT"
# shellcheck disable=SC2086
"$CC" $TARGET $OPT -T "$LDSCRIPT" -nostartfiles -Xlinker --gc-sections $LIBPATHS $LDFLAGS \
    -Wl,-Map,CH585F.map --specs=nano.specs --specs=nosys.specs -o CH585F.elf $OBJS $LIBS
"$OBJCOPY" -O ihex CH585F.elf CH585F.hex
"$SIZE" --format=berkeley CH585F.elf
echo "built $OUT/CH585F.hex"
