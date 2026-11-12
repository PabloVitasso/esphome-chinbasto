#!/bin/sh
# Builds and runs the glue tests natively. Read README.md.
# Usage: tests/glue/run.sh [extra compiler flags]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${TMPDIR:-/tmp}/chinbasto-glue-test
log=$out.log
CXX=${CXX:-g++}

"$CXX" -std=gnu++17 -g -O1 \
  -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -I "$here/fake" -I "$root/components/diesel_heater" -I "$root/tests/host" \
  "$@" \
  "$here/glue_test.cpp" "$root/components/diesel_heater/heater_uart.cpp" \
  -o "$out"

# GCC 12 AddressSanitizer can crash at startup on a kernel with high
# mmap randomization. That crash comes before main(), so the log holds
# no test line. Retry only that case. Any other failure is real.
attempt=1
while :; do
  status=0
  "$out" >"$log" 2>&1 || status=$?
  if [ "$status" -lt 128 ]; then
    break
  fi
  if grep -Eq '^(PASS|FAIL)' "$log"; then
    break
  fi
  if [ "$attempt" -ge 5 ]; then
    break
  fi
  echo "asan startup crash, retry $attempt" >&2
  attempt=$((attempt + 1))
done
cat "$log"
exit "$status"
