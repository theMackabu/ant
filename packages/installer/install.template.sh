#!/usr/bin/env bash
# Ant auto-installer ({{gitHash}})
# Generated from {{configPath}}

set -euo pipefail

app_name="{{name}}"
binary_name="{{binary}}"
install_env="{{installEnv}}"
default_install_dir="{{defaultInstallDir}}"
download_url="{{downloadUrl}}"

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

run_with_loader() {
  local message="$1"
  shift
  local width=24
  local index=0
  local phase=0
  local pid status
  local notes=(
    'resolving platform artifact'
    'waiting for manifest cache'
    'streaming ant binary'
    'writing executable'
  )

  if [[ ! -t 1 ]]; then
    status "$message"
    "$@"
    return $?
  fi

  "$@" &
  pid=$!

  while kill -0 "$pid" 2>/dev/null; do
    local offset=$((index % width))
    local bar=""
    local i
    for ((i = 0; i < width; i++)); do
      if (( (i - offset + width) % width < 6 )); then
        bar+="#"
      else
        bar+="-"
      fi
    done
    printf '\r\033[36m%s\033[0m %s [\033[32m%s\033[0m] %s\033[K' \
      '::' "$message" "$bar" "${notes[phase]}"
    index=$(((index + 1) % width))
    if (( index % 6 == 0 )); then
      phase=$(((phase + 1) % ${#notes[@]}))
    fi
    sleep 0.1
  done

  wait "$pid"
  status=$?
  if [[ "$status" -eq 0 ]]; then
    printf '\r\033[K'
  else
    printf '\r\033[31m%s\033[0m %s\033[K\n' '!!' "$message"
  fi
  return "$status"
}

bold() {
  printf '\033[1m%s\033[0m' "$1"
}

has_cmd() {
  command -v "$1" >/dev/null 2>&1
}

tildify() {
  local path="$1"
  if [[ -n "${HOME:-}" && "$path" == "$HOME"* ]]; then
    printf '~%s' "${path#"$HOME"}"
  else
    printf '%s' "$path"
  fi
}

append_or_print() {
  local path="$1"
  local tilde_path="$2"
  local tilde_bin_dir="$3"
  shift 3

  if [[ -f "$path" ]]; then
    {
      printf '\n# %s\n' "$app_name"
      for command in "$@"; do
        printf '%s\n' "$command"
      done
    } >>"$path" || return 1
    success "Added \"$tilde_bin_dir\" to \$PATH in \"$tilde_path\""
    return 0
  fi

  return 1
}

print_manual_path() {
  local config_hint="$1"
  shift

  printf 'Manually add the directory to %s (or similar):\n' "$config_hint"
  for command in "$@"; do
    printf '  '
    bold "$command"
    printf '\n'
  done
}

configure_shell() {
  local shell_name="$1"
  local quoted_install_dir="$2"
  local bin_env="$3"
  local tilde_bin_dir="$4"
  local export_install export_path

  if [[ -z "${HOME:-}" ]]; then
    return 0
  fi

  if [[ "$shell_name" == "fish" ]]; then
    export_install="set --export $install_env $quoted_install_dir"
    export_path="set --export PATH $bin_env \$PATH"
  else
    export_install="export $install_env=$quoted_install_dir"
    export_path="export PATH=\"$bin_env:\$PATH\""
  fi

  case "$shell_name" in
    fish)
      local fish_config="$HOME/.config/fish/config.fish"
      local tilde_fish_config
      tilde_fish_config="$(tildify "$fish_config")"
      append_or_print "$fish_config" "$tilde_fish_config" "$tilde_bin_dir" "$export_install" "$export_path" ||
        print_manual_path "$tilde_fish_config" "$export_install" "$export_path"
      if [[ -f "$fish_config" ]]; then
        configured_refresh_command="source $tilde_fish_config"
      fi
      ;;
    zsh)
      local zsh_config="$HOME/.zshrc"
      local tilde_zsh_config
      tilde_zsh_config="$(tildify "$zsh_config")"
      append_or_print "$zsh_config" "$tilde_zsh_config" "$tilde_bin_dir" "$export_install" "$export_path" ||
        print_manual_path "$tilde_zsh_config" "$export_install" "$export_path"
      if [[ -f "$zsh_config" ]]; then
        configured_refresh_command="source $tilde_zsh_config"
      fi
      ;;
    bash)
      local bash_configs=("$HOME/.bash_profile" "$HOME/.bashrc")
      local bash_config tilde_bash_config

      if [[ -n "${XDG_CONFIG_HOME:-}" ]]; then
        bash_configs+=(
          "$XDG_CONFIG_HOME/.bash_profile"
          "$XDG_CONFIG_HOME/.bashrc"
          "$XDG_CONFIG_HOME/bash_profile"
          "$XDG_CONFIG_HOME/bashrc"
        )
      fi

      for bash_config in "${bash_configs[@]}"; do
        tilde_bash_config="$(tildify "$bash_config")"
        if append_or_print "$bash_config" "$tilde_bash_config" "$tilde_bin_dir" "$export_install" "$export_path"; then
          configured_refresh_command="source $tilde_bash_config"
          return 0
        fi
      done

      print_manual_path '~/.bashrc' "$export_install" "$export_path"
      ;;
    *)
      print_manual_path '~/.bashrc' "$export_install" "$export_path"
      ;;
  esac
}

