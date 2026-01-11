#!/usr/bin/env bash
# Version: [Amber](https://amber-lang.com/) 0.5.1-alpha
# Source: https://github.com/theMackabu/ant/blob/master/.github/install/install.ab

bash_version__0_v0() {
    major_26="$(echo "${BASH_VERSINFO[0]}")"
    minor_27="$(echo "${BASH_VERSINFO[1]}")"
    command_2="$(echo "${BASH_VERSINFO[2]}")"
    __status=$?
    patch_28="${command_2}"
    ret_bash_version0_v0=("${major_26}" "${minor_27}" "${patch_28}")
    return 0
}

replace__1_v0() {
    local source=$1
    local search=$2
    local replace=$3
    # Here we use a command to avoid #646
    result_25=""
    bash_version__0_v0 
    left_comp=("${ret_bash_version0_v0[@]}")
    right_comp=(4 3)
    comp="$(
        # Compare if left array >= right array
        len_comp="$( (( "${#left_comp[@]}" < "${#right_comp[@]}" )) && echo "${#left_comp[@]}"|| echo "${#right_comp[@]}")"
        for (( i=0; i<len_comp; i++ )); do
            left="${left_comp[i]:-0}"
            right="${right_comp[i]:-0}"
            if (( "${left}" > "${right}" )); then
                echo 1
                exit
            elif (( "${left}" < "${right}" )); then
                echo 0
                exit
            fi
        done
        (( "${#left_comp[@]}" == "${#right_comp[@]}" || "${#left_comp[@]}" > "${#right_comp[@]}" )) && echo 1 || echo 0
)"
    if [ "${comp}" != 0 ]; then
        result_25="${source//"${search}"/"${replace}"}"
        __status=$?
    else
        result_25="${source//"${search}"/${replace}}"
        __status=$?
    fi
    ret_replace1_v0="${result_25}"
    return 0
}

text_contains__17_v0() {
    local source=$1
    local search=$2
    command_5="$(if [[ "${source}" == *"${search}"* ]]; then
    echo 1
  fi)"
    __status=$?
    result_6="${command_5}"
    ret_text_contains17_v0="$([ "_${result_6}" != "_1" ]; echo $?)"
    return 0
}

starts_with__23_v0() {
    local text=$1
    local prefix=$2
    command_6="$(if [[ "${text}" == "${prefix}"* ]]; then
    echo 1
  fi)"
    __status=$?
    result_7="${command_6}"
    ret_starts_with23_v0="$([ "_${result_7}" != "_1" ]; echo $?)"
    return 0
}

dir_exists__39_v0() {
    local path=$1
    [ -d "${path}" ]
    __status=$?
    ret_dir_exists39_v0="$(( ${__status} == 0 ))"
    return 0
}

file_exists__40_v0() {
    local path=$1
    [ -f "${path}" ]
    __status=$?
    ret_file_exists40_v0="$(( ${__status} == 0 ))"
    return 0
}

file_append__43_v0() {
    local path=$1
    local content=$2
    command_7="$(echo "${content}" >> "${path}")"
    __status=$?
    if [ "${__status}" != 0 ]; then
        ret_file_append43_v0=''
        return "${__status}"
    fi
    ret_file_append43_v0="${command_7}"
    return 0
}

dir_create__45_v0() {
    local path=$1
    dir_exists__39_v0 "${path}"
    ret_dir_exists39_v0__87_12="${ret_dir_exists39_v0}"
    if [ "$(( ! ${ret_dir_exists39_v0__87_12} ))" != 0 ]; then
        mkdir -p "${path}"
        __status=$?
        if [ "${__status}" != 0 ]; then
            ret_dir_create45_v0=''
            return "${__status}"
        fi
    fi
}

file_chmod__48_v0() {
    local path=$1
    local mode=$2
    file_exists__40_v0 "${path}"
    ret_file_exists40_v0__153_8="${ret_file_exists40_v0}"
    if [ "${ret_file_exists40_v0__153_8}" != 0 ]; then
        chmod "${mode}" "${path}"
        __status=$?
        if [ "${__status}" != 0 ]; then
            ret_file_chmod48_v0=''
            return "${__status}"
        fi
        ret_file_chmod48_v0=''
        return 0
    fi
    echo "The file ${path} doesn't exist"'!'""
    ret_file_chmod48_v0=''
    return 1
}

