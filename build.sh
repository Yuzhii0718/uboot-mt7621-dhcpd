#!/usr/bin/env bash

set -euo pipefail

# Default values
DEFAULT_MTDPARTS="512k(u-boot),512k(u-boot-env),512k(factory),-(firmware)"
DEFAULT_BAUDRATE="115200"
DEFAULT_CONFIG_DIR="configs-mt7621"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOARD="${BOARD:-}"
LOADED_DEFCONFIG=""
DEFCONFIG_ARGS=()

FLASH=""
MTDPARTS=""
KERNEL_OFFSET=""
RESET_PIN="-1"
SYSLED_PIN="-1"
WPS_PIN="-1"
SYSLED2_PIN="-1"
CPUFREQ=""
RAMFREQ=""
DDRPARM=""
OLDPARAM="false"
BAUDRATE="${DEFAULT_BAUDRATE}"
YES="0"

# Board identity (optional, for failsafe sysinfo fallback)
MODEL=""
BOARD_NAME=""

# Partition defaults
DEFAULT_UBOOT_SIZE="512k"
DEFAULT_UBOOT_ENV_SIZE="512k"
DEFAULT_FACTORY_SIZE="512k"

# Partition sizes (optional, used to build MTDPARTS)
UBOOT_SIZE=""
UBOOT_ENV_SIZE=""
FACTORY_SIZE=""

print_usage() {
  cat <<EOF
Usage:
  ./build.sh                      # interactive selection
  ./build.sh [options]            # non-interactive build
  BOARD=<board> ./build.sh        # automatically load configs-mt7621/<board>_defconfig

Options:
  --flash {NOR|NAND|NMBM}         Flash type
  --mtdparts STRING               MTD partition table (without device prefix), example:
                                  512k(u-boot),512k(u-boot-env),512k(factory),-(firmware)
  --uboot-size SIZE               u-boot partition size (optional/legacy: only used to build the default partition table)
  --uboot-env-size SIZE           u-boot-env partition size (optional/legacy: only used to build the default partition table)
  --factory-size SIZE             factory partition size (optional/legacy: only used to build the default partition table)
  --kernel-offset VALUE           Kernel offset (e.g., 0x60000 or decimal)
  --reset-pin INT                 Reset button GPIO (0-48, or -1 to disable)
  --wps-pin INT                   WPS button GPIO (0-48, or -1 to disable; requires reset-pin)
  --sysled-pin INT                System LED GPIO (0-48, or -1 to disable)
  --sysled2-pin INT               System LED2 GPIO (0-48, or -1 to disable)
  --cpufreq INT                   CPU frequency MHz (400-1200)
  --ramfreq {400|800|1066|1200}   DRAM speed MT/s
  --ddrparam NAME                 DDR parameter (choose from built-in list or custom)
  --oldparam {true|false}         Use old DDR timing parameters (default false)
  --baudrate {57600|115200}       Serial baud rate (default 115200)
  --model STRING                  Device model/variant (optional, will be written into failsafe sysinfo fallback)
  --board-name STRING             Board name/code (optional, defaults to --model)
  --yes                           Skip interactive confirmation
  -h, --help                      Show help

Example (non-interactive):
  ./build.sh \
    --flash NMBM \
    --mtdparts "512k(u-boot),512k(u-boot-env),512k(factory),-(firmware)" \
    --model "FCJ_G-AX1800-F" \
    --kernel-offset 0x180000 \
    --reset-pin 7 \
    --wps-pin 6 \
    --sysled-pin 13 \
    --sysled2-pin 15 \
    --cpufreq 1000 \
    --ramfreq 1200 \
    --ddrparam DDR3-512MiB \
    --oldparam false \
    --baudrate 115200 \
    --yes
EOF
}

