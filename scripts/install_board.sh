#!/bin/sh
#
# 板端部署脚本（在 RV1126B 开发板上执行）
#
# 功能：
#   1. 把 OCR / YOLOv8 模型复制到 /opt/rv1126b_desktop/models/
#   2. 把 init.d 自启脚本复制到 /etc/init.d/S10rv1126b_desktop 并赋可执行权限
#   3. 校验 librknnrt.so / libopencv_*.so / 可执行文件是否就位
#   4. 可选立即启动服务（-s 参数）
#
# 用法：
#   ./scripts/install_board.sh                         # 模型源默认 ./models/
#   ./scripts/install_board.sh -m /tmp/rknn_models     # 指定模型源目录
#   ./scripts/install_board.sh -s                      # 安装后立即启动
#   ./scripts/install_board.sh -m ./models -s          # 同上
#
# ./models/ 期望放置以下三个 RKNN 模型：
#   ppocrv4_det.rknn   PP-OCR 检测
#   ppocrv4_rec.rknn   PP-OCR 识别
#   yolov8.rknn        YOLOv8 目标检测（COCO 80 类）
#

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)

MODEL_SRC_DIR="${ROOT_DIR}/models"
MODEL_DST_DIR="/opt/rv1126b_desktop/models"
INIT_SCRIPT_NAME="S10rv1126b_desktop"
INIT_SCRIPT_SRC="${SCRIPT_DIR}/${INIT_SCRIPT_NAME}"
INIT_SCRIPT_DST="/etc/init.d/${INIT_SCRIPT_NAME}"
GPIO_SCRIPT_NAME="S09gpio_init"
GPIO_SCRIPT_SRC="${SCRIPT_DIR}/${GPIO_SCRIPT_NAME}"
GPIO_SCRIPT_DST="/etc/init.d/${GPIO_SCRIPT_NAME}"
APP_NAME="rv1126b_desktop"
START_AFTER_INSTALL=0
REMOVE_LEGACY=1

OCR_DET_MODEL="ppocrv4_det.rknn"
OCR_REC_MODEL="ppocrv4_rec.rknn"
YOLOV8_MODEL="yolov8.rknn"

usage() {
    cat <<EOF
Usage: $0 [-m MODEL_DIR] [-s] [-k]
  -m MODEL_DIR   model source dir (default: ${ROOT_DIR}/models)
                 must contain ${OCR_DET_MODEL}, ${OCR_REC_MODEL} and ${YOLOV8_MODEL}
  -s             start service immediately after install
  -k             keep legacy /etc/init.d/S10rv1126_desktop (default: remove)
  -h             show this help
EOF
    exit 1
}

while getopts "m:skh" opt; do
    case $opt in
        m) MODEL_SRC_DIR=$OPTARG ;;
        s) START_AFTER_INSTALL=1 ;;
        k) REMOVE_LEGACY=0 ;;
        h|*) usage ;;
    esac
done

red()    { printf '\033[91m%s\033[0m\n' "$1"; }
green()  { printf '\033[92m%s\033[0m\n' "$1"; }
yellow() { printf '\033[93m%s\033[0m\n' "$1"; }

if [ "$(id -u)" != "0" ]; then
    red "[ERROR] please run as root"
    exit 1
fi

echo "==================================="
echo "ROOT_DIR        = ${ROOT_DIR}"
echo "MODEL_SRC_DIR   = ${MODEL_SRC_DIR}"
echo "MODEL_DST_DIR   = ${MODEL_DST_DIR}"
echo "INIT_SCRIPT_SRC = ${INIT_SCRIPT_SRC}"
echo "INIT_SCRIPT_DST = ${INIT_SCRIPT_DST}"
echo "==================================="

# ---------- step 1: 模型 ----------
echo ""
echo "[1/4] deploying RKNN models..."
missing=""
for f in "${OCR_DET_MODEL}" "${OCR_REC_MODEL}" "${YOLOV8_MODEL}"; do
    if [ ! -f "${MODEL_SRC_DIR}/${f}" ]; then
        missing="${missing} ${f}"
    fi