env_var_get__106_v0() {
    local name=$1
    command_8="$(echo ${!name})"
    __status=$?
    if [ "${__status}" != 0 ]; then
        ret_env_var_get106_v0=''
        return "${__status}"
    fi
    ret_env_var_get106_v0="${command_8}"
    return 0
}

is_command__108_v0() {
    local command=$1
    [ -x "$(command -v "${command}")" ]
    __status=$?
    if [ "${__status}" != 0 ]; then
        ret_is_command108_v0=0
        return 0
    fi
    ret_is_command108_v0=1
    return 0
}

printf__114_v0() {
    local format=$1
    local args=("${!2}")
    args=("${format}" "${args[@]}")
    __status=$?
    printf "${args[@]}"
    __status=$?
}

escaped__115_v0() {
    local text=$1
    command_9="$(echo $text | sed -e 's/\\/\\\\/g' -e "s/%/%%/g")"
    __status=$?
    ret_escaped115_v0="${command_9}"
    return 0
}

bold__117_v0() {
    local message=$1
    escaped__115_v0 "${message}"
    ret_escaped115_v0__217_21="${ret_escaped115_v0}"
    ret_bold117_v0="\\x1b[1m${ret_escaped115_v0__217_21}\\x1b[0m"
    return 0
}

echo_error__124_v0() {
    local message=$1
    local exit_code=$2
    array_10=("${message}")
    printf__114_v0 "\\x1b[1;3;97;41m%s\\x1b[0m
" array_10[@]
    if [ "$(( ${exit_code} > 0 ))" != 0 ]; then
        exit "${exit_code}"
    fi
}

tildify__134_v0() {
    local path=$1
    env_var_get__106_v0 "HOME"
    __status=$?
    if [ "${__status}" != 0 ]; then
        ret_tildify134_v0="${path}"
        return 0
    fi
    home_24="${ret_env_var_get106_v0}"
    starts_with__23_v0 "${path}" "${home_24}"
    ret_starts_with23_v0__9_6="${ret_starts_with23_v0}"
    if [ "${ret_starts_with23_v0__9_6}" != 0 ]; then
        replace__1_v0 "${path}" "${home_24}" "~"
        ret_tildify134_v0="${ret_replace1_v0}"
        return 0
    fi
    ret_tildify134_v0="${path}"
    return 0
}