get_target() {
{{targetSelection}}
}

main() {
  if ! has_cmd curl; then
    die "curl is required to install $app_name"
  fi

  if [[ $# -gt 0 ]]; then
    die "Too many arguments. Usage: install"
  fi

  if [[ -z "${HOME:-}" ]]; then
    die "HOME environment variable not set"
  fi

  local target ant_uri
  target="$(get_target)"
  ant_uri="${ANT_DOWNLOAD_URL:-$download_url}"
  ant_uri="${ant_uri//\{target\}/$target}"

  local install_dir bin_dir exe
  install_dir="${!install_env:-$HOME/$default_install_dir}"
  bin_dir="$install_dir/bin"
  exe="$bin_dir/$binary_name"

  printf '\n'
  bold 'Ant installer'
  printf '\n'
  line
  detail "target: $target"
  detail "install: $(tildify "$exe")"
  detail "source: $ant_uri"
  printf '\n'

  run_step "Preparing install directory" mkdir -p "$bin_dir" ||
    die "Failed to create install directory \"$bin_dir\""

  run_with_loader "Downloading Ant" \
    curl --fail --location --silent --show-error --output "$exe" "$ant_uri" ||
    die "Failed to download $app_name from \"$ant_uri\""

  run_step "Setting executable permissions" chmod 755 "$exe" ||
    die "Failed to set permissions on $app_name executable"

  success "$app_name installed to $(tildify "$exe")"

  if has_cmd "$binary_name"; then
    printf '\n'
    dim 'Ready.'
    printf '   '
    bold "$binary_name --help"
    printf '\n'
    exit 0
  fi

  local tilde_bin_dir quoted_install_dir bin_env shell_path shell_name configured_refresh_command
  tilde_bin_dir="$(tildify "$bin_dir")"
  quoted_install_dir="\"$install_dir\""
  if [[ "$install_dir" == "$HOME"* ]]; then
    quoted_install_dir="${quoted_install_dir//$HOME/\$HOME}"
  fi
  bin_env="\$$install_env/bin"
  shell_path="${SHELL:-/bin/sh}"
  shell_name="$(basename "$shell_path")"
  configured_refresh_command=""

  printf '\n'
  status "Updating shell profile"
  configure_shell "$shell_name" "$quoted_install_dir" "$bin_env" "$tilde_bin_dir"

  printf '\n'
  dim 'Next steps:'
  if [[ -n "$configured_refresh_command" ]]; then
    printf '  '
    bold "$configured_refresh_command"
    printf '\n'
  fi
  printf '  '
  bold "$binary_name --help"
  printf '\n'
}

main "$@"
