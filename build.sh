#!/usr/bin/env bash

set -euo pipefail

# Default values
DEFAULT_MTDPARTS="512k(u-boot),512k(u-boot-env),512k(factory),-(firmware)"
DEFAULT_BAUDRATE="115200"

FLASH=""
MTDPARTS=""
KERNEL_OFFSET=""
RESET_PIN="-1"
SYSLED_PIN="-1"
CPUFREQ=""
RAMFREQ=""
DDRPARM=""
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
  ./build.sh                      # 交互式选择
  ./build.sh [options]            # 非交互式构建

Options:
  --flash {NOR|NAND|NMBM}         闪存类型
  --mtdparts STRING               MTD 分区表（不含设备前缀），示例：
                                  512k(u-boot),512k(u-boot-env),512k(factory),-(firmware)
  --uboot-size SIZE               u-boot 分区大小（可选/旧用法：仅用于拼接默认分区表）
  --uboot-env-size SIZE           u-boot-env 分区大小（可选/旧用法：仅用于拼接默认分区表）
  --factory-size SIZE             factory 分区大小（可选/旧用法：仅用于拼接默认分区表）
  --kernel-offset VALUE           内核偏移（例如 0x60000 或十进制数）
  --reset-pin INT                 复位按键 GPIO（0-48，或 -1 禁用）
  --sysled-pin INT                系统 LED GPIO（0-48，或 -1 禁用）
  --cpufreq INT                   CPU 频率 MHz（400-1200）
  --ramfreq {400|800|1066|1200}   DRAM 速率 MT/s
  --ddrparam NAME                 DDR 参数（从内置列表选择之一或自定义）
  --baudrate {57600|115200}       串口速率（默认 115200）
  --model STRING                  设备型号/版型（可选，会写入 failsafe sysinfo 兜底）
  --board-name STRING             设备名称/代号（可选，默认同 --model）
  --yes                           跳过交互确认
  -h, --help                      显示帮助

示例（非交互）:
  ./build.sh \
    --flash NMBM \
    --mtdparts "512k(u-boot),512k(u-boot-env),512k(factory),-(firmware)" \
    --model "FCJ_G-AX1800-F" \
    --kernel-offset 0x180000 \
    --reset-pin 7 \
    --sysled-pin 13 \
    --cpufreq 1000 \
    --ramfreq 1200 \
    --ddrparam DDR3-512MiB \
    --baudrate 115200 \
    --yes
EOF
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
      --sysled-pin) SYSLED_PIN="$2"; shift 2;;
      --cpufreq) CPUFREQ="$2"; shift 2;;
      --ramfreq) RAMFREQ="$2"; shift 2;;
      --ddrparam) DDRPARM="$2"; shift 2;;
      --baudrate) BAUDRATE="$2"; shift 2;;
      --model) MODEL="$2"; shift 2;;
      --board-name) BOARD_NAME="$2"; shift 2;;
      --yes) YES="1"; shift;;
      -h|--help) print_usage; exit 0;;
      *) echo "未知参数: $1"; print_usage; exit 1;;
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
      # -e: readline（支持方向键/历史）; -i: 预填默认值
      read -e -r -p "${prompt} [默认: ${default_val}] > " -i "${default_val}" var || true
    else
      read -r -p "${prompt} [默认: ${default_val}] > " var || true
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
    read -e -r -p "选择序号 (输入数字 1-${#items[@]}) > " idx || true
  else
    read -r -p "选择序号 (输入数字 1-${#items[@]}) > " idx || true
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
    read -e -r -p "选择序号 (默认: ${default_val}) > " idx || true
  else
    read -r -p "选择序号 (默认: ${default_val}) > " idx || true
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
  # MTDPARTS 基本校验：允许用户重命名/新增分区，但必须包含 firmware 分区
  # 注意：脚本期望传入的是“不含设备前缀”的分区串（不要包含 spi0.0: 之类前缀）
  if [[ -z "${MTDPARTS}" ]]; then
    echo "错误: 未提供 MTD 分区表，示例：${DEFAULT_MTDPARTS}"; exit 1
  fi
  if echo -n "${MTDPARTS}" | grep -Eq '^[^,()]+:'; then
    echo "错误: MTD 分区表请勿包含设备前缀（例如 spi0.0:），应形如：${DEFAULT_MTDPARTS}"; exit 1
  fi
  if ! echo -n "${MTDPARTS}" | grep -q "(firmware)"; then
    echo "错误: MTD 分区表必须包含名为 firmware 的分区，例如：${DEFAULT_MTDPARTS}"; exit 1
  fi
  if ! echo -n "${MTDPARTS}" | grep -Eq '\([^()]+\)'; then
    echo "错误: MTD 分区表格式不合法，示例：${DEFAULT_MTDPARTS}"; exit 1
  fi
  # 若提供了独立分区大小，进行基本合法性校验
  for tok in "${UBOOT_SIZE}" "${UBOOT_ENV_SIZE}" "${FACTORY_SIZE}"; do
    if [[ -n "$tok" ]] && ! is_size_token "$tok"; then
      echo "错误: 分区大小需为数字+单位（k/m），例如 512k、1m"; exit 1
    fi
  done
  # FLASH 类型
  case "${FLASH}" in
    NOR|NAND|NMBM) :;;
    *) echo "错误: 请选择 FLASH 类型 NOR/NAND/NMBM"; exit 1;;
  esac
  # KERNEL_OFFSET 允许十六进制或十进制
  if [[ -z "${KERNEL_OFFSET}" ]] || ! [[ "${KERNEL_OFFSET}" =~ ^(0x[0-9a-fA-F]+|[0-9]+)$ ]]; then
    echo "错误: kernel-offset 需为十六进制(如 0x60000)或十进制"; exit 1
  fi
  # GPIO 范围或 -1
  for p in RESET_PIN SYSLED_PIN; do
    local val="${!p}"
    if ! [[ "${val}" =~ ^-?[0-9]+$ ]]; then
      echo "错误: ${p} 必须是整数（-1 或 0-48）"; exit 1
    fi
    if (( val != -1 && (val < 0 || val > 48) )); then
      echo "错误: ${p} 超出范围（-1 或 0-48）"; exit 1
    fi
  done
  # CPU 频率
  if [[ -z "${CPUFREQ}" ]] || ! [[ "${CPUFREQ}" =~ ^[0-9]+$ ]] || (( CPUFREQ < 400 || CPUFREQ > 1200 )); then
    echo "错误: cpufreq 必须是 400-1200 的整数 MHz"; exit 1
  fi
  # RAM 频率
  case "${RAMFREQ}" in
    400|800|1066|1200) :;;
    *) echo "错误: ramfreq 仅支持 400/800/1066/1200"; exit 1;;
  esac
  # 波特率
  case "${BAUDRATE}" in
    57600|115200) :;;
    *) echo "错误: baudrate 仅支持 57600 或 115200"; exit 1;;
  esac
}

