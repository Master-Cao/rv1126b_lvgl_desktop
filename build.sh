#!/bin/bash
#
# rv1126b_desktop 交叉编译脚本
#
# 默认使用 /opt/atk-dlrv1126b-toolchain 工具链；
# 板端运行时依赖：
#   /opt/rv1126b_desktop/models/ppocrv4_det.rknn   (OCR 检测)
#   /opt/rv1126b_desktop/models/ppocrv4_rec.rknn   (OCR 识别)
#   /opt/rv1126b_desktop/models/yolov8.rknn        (目标检测，COCO 80 类)
#   /opt/rv1126b_desktop/models/yolov8_seg.rknn     (实例分割，COCO 80 类)
#   rootfs/sysroot 提供 librknnrt.so / libopencv_core / libopencv_imgproc / libopencv_imgcodecs
#
# 用法：
#   ./build.sh                # Release 增量构建
#   ./build.sh -c             # 清理后重新构建
#   ./build.sh -d             # Debug 构建
#   ./build.sh -j 8           # 8 路并行
#   TOOLCHAIN_DIR=/path ./build.sh    # 指定工具链根目录
#

set -e

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
TARGET=rv1126b_desktop

BUILD_TYPE=Release
JOBS=$(nproc 2>/dev/null || echo 4)
CLEAN=0
DO_INSTALL=0
TOOLCHAIN_DIR=${TOOLCHAIN_DIR:-/opt/atk-dlrv1126b-toolchain}

usage() {
    cat <<EOF
Usage: $0 [-c] [-d] [-j N] [-i] [-t /path/to/toolchain]
  -c           clean build directory before building
  -d           build with Debug type (default: Release)
  -j N         parallel make jobs (default: nproc)
  -i           run "make install" after build
  -t <path>    toolchain root (default: /opt/atk-dlrv1126b-toolchain)
  -h           show this help
EOF
    exit 1
}

while getopts "cdj:it:h" opt; do
    case $opt in
        c) CLEAN=1 ;;
        d) BUILD_TYPE=Debug ;;
        j) JOBS=$OPTARG ;;
        i) DO_INSTALL=1 ;;
        t) TOOLCHAIN_DIR=$OPTARG ;;
        h|*) usage ;;
    esac
done

GCC_PREFIX=${TOOLCHAIN_DIR}/bin/aarch64-buildroot-linux-gnu
if [[ ! -x ${GCC_PREFIX}-gcc ]]; then
    echo -e "\e[91m[ERROR] cross compiler not found: ${GCC_PREFIX}-gcc\e[0m"
    echo "        export TOOLCHAIN_DIR=/path/to/atk-dlrv1126b-toolchain  or  use -t <path>"
    exit 1
fi

export CC=${GCC_PREFIX}-gcc
export CXX=${GCC_PREFIX}-g++

BUILD_DIR=${ROOT_DIR}/build/${BUILD_TYPE}

echo "==================================="
echo "ROOT_DIR     = ${ROOT_DIR}"
echo "BUILD_DIR    = ${BUILD_DIR}"
echo "BUILD_TYPE   = ${BUILD_TYPE}"
echo "TARGET       = ${TARGET}"
echo "TOOLCHAIN    = ${TOOLCHAIN_DIR}"
echo "CC           = ${CC}"
echo "CXX          = ${CXX}"
echo "JOBS         = ${JOBS}"
echo "==================================="

if [[ ${CLEAN} -eq 1 && -d ${BUILD_DIR} ]]; then
    echo "[clean] removing ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake "${ROOT_DIR}" \
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE}

make -j${JOBS}

if [[ ${DO_INSTALL} -eq 1 ]]; then
    make install
fi

BIN_PATH=${BUILD_DIR}/${TARGET}
PLATE_TEST_PATH=${BUILD_DIR}/plate_test
if [[ -f ${BIN_PATH} ]]; then
    echo ""
    echo -e "\e[92m[OK] build finished: ${BIN_PATH}\e[0m"
    if [[ -f ${PLATE_TEST_PATH} ]]; then
        echo -e "\e[92m[OK] plate_test:    ${PLATE_TEST_PATH}\e[0m"
    fi
    echo ""
    echo "板端部署示例："
    echo "  ssh root@<board> 'mkdir -p /opt/rv1126b_desktop/models'"
    echo "  scp ${BIN_PATH}                        root@<board>:/opt/rv1126b_desktop/"
    echo "  scp ${ROOT_DIR}/models/ppocrv4_det.rknn  root@<board>:/opt/rv1126b_desktop/models/"
    echo "  scp ${ROOT_DIR}/models/ppocrv4_rec.rknn  root@<board>:/opt/rv1126b_desktop/models/"
    echo "  scp ${ROOT_DIR}/models/yolov8.rknn       root@<board>:/opt/rv1126b_desktop/models/"
    if [[ -f ${PLATE_TEST_PATH} ]]; then
        echo "  scp ${PLATE_TEST_PATH}                   root@<board>:/opt/rv1126b_desktop/"
        echo "  scp ${ROOT_DIR}/scripts/plate_board_test.sh root@<board>:/opt/rv1126b_desktop/"
    fi
    echo ""
    echo "或在板端使用脚本一次部署所有模型："
    echo "  ${ROOT_DIR}/scripts/install_board.sh -m ${ROOT_DIR}/models -s"
    echo ""
    echo "运行（可选环境变量）："
    echo "  export OCR_DET_MODEL=/opt/rv1126b_desktop/models/ppocrv4_det.rknn"
    echo "  export OCR_REC_MODEL=/opt/rv1126b_desktop/models/ppocrv4_rec.rknn"
    echo "  export DETECT_MODEL=/opt/rv1126b_desktop/models/yolov8.rknn"
    echo "  /opt/rv1126b_desktop/${TARGET}"
else
    echo -e "\e[91m[ERROR] target binary not found: ${BIN_PATH}\e[0m"
    exit 1
fi