load_board_defconfig() {
  local board="$1"
  local cfg_file="${SCRIPT_DIR}/${DEFAULT_CONFIG_DIR}/${board}_defconfig"
  local line
  local -a parsed

  if [[ ! -f "${cfg_file}" ]]; then
    echo "Error: BOARD='${board}' specified but config file not found: ${cfg_file}"; return 1
  fi

  DEFCONFIG_ARGS=()
  while IFS= read -r line || [[ -n "${line}" ]]; do
    # Trim leading/trailing whitespace
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    # Skip blank lines and comments
    if [[ -z "${line}" || "${line}" == \#* ]]; then
      continue
    fi

    parsed=()
    # Use shell semantics to split (supports quotes), for local defconfig files only
    # shellcheck disable=SC2206
    eval "parsed=(${line})"
    if (( ${#parsed[@]} > 0 )); then
      DEFCONFIG_ARGS+=("${parsed[@]}")
    fi
  done < "${cfg_file}"

  LOADED_DEFCONFIG="${cfg_file}"
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --flash) FLASH="$2"; shift 2;;
      --mtdparts) MTDPARTS="$2"; shift 2;;
      --uboot-size) UBOOT_SIZE="$2"; shift 2;;
      --uboot-env-size) UBOOT_ENV_SIZE="$2"; shift 2;;
      --factory-size) FACTORY_SIZE="$2"; shift 2;;
      --kernel-offset) KERNEL_OFFSET="$2"; shift 2;;
      --reset-pin) RESET_PIN="$2"; shift 2;;
      --wps-pin) WPS_PIN="$2"; shift 2;;
      --sysled-pin) SYSLED_PIN="$2"; shift 2;;
      --sysled2-pin) SYSLED2_PIN="$2"; shift 2;;
      --cpufreq) CPUFREQ="$2"; shift 2;;
      --ramfreq) RAMFREQ="$2"; shift 2;;
      --ddrparam) DDRPARM="$2"; shift 2;;
      --oldparam) OLDPARAM="$2"; shift 2;;
      --baudrate) BAUDRATE="$2"; shift 2;;
      --model) MODEL="$2"; shift 2;;
      --board-name) BOARD_NAME="$2"; shift 2;;
      --yes) YES="1"; shift;;
      -h|--help) print_usage; exit 0;;
      *) echo "Unknown argument: $1"; print_usage; exit 1;;
    esac
  done
}

is_size_token() {
  local v="$1"
  [[ "$v" =~ ^[0-9]+[kKmM]$ ]]
}

build_mtdparts() {
  local u="${UBOOT_SIZE:-${DEFAULT_UBOOT_SIZE}}"
  local e="${UBOOT_ENV_SIZE:-${DEFAULT_UBOOT_ENV_SIZE}}"
  local f="${FACTORY_SIZE:-${DEFAULT_FACTORY_SIZE}}"
  echo "${u}(u-boot),${e}(u-boot-env),${f}(factory),-(firmware)"
}

ask() {
  local prompt="$1"; shift
  local default_val="${1:-}"; shift || true
  local var
  if [[ -n "${default_val}" ]]; then
    if [[ -t 0 ]]; then
      # -e: readline (supports arrow keys/history); -i: prefill default value
      read -e -r -p "${prompt} [Default: ${default_val}] > " -i "${default_val}" var || true
    else
      read -r -p "${prompt} [Default: ${default_val}] > " var || true
    fi
    echo "${var:-${default_val}}"
  else
    if [[ -t 0 ]]; then
      read -e -r -p "${prompt} > " var || true
    else
      read -r -p "${prompt} > " var || true
    fi
    echo "${var}"
  fi
}