get_target__135_v0() {
    command_11="$(uname -ms)"
    __status=$?
    platform_4="${command_11}"
    target_5=""
    text_contains__17_v0 "${platform_4}" "Darwin"
    ret_text_contains17_v0__21_5="${ret_text_contains17_v0}"
    text_contains__17_v0 "${platform_4}" "x86_64"
    ret_text_contains17_v0__21_43="${ret_text_contains17_v0}"
    text_contains__17_v0 "${platform_4}" "Darwin"
    ret_text_contains17_v0__23_5="${ret_text_contains17_v0}"
    text_contains__17_v0 "${platform_4}" "arm64"
    ret_text_contains17_v0__23_43="${ret_text_contains17_v0}"
    text_contains__17_v0 "${platform_4}" "Linux"
    ret_text_contains17_v0__25_5="${ret_text_contains17_v0}"
    text_contains__17_v0 "${platform_4}" "aarch64"
    ret_text_contains17_v0__25_43="${ret_text_contains17_v0}"
    text_contains__17_v0 "${platform_4}" "arm64"
    ret_text_contains17_v0__25_81="${ret_text_contains17_v0}"
    text_contains__17_v0 "${platform_4}" "MINGW64"
    ret_text_contains17_v0__27_5="${ret_text_contains17_v0}"
    text_contains__17_v0 "${platform_4}" "Linux"
    ret_text_contains17_v0__29_5="${ret_text_contains17_v0}"
    text_contains__17_v0 "${platform_4}" "riscv64"
    ret_text_contains17_v0__29_42="${ret_text_contains17_v0}"
    if [ "$(( ${ret_text_contains17_v0__21_5} && ${ret_text_contains17_v0__21_43} ))" != 0 ]; then
        target_5="darwin-x64"
    elif [ "$(( ${ret_text_contains17_v0__23_5} && ${ret_text_contains17_v0__23_43} ))" != 0 ]; then
        target_5="darwin-aarch64"
    elif [ "$(( ${ret_text_contains17_v0__25_5} && $(( ${ret_text_contains17_v0__25_43} || ${ret_text_contains17_v0__25_81} )) ))" != 0 ]; then
        target_5="linux-aarch64"
    elif [ "${ret_text_contains17_v0__27_5}" != 0 ]; then
        target_5="windows-x64"
    elif [ "$(( ${ret_text_contains17_v0__29_5} && ${ret_text_contains17_v0__29_42} ))" != 0 ]; then
        echo_error__124_v0 "Not supported on riscv64" 1
    else
        target_5="linux-x64"
    fi
    starts_with__23_v0 "${target_5}" "linux"
    ret_starts_with23_v0__35_6="${ret_starts_with23_v0}"
    if [ "${ret_starts_with23_v0__35_6}" != 0 ]; then
        file_exists__40_v0 "/etc/alpine-release"
        ret_file_exists40_v0__36_8="${ret_file_exists40_v0}"
        if [ "${ret_file_exists40_v0__36_8}" != 0 ]; then
            target_5="${target_5}-musl"
        fi
    fi
    if [ "$([ "_${target_5}" != "_darwin-x64" ]; echo $?)" != 0 ]; then
        command_12="$(sysctl -n sysctl.proc_translated 2>/dev/null >/dev/null 2>&1)"
        __status=$?
        if [ "${__status}" != 0 ]; then
            :
        fi
        rosetta_8="${command_12}"
        if [ "$([ "_${rosetta_8}" != "_1" ]; echo $?)" != 0 ]; then
            target_5="darwin-aarch64"
            echo -e "\x1b[2mYour shell is running in Rosetta 2. Downloading ant for ${target_5} instead\x1b[0m"
            __status=$?
        fi
    fi
    ret_get_target135_v0="${target_5}"
    return 0
}

get_shell_config__136_v0() {
    local shell_name=$1
    local install_env=$2
    local quoted_install_dir=$3
    local bin_env=$4
    if [ "$([ "_${shell_name}" != "_fish" ]; echo $?)" != 0 ]; then
        ret_get_shell_config136_v0=("set --export ${install_env} ${quoted_install_dir}" "set --export PATH ${bin_env} \$PATH")
        return 0
    fi
    ret_get_shell_config136_v0=("export ${install_env}=${quoted_install_dir}" "export PATH=\"${bin_env}:\$PATH\"")
    return 0
}

