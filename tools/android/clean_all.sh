#!/bin/bash
# ============================================================
# tools/android/clean_all.sh —— 清掉 tools/android 下所有**可再生成**的产物
#
# 默认把 build_openssl_android.sh 的产物（3rdparty/openssl/）也一起删掉；
# 下次做 native 构建前，先重新跑一次那个脚本即可（它还会顺手重建
# build/openssl/ 这个源码外构建目录）。
#
# 用法：
#   bash tools/android/clean_all.sh                 # 全清（含预编译 OpenSSL）
#   bash tools/android/clean_all.sh --keep-openssl  # 保留 3rdparty/openssl/
#   bash tools/android/clean_all.sh -n              # 只列出会删什么，不真删
#
# ⚠️ 不删：源码、gradle wrapper、local.properties、.idea、
#    以及仓库根的 OpenSSL **源码**（3rdparty/openssl-3.5.8）。
# ============================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

usage() {
  cat <<'EOF'
usage: clean_all.sh [--keep-openssl] [-n|--dry-run]

  (default)        remove all build outputs AND the prebuilt OpenSSL libs
  --keep-openssl   keep 3rdparty/openssl/<abi>/ (skip build_openssl_android.sh)
  -n, --dry-run    print what would be removed, delete nothing
  -h, --help       show this help

Removed: build/
         app/build/
         app/.cxx/
         app/.externalNativeBuild/
         .gradle/
         .kotlin/
         captures/
         3rdparty/openssl/            (unless --keep-openssl)

Kept: sources, gradle wrapper, local.properties, .idea, OpenSSL source tree.
EOF
}

KEEP_OPENSSL=0
DRY_RUN=0
for arg in "$@"; do
  case "$arg" in
    --keep-openssl) KEEP_OPENSSL=1 ;;
    -n|--dry-run)   DRY_RUN=1 ;;
    -h|--help)      usage; exit 0 ;;
    *) echo "unknown option: $arg" >&2; usage >&2; exit 2 ;;
  esac
done

# 防呆：确认脚本就在 tools/android 里。下面的路径都是相对的，跑错目录再配合
# rm -rf 就是灾难 —— 两个标志性文件缺一个就拒绝执行。
[ -f "$HERE/app/build.gradle.kts" ] && [ -f "$HERE/build_openssl_android.sh" ] || {
  echo "refusing to run: $HERE does not look like tools/android" >&2
  exit 1
}

TARGETS=(
  "build"
  "app/build"
  "app/.cxx"
  "app/.externalNativeBuild"
  ".gradle"
  ".kotlin"
  "captures"
)
[ "$KEEP_OPENSSL" -eq 1 ] || TARGETS+=("3rdparty/openssl")

echo "clean: $HERE"
[ "$DRY_RUN" -eq 1 ] && echo "(dry run — nothing will be deleted)"
echo

total_kb=0
removed_any=0
for t in "${TARGETS[@]}"; do
  [ -e "$t" ] || continue
  human="$(du -sh "$t" 2>/dev/null | awk '{print $1}')"
  kb="$(du -sk "$t" 2>/dev/null | awk '{print $1}')"
  [ -n "$kb" ] || kb=0
  total_kb=$((total_kb + kb))
  removed_any=1
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "would remove  $t  ($human)"
  else
    rm -rf "$t"
    echo "removed       $t  ($human)"
  fi
done

if [ "$DRY_RUN" -eq 1 ]; then
  echo
  echo "total: $(awk -v k="$total_kb" 'BEGIN{printf "%.1f MB", k/1024}') would be freed"
  exit 0
fi

if [ "$removed_any" -eq 0 ]; then
  echo "nothing to clean — already clean (29 MB is the source + prebuilt libs)"
else
  echo
  echo "freed: $(awk -v k="$total_kb" 'BEGIN{printf "%.1f MB", k/1024}')"
fi

if [ "$KEEP_OPENSSL" -eq 0 ]; then
  # 只有真删了 OpenSSL 目录才提醒；--keep-openssl 时不必。
  if [ ! -d "3rdparty/openssl" ]; then
    echo
    echo "NOTE: 3rdparty/openssl/ was removed."
    echo "      Run 'bash tools/android/build_openssl_android.sh' before the next"
    echo "      Gradle/native build (it recreates include/ and lib/ for arm64-v8a)."
  fi
fi