select_from() {
  local prompt="$1"; shift
  local -a items=("$@")
  echo "${prompt}" >&2;
  local i=1
  for it in "${items[@]}"; do
    echo "  ${i}) ${it}" >&2
    ((i++))
  done
  if [[ -t 0 ]]; then
    read -e -r -p "Select index (enter number 1-${#items[@]}) > " idx || true
  else
    read -r -p "Select index (enter number 1-${#items[@]}) > " idx || true
  fi
  if [[ -z "${idx}" ]] || ! [[ "${idx}" =~ ^[0-9]+$ ]] || (( idx < 1 || idx > ${#items[@]} )); then
    echo ""; return 1
  fi
  echo "${items[$((idx-1))]}"
}

select_with_default() {
  local prompt="$1"; shift
  local default_val="$1"; shift
  local -a items=("$@")
  echo "${prompt}" >&2;
  local i=1
  for it in "${items[@]}"; do
    echo "  ${i}) ${it}" >&2
    ((i++))
  done
  if [[ -t 0 ]]; then
    read -e -r -p "Select index (default: ${default_val}) > " idx || true
  else
    read -r -p "Select index (default: ${default_val}) > " idx || true
  fi
  if [[ -z "${idx}" ]]; then
    echo "${default_val}"; return 0
  fi
  if ! [[ "${idx}" =~ ^[0-9]+$ ]] || (( idx < 1 || idx > ${#items[@]} )); then
    echo "${default_val}"; return 0
  fi
  echo "${items[$((idx-1))]}"
}

validate() {
  # MTDPARTS basic validation: allow renaming/adding partitions, but must include firmware partition
  # Note: script expects partition string without device prefix (do not include spi0.0: etc)
  if [[ -z "${MTDPARTS}" ]]; then
    echo "Error: No MTD partition table provided, example: ${DEFAULT_MTDPARTS}"; exit 1
  fi
  if echo -n "${MTDPARTS}" | grep -Eq '^[^,()]+:'; then
    echo "Error: MTD partition table must not include device prefix (e.g., spi0.0:), should look like: ${DEFAULT_MTDPARTS}"; exit 1
  fi
  if ! echo -n "${MTDPARTS}" | grep -q "(firmware)"; then
    echo "Error: MTD partition table must include a partition named firmware, e.g.: ${DEFAULT_MTDPARTS}"; exit 1
  fi
  if ! echo -n "${MTDPARTS}" | grep -Eq '\([^()]+\)'; then
    echo "Error: MTD partition table format invalid, example: ${DEFAULT_MTDPARTS}"; exit 1
  fi
  # If separate partition sizes provided, perform basic validation
  for tok in "${UBOOT_SIZE}" "${UBOOT_ENV_SIZE}" "${FACTORY_SIZE}"; do
    if [[ -n "$tok" ]] && ! is_size_token "$tok"; then
      echo "Error: Partition size must be number+unit (k/m), e.g., 512k, 1m"; exit 1
    fi
  done
  # FLASH type
  case "${FLASH}" in
    NOR|NAND|NMBM) :;;
    *) echo "Error: Choose FLASH type NOR/NAND/NMBM"; exit 1;;
  esac
  # KERNEL_OFFSET accepts hex or decimal
  if [[ -z "${KERNEL_OFFSET}" ]] || ! [[ "${KERNEL_OFFSET}" =~ ^(0x[0-9a-fA-F]+|[0-9]+)$ ]]; then
    echo "Error: kernel-offset must be hexadecimal (e.g., 0x60000) or decimal"; exit 1
  fi
  # GPIO range or -1
  for p in RESET_PIN SYSLED_PIN WPS_PIN SYSLED2_PIN; do
    local val="${!p}"
    if ! [[ "${val}" =~ ^-?[0-9]+$ ]]; then
      echo "Error: ${p} must be an integer (-1 or 0-48)"; exit 1
    fi
    if (( val != -1 && (val < 0 || val > 48) )); then
      echo "Error: ${p} out of range (-1 or 0-48)"; exit 1
    fi
  done
  if (( WPS_PIN != -1 && RESET_PIN == -1 )); then
    echo "Error: Using wps-pin requires reset-pin to be enabled"; exit 1
  fi
  # CPU frequency
  if [[ -z "${CPUFREQ}" ]] || ! [[ "${CPUFREQ}" =~ ^[0-9]+$ ]] || (( CPUFREQ < 400 || CPUFREQ > 1200 )); then
    echo "Error: cpufreq must be an integer MHz between 400 and 1200"; exit 1
  fi
  # RAM frequency
  case "${RAMFREQ}" in
    400|800|1066|1200) :;;
    *) echo "Error: ramfreq only supports 400/800/1066/1200"; exit 1;;
  esac
  # old DDR parameter switch
  case "${OLDPARAM,,}" in
    true|false|1|0|yes|no|y|n) :;;
    *) echo "Error: oldparam only supports true/false"; exit 1;;
  esac
  # Baud rate
  case "${BAUDRATE}" in
    57600|115200) :;;
    *) echo "Error: baudrate only supports 57600 or 115200"; exit 1;;
  esac
}