configure_shell__137_v0() {
    local shell_name=$1
    local install_env=$2
    local quoted_install_dir=$3
    local bin_env=$4
    local tilde_bin_dir=$5
    get_shell_config__136_v0 "${shell_name}" "${install_env}" "${quoted_install_dir}" "${bin_env}"
    commands_33=("${ret_get_shell_config136_v0[@]}")
    refresh_command_34=""
    env_var_get__106_v0 "HOME"
    __status=$?
    if [ "${__status}" != 0 ]; then
        ret_configure_shell137_v0=""
        return 0
    fi
    home_35="${ret_env_var_get106_v0}"
    if [ "$([ "_${shell_name}" != "_fish" ]; echo $?)" != 0 ]; then
        fish_config_36="${home_35}/.config/fish/config.fish"
        tildify__134_v0 "${fish_config_36}"
        tilde_fish_config_37="${ret_tildify134_v0}"
        file_exists__40_v0 "${fish_config_36}"
        ret_file_exists40_v0__79_10="${ret_file_exists40_v0}"
        if [ "${ret_file_exists40_v0__79_10}" != 0 ]; then
            content_38="
# ant
"
            for command_39 in "${commands_33[@]}"; do
                content_38+="${command_39}
"
            done
            file_append__43_v0 "${fish_config_36}" "${content_38}"
            __status=$?
            if [ "${__status}" != 0 ]; then
                echo "Manually add the directory to ${tilde_fish_config_37} (or similar):"
                for cmd_40 in "${commands_33[@]}"; do
                    bold__117_v0 "${cmd_40}"
                    ret_bold117_v0__87_32="${ret_bold117_v0}"
                    echo -e "  ${ret_bold117_v0__87_32}"
                    __status=$?
                done
                ret_configure_shell137_v0=""
                return 0
            fi
            echo -e "\x1b[2mAdded \"${tilde_bin_dir}\" to \$PATH in \"${tilde_fish_config_37}\"\x1b[0m"
            __status=$?
            refresh_command_34="source ${tilde_fish_config_37}"
        else
            echo "Manually add the directory to ${tilde_fish_config_37} (or similar):"
            for cmd_41 in "${commands_33[@]}"; do
                bold__117_v0 "${cmd_41}"
                ret_bold117_v0__96_30="${ret_bold117_v0}"
                echo -e "  ${ret_bold117_v0__96_30}"
                __status=$?
            done
        fi
    elif [ "$([ "_${shell_name}" != "_zsh" ]; echo $?)" != 0 ]; then
        zsh_config_42="${home_35}/.zshrc"
        tildify__134_v0 "${zsh_config_42}"
        tilde_zsh_config_43="${ret_tildify134_v0}"
        file_exists__40_v0 "${zsh_config_42}"
        ret_file_exists40_v0__104_10="${ret_file_exists40_v0}"
        if [ "${ret_file_exists40_v0__104_10}" != 0 ]; then
            content_44="
# ant
"
            for command_45 in "${commands_33[@]}"; do
                content_44+="${command_45}
"
            done
            file_append__43_v0 "${zsh_config_42}" "${content_44}"
            __status=$?
            if [ "${__status}" != 0 ]; then
                echo "Manually add the directory to ${tilde_zsh_config_43} (or similar):"
                for cmd_46 in "${commands_33[@]}"; do
                    bold__117_v0 "${cmd_46}"
                    ret_bold117_v0__112_32="${ret_bold117_v0}"
                    echo -e "  ${ret_bold117_v0__112_32}"
                    __status=$?
                done
                ret_configure_shell137_v0=""
                return 0
            fi
            echo -e "\x1b[2mAdded \"${tilde_bin_dir}\" to \$PATH in \"${tilde_zsh_config_43}\"\x1b[0m"
            __status=$?
            refresh_command_34="source ${tilde_zsh_config_43}"
        else
            echo "Manually add the directory to ${tilde_zsh_config_43} (or similar):"
            for cmd_47 in "${commands_33[@]}"; do
                bold__117_v0 "${cmd_47}"
                ret_bold117_v0__121_30="${ret_bold117_v0}"
                echo -e "  ${ret_bold117_v0__121_30}"
                __status=$?
            done
        fi
    elif [ "$([ "_${shell_name}" != "_bash" ]; echo $?)" != 0 ]; then
        bash_configs_48=("${home_35}/.bash_profile" "${home_35}/.bashrc")
        env_var_get__106_v0 "XDG_CONFIG_HOME"
        __status=$?
        if [ "${__status}" != 0 ]; then
            :
        fi
        xdg_config_49="${ret_env_var_get106_v0}"
        if [ "$([ "_${xdg_config_49}" == "_" ]; echo $?)" != 0 ]; then
            bash_configs_48+=("${xdg_config_49}/.bash_profile" "${xdg_config_49}/.bashrc" "${xdg_config_49}/bash_profile" "${xdg_config_49}/bashrc")
        fi
        set_manually_50=1
        for bash_config_51 in "${bash_configs_48[@]}"; do
            tildify__134_v0 "${bash_config_51}"
            tilde_bash_config_52="${ret_tildify134_v0}"
            file_exists__40_v0 "${bash_config_51}"
            ret_file_exists40_v0__147_12="${ret_file_exists40_v0}"
            if [ "${ret_file_exists40_v0__147_12}" != 0 ]; then
                content_53="
# ant
"
                for command_54 in "${commands_33[@]}"; do
                    content_53+="${command_54}
"
                done
                file_append__43_v0 "${bash_config_51}" "${content_53}"
                __status=$?
                if [ "${__status}" != 0 ]; then
                    continue
                fi
                echo -e "\x1b[2mAdded \"${tilde_bin_dir}\" to \$PATH in \"${tilde_bash_config_52}\"\x1b[0m"
                __status=$?
                refresh_command_34="source ${tilde_bash_config_52}"
                set_manually_50=0
                break
            fi
        done
        if [ "${set_manually_50}" != 0 ]; then
            echo "Manually add the directory to ~/.bashrc (or similar):"
            for cmd_55 in "${commands_33[@]}"; do
                bold__117_v0 "${cmd_55}"
                ret_bold117_v0__165_30="${ret_bold117_v0}"
                echo -e "  ${ret_bold117_v0__165_30}"
                __status=$?
            done
        fi
    else
        echo "Manually add the directory to ~/.bashrc (or similar):"
        bold__117_v0 "export ${install_env}=${quoted_install_dir}"
        ret_bold117_v0__171_26="${ret_bold117_v0}"
        echo -e "  ${ret_bold117_v0__171_26}"
        __status=$?
        bold__117_v0 "export PATH=\"${bin_env}:\$PATH\""
        ret_bold117_v0__172_26="${ret_bold117_v0}"
        echo -e "  ${ret_bold117_v0__172_26}"
        __status=$?
    fi
    ret_configure_shell137_v0="${refresh_command_34}"
    return 0
}

