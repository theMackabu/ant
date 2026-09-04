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
