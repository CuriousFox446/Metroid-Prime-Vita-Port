#!/usr/bin/env bash
# Names the [drain] lines of a Vita log: where the game waited for Aurora's FIFO thread.
# Their offsets are from aurora_take_drain_sites, so the ELF of the same build places them.
#   tools/vita_drain_sites.sh metroid_prime_port.log [build/vita/metroid_prime_port]
set -euo pipefail
log=${1:?usage: $0 <log> [elf]}
elf=${2:-build/vita/metroid_prime_port}
tools=${VITASDK:-$HOME/vitasdk}/bin
anchor=$(("0x$("$tools/arm-vita-eabi-nm" "$elf" | awk '$3 == "aurora_take_drain_sites" {print $1}')"))

where() {
  # A return address: the call is the instruction before it (Thumb, so 2 bytes back is inside it).
  local offset=$1
  [ "$offset" = "+0" ] && { echo "-"; return; }
  printf '0x%x' $((anchor + offset - 2)) | xargs "$tools/arm-vita-eabi-addr2line" -f -C -s -e "$elf" | paste -sd' '
}

grep '^\[drain\]' "$log" | sed -E 's/.*caller ([-+][0-9]+) origin ([-+][0-9]+)/\1 \2/' | sort | uniq -c | sort -rn | head -20 |
  while read -r seconds caller origin; do
    echo "$seconds seconds: $(where "$caller")  <-  $(where "$origin")"
    grep '^\[drain\]' "$log" | grep -- "caller $caller origin $origin" | tail -1 | cut -c1-60
  done