interactive() {
  # FLASH 类型
  FLASH=$(select_with_default "选择闪存类型:" "NMBM" NOR NAND NMBM)
  # 分区表：允许用户重命名分区或新增自定义分区
  # 说明：这里输入的是“不含设备前缀”的分区串；且必须包含 (firmware) 分区
  MTDPARTS=$(ask "输入 MTD 分区表（不含设备前缀，需包含 firmware 分区）" "${DEFAULT_MTDPARTS}")
  # kernel offset，不同闪存可能不同，这里仅做示例提示
  local example_offset="0x180000"
  KERNEL_OFFSET=$(ask "输入内核偏移 (示例 ${example_offset})" "${example_offset}")
  # GPIO
  RESET_PIN=$(ask "复位按钮 GPIO (0-48，-1 禁用)" "-1")
  SYSLED_PIN=$(ask "系统 LED GPIO (0-48，-1 禁用)" "-1")
  # CPU 频率
  local cpusel=$(select_with_default "选择 CPU 频率 (MHz)：" "1000" 880 1000 1100 1200)
  CPUFREQ="${cpusel}"
  # RAM 频率
  local ramsel=$(select_with_default "选择 DRAM 速率 (MT/s)：" "1200" 400 800 1066 1200)
  RAMFREQ="${ramsel}"
  # DDR 参数
  echo "选择 DDR 初始化参数（或留空自定义输入）："
  local ddrsel=$(select_from "内置列表：" \
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
    DDRPARM=$(ask "自定义 DDR 参数（大小写需与 customize.sh 中 case 项一致）" "DDR3-256MiB")
  else
    DDRPARM="${ddrsel}"
  fi
  # 波特率
  local brsel=$(select_with_default "选择串口波特率：" "115200" 57600 115200)
  BAUDRATE="${brsel}"

  # 版型/名称（可选，用于 failsafe sysinfo 的兜底显示）
  MODEL=$(ask "设备型号/版型（可选，留空则不写入 failsafe 兜底配置）" "")
  if [[ -n "${MODEL}" ]]; then
    BOARD_NAME=$(ask "设备名称/代号（可选，默认同上）" "${MODEL}")
  else
    BOARD_NAME=""
  fi
}

summary() {
  cat <<EOF
将执行：
  ./customize.sh '${FLASH}' '${MTDPARTS}' '${KERNEL_OFFSET}' '${RESET_PIN}' \
  '${SYSLED_PIN}' '${CPUFREQ}' '${RAMFREQ}' '${DDRPARM}' '${BAUDRATE}' '${MODEL}' '${BOARD_NAME}'
EOF
}

main() {
  parse_args "$@"
  # 如未直接提供 mtdparts，但提供了各分区大小，则拼接
  if [[ -z "${MTDPARTS}" ]] && { [[ -n "${UBOOT_SIZE}" ]] || [[ -n "${UBOOT_ENV_SIZE}" ]] || [[ -n "${FACTORY_SIZE}" ]]; }; then
    MTDPARTS=$(build_mtdparts)
  fi
  if [[ -z "${FLASH}" || -z "${MTDPARTS}" || -z "${KERNEL_OFFSET}" || -z "${CPUFREQ}" || -z "${RAMFREQ}" || -z "${DDRPARM}" ]]; then
    echo "进入交互式配置..."
    interactive
  fi
  validate

  # defaults
  if [[ -z "${BOARD_NAME}" ]] && [[ -n "${MODEL}" ]]; then
    BOARD_NAME="${MODEL}"
  fi

  summary
  if [[ "${YES}" != "1" ]]; then
    if [[ -t 0 ]]; then
      read -e -r -p "确认执行？[y/N] " confirm || true
    else
      read -r -p "确认执行？[y/N] " confirm || true
    fi
    if [[ "${confirm,,}" != "y" ]]; then
      echo "已取消。"; exit 0
    fi
  fi
  ./customize.sh "${FLASH}" "${MTDPARTS}" "${KERNEL_OFFSET}" "${RESET_PIN}" \
                 "${SYSLED_PIN}" "${CPUFREQ}" "${RAMFREQ}" "${DDRPARM}" "${BAUDRATE}" "${MODEL}" "${BOARD_NAME}"
  echo "构建完成。若成功，产物位于 ./archive/。"
}

main "$@"