interactive() {
  FLASH=$(select_with_default "Select flash type:" "NMBM" NOR NAND NMBM)
  # Partition table: allow renaming or adding custom partitions
  # Note: input should be partition string without device prefix and must include (firmware)
  MTDPARTS=$(ask "Enter MTD partition table (without device prefix, must include firmware partition)" "${DEFAULT_MTDPARTS}")
  # kernel offset (different flashes may use different offsets) — example shown
  local example_offset="0x180000"
  KERNEL_OFFSET=$(ask "Enter kernel offset (example ${example_offset})" "${example_offset}")
  # GPIOs
  RESET_PIN=$(ask "Reset button GPIO (0-48, -1 to disable)" "-1")
  SYSLED_PIN=$(ask "System LED GPIO (0-48, -1 to disable)" "-1")
  WPS_PIN=$(ask "WPS button GPIO (0-48, -1 to disable; requires reset-pin)" "-1")
  SYSLED2_PIN=$(ask "System LED2 GPIO (0-48, -1 to disable)" "-1")
  # CPU frequency
  local cpusel=$(select_with_default "Select CPU frequency (MHz):" "1000" 880 1000 1100 1200)
  CPUFREQ="${cpusel}"
  # RAM frequency
  local ramsel=$(select_with_default "Select DRAM speed (MT/s):" "1200" 400 800 1066 1200)
  RAMFREQ="${ramsel}"
  # DDR parameters
  echo "Choose DDR initialization parameter (or leave empty to enter custom):"
  local ddrsel=$(select_from "Built-in list:" \
    DDR2-64MiB \
    DDR2-128MiB \
    DDR2-W9751G6KB-64MiB-1066MHz \
    DDR2-W971GG6KB25-128MiB-800MHz \
    DDR2-W971GG6KB18-128MiB-1066MHz \
    DDR3-128MiB \
    DDR3-256MiB \
    DDR3-512MiB \
    DDR3-128MiB-KGD) || true
  if [[ -z "${ddrsel}" ]]; then
    DDRPARM=$(ask "Custom DDR parameter (case must match entries in customize.sh case)" "DDR3-256MiB")
  else
    DDRPARM="${ddrsel}"
  fi
  # old DDR parameter switch
  local oldsel
  oldsel=$(select_with_default "Use old DDR timing parameters:" "false" false true)
  OLDPARAM="${oldsel}"
  # Baud rate
  local brsel=$(select_with_default "Select serial baud rate:" "115200" 57600 115200)
  BAUDRATE="${brsel}"

  # Model/Name (optional; used as a fallback display in failsafe sysinfo)
  MODEL=$(ask "Device model/variant (optional; leave empty to not write into failsafe fallback config)" "")
  if [[ -n "${MODEL}" ]]; then
    BOARD_NAME=$(ask "Board name/code (optional, default same as above)" "${MODEL}")
  else
    BOARD_NAME=""
  fi
}

summary() {
  cat <<EOF
======================================================================
Will execute:
  ./customize.sh '${FLASH}' '${MTDPARTS}' '${KERNEL_OFFSET}' '${RESET_PIN}' \
  '${SYSLED_PIN}' '${CPUFREQ}' '${RAMFREQ}' '${DDRPARM}' '${BAUDRATE}' '${MODEL}' '${BOARD_NAME}' '${OLDPARAM}' '${WPS_PIN}' '${SYSLED2_PIN}'
EOF
}

main() {
  if [[ -n "${BOARD}" ]]; then
    load_board_defconfig "${BOARD}" || exit 1
    parse_args "${DEFCONFIG_ARGS[@]}" "$@"
  else
    parse_args "$@"
  fi
  # If MTDPARTS not provided directly but partition sizes are provided, build MTDPARTS
  if [[ -z "${MTDPARTS}" ]] && { [[ -n "${UBOOT_SIZE}" ]] || [[ -n "${UBOOT_ENV_SIZE}" ]] || [[ -n "${FACTORY_SIZE}" ]]; }; then
    MTDPARTS=$(build_mtdparts)
  fi
  if [[ -z "${FLASH}" || -z "${MTDPARTS}" || -z "${KERNEL_OFFSET}" || -z "${CPUFREQ}" || -z "${RAMFREQ}" || -z "${DDRPARM}" ]]; then
    echo "Entering interactive configuration..."
    interactive
  fi
  validate

  # defaults
  if [[ -z "${BOARD_NAME}" ]] && [[ -n "${MODEL}" ]]; then
    BOARD_NAME="${MODEL}"
  fi

  if [[ -n "${LOADED_DEFCONFIG}" ]]; then
    echo "Loaded BOARD Config: ${LOADED_DEFCONFIG}"
  fi

  summary
  if [[ "${YES}" != "1" ]]; then
    if [[ -t 0 ]]; then
      read -e -r -p "Confirm execution? [y/N] " confirm || true
    else
      read -r -p "Confirm execution? [y/N] " confirm || true
    fi
    if [[ "${confirm,,}" != "y" ]]; then
      echo "Cancelled."; exit 0
    fi
  fi
  ./customize.sh "${FLASH}" "${MTDPARTS}" "${KERNEL_OFFSET}" "${RESET_PIN}" \
                 "${SYSLED_PIN}" "${CPUFREQ}" "${RAMFREQ}" "${DDRPARM}" "${BAUDRATE}" "${MODEL}" "${BOARD_NAME}" "${OLDPARAM}" "${WPS_PIN}" "${SYSLED2_PIN}"
  echo "======================================================================"
  echo "Build complete. If successful, artifacts are located in ./archive/ ."
}

main "$@"
