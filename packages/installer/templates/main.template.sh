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

  run_download "Downloading Ant" "$exe" "$ant_uri" ||
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