done
if [ -n "${missing}" ]; then
    red "[ERROR] missing model files in ${MODEL_SRC_DIR}:${missing}"
    echo "        copy the .rknn files into ${MODEL_SRC_DIR} or pass -m <dir>"
    echo "        expected: ${OCR_DET_MODEL} / ${OCR_REC_MODEL} / ${YOLOV8_MODEL}"
    exit 1
fi

mkdir -p "${MODEL_DST_DIR}"
cp -f "${MODEL_SRC_DIR}/${OCR_DET_MODEL}" "${MODEL_DST_DIR}/${OCR_DET_MODEL}"
cp -f "${MODEL_SRC_DIR}/${OCR_REC_MODEL}" "${MODEL_DST_DIR}/${OCR_REC_MODEL}"
cp -f "${MODEL_SRC_DIR}/${YOLOV8_MODEL}"  "${MODEL_DST_DIR}/${YOLOV8_MODEL}"
green "      models -> ${MODEL_DST_DIR}/"
ls -lh "${MODEL_DST_DIR}/${OCR_DET_MODEL}" \
       "${MODEL_DST_DIR}/${OCR_REC_MODEL}" \
       "${MODEL_DST_DIR}/${YOLOV8_MODEL}"

# ---------- step 2: init.d 脚本 ----------
echo ""
echo "[2/4] installing init.d script..."
if [ ! -f "${INIT_SCRIPT_SRC}" ]; then
    red "[ERROR] init.d script not found: ${INIT_SCRIPT_SRC}"
    exit 1
fi
cp -f "${INIT_SCRIPT_SRC}" "${INIT_SCRIPT_DST}"
chmod +x "${INIT_SCRIPT_DST}"
green "      ${INIT_SCRIPT_DST} installed"

# GPIO 开机初始化脚本（S09，早于桌面 S10 运行）
if [ -f "${GPIO_SCRIPT_SRC}" ]; then
    cp -f "${GPIO_SCRIPT_SRC}" "${GPIO_SCRIPT_DST}"
    chmod +x "${GPIO_SCRIPT_DST}"
    green "      ${GPIO_SCRIPT_DST} installed"
else
    yellow "      [WARN] GPIO init script not found: ${GPIO_SCRIPT_SRC}"
fi

# 移除旧版自启脚本（用户原始名为 S10rv1126_desktop，注意没有 b）
if [ "${REMOVE_LEGACY}" = "1" ] && [ -f "/etc/init.d/S10rv1126_desktop" ]; then
    rm -f /etc/init.d/S10rv1126_desktop
    yellow "      removed legacy /etc/init.d/S10rv1126_desktop"
fi

# ---------- step 3: 运行环境校验 ----------
echo ""
echo "[3/4] runtime sanity check..."

# 可执行文件
APP_BIN=""
for candidate in \
    "${ROOT_DIR}/build/Release/${APP_NAME}" \
    "${ROOT_DIR}/build/Debug/${APP_NAME}" \
    "${ROOT_DIR}/build/${APP_NAME}"; do
    if [ -x "$candidate" ]; then
        APP_BIN=$candidate
        break
    fi
done

if [ -z "${APP_BIN}" ]; then
    yellow "      [WARN] ${APP_NAME} executable not found under ${ROOT_DIR}/build/"
    yellow "             cross-compile on host first (./build.sh) and sync to board"
else
    green "      app  = ${APP_BIN}"
fi

# 关键 .so
check_lib() {
    libname=$1
    if ldconfig -p 2>/dev/null | grep -q "$libname"; then
        green "      lib  = $libname (OK)"
    elif find /usr/lib /usr/local/lib /lib -maxdepth 3 -name "$libname*" 2>/dev/null | grep -q "."; then
        green "      lib  = $libname (OK, by find)"
    else
        red   "      lib  = $libname MISSING"
    fi
}

check_lib librknnrt.so
check_lib libopencv_core.so
check_lib libopencv_imgproc.so
check_lib libopencv_imgcodecs.so

# ---------- step 4: 启动 ----------
echo ""
if [ "${START_AFTER_INSTALL}" = "1" ]; then
    echo "[4/4] starting service..."
    "${INIT_SCRIPT_DST}" restart
else
    echo "[4/4] install done. start manually with:"
    echo "         ${INIT_SCRIPT_DST} start"
    echo "      or reboot."
fi

echo ""
green "[OK] install_board.sh finished"
