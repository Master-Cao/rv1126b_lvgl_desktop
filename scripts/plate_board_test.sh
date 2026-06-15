#!/bin/bash
# 板端车牌检测离线测试脚本
#
# 用法（在 RV1126B 板子上）:
#   chmod +x plate_board_test.sh
#   ./plate_board_test.sh /path/to/plate.jpg
#   ./plate_board_test.sh -m /opt/rv1126b_desktop/models/plate_det.rknn -i test.jpg
#   ./plate_board_test.sh --all ./images
#
# 依赖:
#   - plate_test 可执行文件（交叉编译: ./build.sh 后得到 build/Release/plate_test）
#   - plate_det.rknn 或 plate.rknn
#   - 测试图片（jpg/png）

set -e

MODEL="${PLATE_DET_MODEL:-/opt/rv1126b_desktop/models/plate_det.rknn}"
ALT_MODEL="/opt/rv1126b_desktop/models/plate.rknn"
BIN="${PLATE_TEST_BIN:-/opt/rv1126b_desktop/plate_test}"
CONF="0.15"
OUT_DIR="."
RUN_ALL=0
IMAGE=""

usage() {
    cat <<EOF
用法: $0 [选项] [图片路径]

选项:
  -m, --model <path>   RKNN 模型路径
  -b, --bin <path>     plate_test 可执行文件路径
  -i, --image <path>   单张测试图
  --all <dir>          测试目录下全部 jpg/png
  --conf <float>       置信度阈值 (默认 0.15)
  -o, --outdir <dir>   结果图输出目录 (默认当前目录)
  -h, --help           帮助

环境变量:
  PLATE_DET_MODEL      模型路径
  PLATE_TEST_BIN       plate_test 路径

示例:
  $0 -i /tmp/plate.jpg
  $0 --all ./images
  scp build/Release/plate_test root@board:/opt/rv1126b_desktop/
  scp models/plate_det.rknn root@board:/opt/rv1126b_desktop/models/
EOF
}

resolve_model() {
    if [[ -n "$MODEL" && -f "$MODEL" ]]; then
        echo "$MODEL"
        return
    fi
    if [[ -f "$ALT_MODEL" ]]; then
        echo "$ALT_MODEL"
        return
    fi
    echo "$MODEL"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -m|--model) MODEL="$2"; shift 2 ;;
        -b|--bin) BIN="$2"; shift 2 ;;
        -i|--image) IMAGE="$2"; shift 2 ;;
        --all) RUN_ALL=1; IMAGE="$2"; shift 2 ;;
        --conf) CONF="$2"; shift 2 ;;
        -o|--outdir) OUT_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "未知选项: $1"; usage; exit 1 ;;
        *) IMAGE="$1"; shift ;;
    esac
done

MODEL="$(resolve_model)"

if [[ ! -x "$BIN" ]]; then
    echo "ERROR: 找不到 plate_test: $BIN"
    echo "请先交叉编译并拷贝到板端:"
    echo "  ./build.sh"
    echo "  scp build/Release/plate_test root@<board>:/opt/rv1126b_desktop/"
    exit 1
fi

if [[ ! -f "$MODEL" ]]; then
    echo "ERROR: 找不到模型: $MODEL"
    echo "请部署: scp plate_det.rknn root@<board>:/opt/rv1126b_desktop/models/"
    exit 1
fi

mkdir -p "$OUT_DIR"

run_one() {
    local img="$1"
    local base
    base="$(basename "$img")"
    base="${base%.*}"
    local out="${OUT_DIR}/${base}_plate_test.jpg"

    echo "----------------------------------------"
    echo "测试: $img"
    if "$BIN" -m "$MODEL" -i "$img" -o "$out" --conf "$CONF"; then
        echo "OK: $out"
        return 0
    fi
    local rc=$?
    if [[ $rc -eq 2 ]]; then
        echo "WARN: 未检出目标 (exit=2)"
        return 2
    fi
    echo "FAIL: exit=$rc"
    return $rc
}

echo "======== plate 板端测试 ========"
echo "BIN   : $BIN"
echo "MODEL : $MODEL"
echo "CONF  : $CONF"
echo "OUT   : $OUT_DIR"
echo "=============================="

if [[ $RUN_ALL -eq 1 ]]; then
    if [[ ! -d "$IMAGE" ]]; then
        echo "ERROR: 目录不存在: $IMAGE"
        exit 1
    fi
    ok=0
    fail=0
    empty=0
    shopt -s nullglob
    files=("$IMAGE"/*.{jpg,jpeg,png,JPG,JPEG,PNG})
    if [[ ${#files[@]} -eq 0 ]]; then
        echo "ERROR: 目录内无图片: $IMAGE"
        exit 1
    fi
    for f in "${files[@]}"; do
        set +e
        run_one "$f"
        rc=$?
        set -e
        if [[ $rc -eq 0 ]]; then
            ok=$((ok + 1))
        elif [[ $rc -eq 2 ]]; then
            empty=$((empty + 1))
        else
            fail=$((fail + 1))
        fi
    done
    echo "=============================="
    echo "完成: 检出 $ok / 未检出 $empty / 失败 $fail / 共 ${#files[@]}"
    exit 0
fi

if [[ -z "$IMAGE" ]]; then
    echo "ERROR: 请指定图片路径或 --all <目录>"
    usage
    exit 1
fi

if [[ ! -f "$IMAGE" ]]; then
    echo "ERROR: 图片不存在: $IMAGE"
    exit 1
fi

run_one "$IMAGE"
