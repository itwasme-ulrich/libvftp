#!/bin/sh
# Copies libvftp into the projects that build it -- one source of truth, no
# hand-synced headers (the old vftp_types.h / gs/vftp/types.h pair is gone).
#
#   tools/vendor.sh          copy include/ src/ port/ tests/ into every destination
#   tools/vendor.sh --check  exit 1 if any destination differs from this tree
#
# Destinations keep their own build glue (CMake lines, wscript) outside the
# copied directories.
set -eu
LIB=$(cd "$(dirname "$0")/.." && pwd)
TOP=$(cd "$LIB/.." && pwd)
DESTS="$TOP/FSW_devinit/src/middleware/vftp/libvftp $TOP/v-client/lib/libvftp $TOP/slt_exp_fsw-1.1.5/lib/libvftp"
DIRS="include src port tests"
rc=0
for d in $DESTS; do
    for sub in $DIRS; do
        if [ "${1:-}" = "--check" ]; then
            if ! diff -r -q "$LIB/$sub" "$d/$sub" >/dev/null 2>&1; then
                echo "OUT OF SYNC: $d/$sub"; rc=1
            fi
        else
            mkdir -p "$d"
            rm -rf "${d:?}/$sub"
            cp -r "$LIB/$sub" "$d/$sub"
        fi
    done
    [ "${1:-}" = "--check" ] || echo "vendored -> $d"
done
[ "${1:-}" = "--check" ] && [ $rc -eq 0 ] && echo "all vendored copies identical"
exit $rc
