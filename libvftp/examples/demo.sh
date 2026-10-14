#!/bin/sh
# Two real processes, CCSDS Space Packets over UDP. Server and client each drop
# 10% of what they send and of what they receive: ~19% loss per direction,
# ~34% of request/reply round trips fail.
set -u
B=$(dirname "$0")/../build
SRV=$(mktemp -d) ; CLI=$(mktemp -d)
PORT=${PORT:-5199}
head -c 300000 /dev/urandom > "$CLI/big.bin"
head -c 20000 /dev/urandom > "$CLI/small.bin"
"$B/vftp_spp_server" -p $PORT -r "$SRV" -l 100 -i 3000 > "$SRV/server.log" 2>&1 &
SPID=$!
sleep 0.3
C="$B/vftp_spp_client -p $PORT -l 100 -t 300 -g 100 -r 8"
fail=0
run() { echo "\$ $*"; "$@" | grep -v "^  round" ; [ "$?" -eq 0 ] || true; }
check() { if cmp -s "$1" "$2"; then echo "    -> identical: $(basename "$2")"; else echo "    -> DIFFERENT: $2"; fail=1; fi; }
run $C burst upload "$CLI/big.bin" big.bin;          check "$CLI/big.bin" "$SRV/big.bin"
run $C burst download big.bin "$CLI/big.back";       check "$CLI/big.bin" "$CLI/big.back"
run $C poll upload "$CLI/small.bin" small.bin;       check "$CLI/small.bin" "$SRV/small.bin"
run $C poll download small.bin "$CLI/small.back";    check "$CLI/small.bin" "$CLI/small.back"
run $C burst ls ""
run $C burst regen ""
run $C burst download LIST.TXT "$CLI/list.txt";     cat "$CLI/list.txt"
run $C burst rm small.bin
[ -e "$SRV/small.bin" ] && { echo "    -> rm failed"; fail=1; } || echo "    -> removed"
kill $SPID; wait $SPID 2>/dev/null
echo "--- server log ---"; cat "$SRV/server.log"
rm -rf "$SRV" "$CLI"
[ $fail -eq 0 ] && echo "DEMO PASS" || echo "DEMO FAIL"
exit $fail
