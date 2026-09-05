format_bytes() {
  local bytes="$1"
  if (( bytes >= 1048576 )); then
    printf '%d.%d MiB' "$((bytes / 1048576))" "$(((bytes % 1048576) * 10 / 1048576))"
  elif (( bytes >= 1024 )); then
    printf '%d.%d KiB' "$((bytes / 1024))" "$(((bytes % 1024) * 10 / 1024))"
  else
    printf '%d B' "$bytes"
  fi
}

render_download_progress() {
  local message="$1"
  local downloaded="$2"
  local total="$3"
  local index="$4"
  local width=24
  local filled=0
  local percent=0
  local bar=""
  local text
  local i

  if (( total > 0 )); then
    percent=$((downloaded * 100 / total))
    if (( percent > 100 )); then
      percent=100
    fi
    filled=$((percent * width / 100))
    for ((i = 0; i < width; i++)); do
      if (( i < filled )); then
        bar+="#"
      else
        bar+="-"
      fi
    done
    text="${percent}% ($(format_bytes "$downloaded") / $(format_bytes "$total"))"
  else
    local offset=$((index % width))
    for ((i = 0; i < width; i++)); do
      if (( (i - offset + width) % width < 6 )); then
        bar+="#"
      else
        bar+="-"
      fi
    done
    text="$(format_bytes "$downloaded") downloaded"
  fi

  printf '\r\033[36m%s\033[0m %s [\033[32m%s\033[0m] %s\033[K' \
    '::' "$message" "$bar" "$text"
}

run_download() {
  local message="$1"
  local output="$2"
  local uri="$3"

  if [[ ! -t 1 ]]; then
    status "$message"
    curl --fail --location --silent --show-error --output "$output" "$uri"
    return $?
  fi

  local headers="${output}.headers.$$"
  local errors="${output}.errors.$$"
  local pid status downloaded total candidate index=0
  : >"$headers" || return 1
  : >"$errors" || return 1
  : >"$output" || return 1

  curl --fail --location --silent --show-error \
    --dump-header "$headers" --output "$output" "$uri" 2>"$errors" &
  pid=$!
  total=0

  while kill -0 "$pid" 2>/dev/null; do
    downloaded="$(wc -c <"$output")"
    downloaded=$((downloaded + 0))
    candidate="$(awk '
      /^HTTP\// { response_length = "" }
      tolower($1) == "content-length:" {
        gsub(/\r/, "", $2)
        response_length = $2
      }
      END { print response_length }
    ' "$headers")"
    if [[ "$candidate" =~ ^[0-9]+$ ]]; then
      total="$candidate"
    else
      total=0
    fi
    render_download_progress "$message" "$downloaded" "$total" "$index"
    index=$(((index + 1) % 24))
    sleep 0.1
  done

  if wait "$pid"; then
    status=0
  else
    status=$?
  fi

  downloaded="$(wc -c <"$output")"
  downloaded=$((downloaded + 0))
  if (( status == 0 && total == 0 )); then
    total="$downloaded"
  fi
  render_download_progress "$message" "$downloaded" "$total" "$index"
  printf '\n'

  if (( status != 0 )); then
    cat "$errors" >&2
  fi
  rm -f "$headers" "$errors"
  return "$status"
}