declare -r args_3=("$0" "$@")
is_command__108_v0 "curl"
ret_is_command108_v0__180_10="${ret_is_command108_v0}"
if [ "$(( ! ${ret_is_command108_v0__180_10} ))" != 0 ]; then
    echo_error__124_v0 "curl is required to install ant" 1
fi
is_command__108_v0 "unzip"
ret_is_command108_v0__184_10="${ret_is_command108_v0}"
if [ "$(( ! ${ret_is_command108_v0__184_10} ))" != 0 ]; then
    echo_error__124_v0 "unzip is required to install ant" 1
fi
__length_18=("${args_3[@]}")
if [ "$(( ${#__length_18[@]} > 3 ))" != 0 ]; then
    echo_error__124_v0 "Too many arguments. Usage: install.ab [tag] [--mbedtls]" 1
fi
get_target__135_v0 
target_9="${ret_get_target135_v0}"
use_mbedtls_10=0
for arg_11 in "${args_3[@]}"; do
    if [ "$([ "_${arg_11}" != "_--mbedtls" ]; echo $?)" != 0 ]; then
        use_mbedtls_10=1
    fi
done
if [ "${use_mbedtls_10}" != 0 ]; then
    starts_with__23_v0 "${target_9}" "darwin"
    ret_starts_with23_v0__200_8="${ret_starts_with23_v0}"
    if [ "${ret_starts_with23_v0__200_8}" != 0 ]; then
        target_9="${target_9}-mbedtls"
    else
        echo_error__124_v0 "--mbedtls option is only supported on macOS" 1
    fi
fi
env_var_get__106_v0 "GITHUB"
__status=$?
if [ "${__status}" != 0 ]; then
    :
fi
github_12="${ret_env_var_get106_v0}"
if [ "$([ "_${github_12}" != "_" ]; echo $?)" != 0 ]; then
    github_12="https://github.com"
fi
github_repo_13="${github_12}/themackabu/ant"
ant_uri_14=""
tag_15=""
for arg_16 in "${args_3[@]}"; do
    if [ "$(( $([ "_${arg_16}" == "_--mbedtls" ]; echo $?) && $([ "_${arg_16}" == "_${args_3[0]}" ]; echo $?) ))" != 0 ]; then
        tag_15="${arg_16}"
    fi
done
if [ "$([ "_${tag_15}" != "_" ]; echo $?)" != 0 ]; then
    ant_uri_14="${github_repo_13}/releases/latest/download/ant-${target_9}.zip"
else
    ant_uri_14="${github_repo_13}/releases/download/${tag_15}/ant-${target_9}.zip"
fi
install_env_17="ANT_INSTALL"
bin_env_18="\$${install_env_17}/bin"
env_var_get__106_v0 "HOME"
__status=$?
if [ "${__status}" != 0 ]; then
    echo_error__124_v0 "HOME environment variable not set" 1
