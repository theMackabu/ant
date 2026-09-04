die() {
  printf '\033[1;3;97;41m%s\033[0m\n' "$1" >&2
  exit 1
}

dim() {
  printf '\033[2m%s\033[0m\n' "$1"
}

line() {
  printf '\033[2m%s\033[0m\n' '------------------------------------------------------------'
}

status() {
  printf '\033[36m%s\033[0m %s\n' '::' "$1"
}

detail() {
  printf '   \033[2m%s\033[0m\n' "$1"
}

success() {
  printf '\033[32m%s\033[0m %s\n' 'ok' "$1"
}

run_step() {
  local message="$1"
  shift
  local status_code

  if [[ ! -t 1 ]]; then
    status "$message"
    "$@"
    return $?
  fi

  printf '\033[36m%s\033[0m %s' '::' "$message"
  "$@"
  status_code=$?
  if [[ "$status_code" -eq 0 ]]; then
    printf '\r\033[K'
  else
    printf '\r\033[31m%s\033[0m %s\033[K\n' '!!' "$message"
  fi
  return "$status_code"
}
