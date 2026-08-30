#!/usr/bin/env bash
set -euo pipefail

MINIMUM_SECONDS=${ANT_BENCH_COOLDOWN_SECONDS:-300}
MAXIMUM_SECONDS=${ANT_BENCH_COOLDOWN_MAX_SECONDS:-900}
LOAD_LIMIT=${ANT_BENCH_LOAD_LIMIT:-1.5}
NOT_BEFORE_LOCAL=${ANT_BENCH_NOT_BEFORE_LOCAL:-}
STARTED=$(date +%s)
NOT_BEFORE_EPOCH=0

if [[ -n "$NOT_BEFORE_LOCAL" ]]; then
  [[ "$NOT_BEFORE_LOCAL" =~ ^([01][0-9]|2[0-3]):[0-5][0-9]$ ]] || {
    echo "ANT_BENCH_NOT_BEFORE_LOCAL must use HH:MM" >&2
    exit 1
  }
  pacific_date=$(TZ=America/Los_Angeles date +%Y-%m-%d)
  NOT_BEFORE_EPOCH=$(TZ=America/Los_Angeles date -j -f '%Y-%m-%d %H:%M:%S' \
    "$pacific_date $NOT_BEFORE_LOCAL:00" +%s)
fi

echo "cooling down for at least ${MINIMUM_SECONDS}s before measurement"
if (( NOT_BEFORE_EPOCH > STARTED )); then
  echo "measurement will not start before ${NOT_BEFORE_LOCAL} America/Los_Angeles"
fi
while true; do
  now=$(date +%s)
  elapsed=$((now - STARTED))
  load=$(sysctl -n vm.loadavg | awk '{gsub(/[{}]/, ""); print $2}')

  if (( elapsed >= MINIMUM_SECONDS && now >= NOT_BEFORE_EPOCH )) \
      && awk -v load="$load" -v limit="$LOAD_LIMIT" 'BEGIN { exit !(load <= limit) }'; then
    echo "cooldown complete after ${elapsed}s (one-minute load ${load})"
    break
  fi
  if (( elapsed >= MAXIMUM_SECONDS && now >= NOT_BEFORE_EPOCH )); then
    echo "cooldown limit reached after ${elapsed}s (one-minute load ${load})"
    break
  fi
  sleep 15
done

pmset -g therm || true