fi
home_19="${ret_env_var_get106_v0}"
env_var_get__106_v0 "ANT_INSTALL"
__status=$?
if [ "${__status}" != 0 ]; then
    :
fi
env_install_20="${ret_env_var_get106_v0}"
install_dir_21="$(if [ "$([ "_${env_install_20}" == "_" ]; echo $?)" != 0 ]; then echo "${env_install_20}"; else echo "${home_19}/.ant"; fi)"
bin_dir_22="${install_dir_21}/bin"
exe_23="${bin_dir_22}/ant"
dir_exists__39_v0 "${bin_dir_22}"
ret_dir_exists39_v0__241_10="${ret_dir_exists39_v0}"
if [ "$(( ! ${ret_dir_exists39_v0__241_10} ))" != 0 ]; then
    dir_create__45_v0 "${bin_dir_22}"
    __status=$?
    if [ "${__status}" != 0 ]; then
        echo_error__124_v0 "Failed to create install directory \"${bin_dir_22}\"" 1
    fi
fi
curl --fail --location --progress-bar --output "${exe_23}.zip" "${ant_uri_14}"
__status=$?
if [ "${__status}" != 0 ]; then
    echo_error__124_v0 "Failed to download ant from \"${ant_uri_14}\"" 1
fi
unzip -oqd "${bin_dir_22}" "${exe_23}.zip"
__status=$?
if [ "${__status}" != 0 ]; then
    echo_error__124_v0 "Failed to extract ant" 1
fi
file_chmod__48_v0 "${exe_23}" "755"
__status=$?
if [ "${__status}" != 0 ]; then
    echo_error__124_v0 "Failed to set permissions on ant executable" 1
fi
rm "${exe_23}.zip"
__status=$?
if [ "${__status}" != 0 ]; then
    echo_error__124_v0 "Failed to clean up zip file" 1
fi
tildify__134_v0 "${exe_23}"
ret_tildify134_v0__263_78="${ret_tildify134_v0}"
array_19=("${ret_tildify134_v0__263_78}")
printf__114_v0 "\\x1b[32mant was installed successfully to \\x1b[1;32m%s\\x1b[0m
" array_19[@]
is_command__108_v0 "ant"
ret_is_command108_v0__265_6="${ret_is_command108_v0}"
if [ "${ret_is_command108_v0__265_6}" != 0 ]; then
    echo "Run 'ant --help' to get started"
    exit 0
    __status=$?
fi
tildify__134_v0 "${bin_dir_22}"
tilde_bin_dir_29="${ret_tildify134_v0}"
quoted_install_dir_30="\"${install_dir_21}\""
starts_with__23_v0 "${install_dir_21}" "${home_19}"
ret_starts_with23_v0__273_6="${ret_starts_with23_v0}"
if [ "${ret_starts_with23_v0__273_6}" != 0 ]; then
    replace__1_v0 "${quoted_install_dir_30}" "${home_19}" "\$HOME"
    quoted_install_dir_30="${ret_replace1_v0}"
fi
echo ""
env_var_get__106_v0 "SHELL"
__status=$?
if [ "${__status}" != 0 ]; then
    :
fi
shell_path_31="${ret_env_var_get106_v0}"
command_20="$(basename "${shell_path_31}")"
__status=$?
shell_name_32="${command_20}"
configure_shell__137_v0 "${shell_name_32}" "${install_env_17}" "${quoted_install_dir_30}" "${bin_env_18}" "${tilde_bin_dir_29}"
refresh_command_56="${ret_configure_shell137_v0}"
echo ""
echo -e "\x1b[2mTo get started, run:\x1b[0m"
__status=$?
if [ "$([ "_${refresh_command_56}" == "_" ]; echo $?)" != 0 ]; then
    bold__117_v0 "${refresh_command_56}"
    ret_bold117_v0__289_24="${ret_bold117_v0}"
    echo -e "  ${ret_bold117_v0__289_24}"
    __status=$?
fi
bold__117_v0 "ant --help"
ret_bold117_v0__292_22="${ret_bold117_v0}"
echo -e "  ${ret_bold117_v0__292_22}"
__status=$?
