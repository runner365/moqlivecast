#!/bin/bash
# ============================================================
# 把 vendored 的 OpenSSL 交叉编译成 Android 用的静态库。
#
# 用法：
#   bash tools/android/build_openssl_android.sh [abi]
#     默认 arm64-v8a。支持的 abi：arm64-v8a / armeabi-v7a / x86_64
#
# 产物：tools/android/3rdparty/openssl/<abi>/{include,lib}
#
# ⚠️ 为什么单独一个脚本、而不是塞进 CMake 的 ExternalProject：
#   Android 的 OpenSSL 必须用 **NDK 的 clang** 交叉编译，而 Gradle 调 CMake 时
#   那套 toolchain 是给调用方自己的代码用的。两层 toolchain 叠在一起 configure
#   会互相污染，失败信息还指向错的地方。这里一次生成、CMake 只负责 import ——
#   失败点单一，重跑也便宜。
#
# ⚠️ 顶层仓库的 CMakeLists 里那段 OpenSSL target 选择**对 Android 是错的**：
#   Android 在 CMake 眼里也是 UNIX，于是会落进 `linux-aarch64` 分支，链接出一个
#   **Linux 的 libcrypto**（Bionic 上跑不起来）。所以 Android 这条路不能复用
#   那段逻辑，必须走本脚本的 android-arm64。
# ============================================================
set -euo pipefail

ABI="${1:-arm64-v8a}"

case "$ABI" in
  arm64-v8a)   OPENSSL_TARGET=android-arm64  ;;
  armeabi-v7a) OPENSSL_TARGET=android-arm ;;
  x86_64)      OPENSSL_TARGET=android-x86_64 ;;
  *) echo "unsupported abi: $ABI" >&2; exit 1 ;;
esac

# minSdk 在 app/build.gradle.kts 里定死 24，这里必须一致 —— 否则 OpenSSL 编出来的
# 符号版本可能高于 app 声明的最低 API，装到老设备上会加载失败。
API="${ANDROID_API:-24}"

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC="$ROOT/3rdparty/openssl-3.5.8"
OUT="$HERE/3rdparty/openssl/$ABI"
BUILD="$HERE/build/openssl/$ABI"

[ -f "$SRC/Configure" ] || { echo "OpenSSL source not found at $SRC" >&2; exit 1; }

# ── 找 NDK ─────────────────────────────────────
# 优先级：显式环境变量 > ANDROID_HOME/ANDROID_SDK_ROOT 下的最新一版。
NDK="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}"
if [ -z "$NDK" ]; then
  SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
  NDK="$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1 || true)"
fi
[ -n "$NDK" ] && [ -d "$NDK" ] || {
  echo "NDK not found. Set ANDROID_NDK_ROOT, or install an NDK under \$ANDROID_HOME/ndk." >&2
  exit 1
}

# NDK 的预编译宿主目录（macOS 上 r30 只提供 darwin-x86_64，在 Apple Silicon 上
# 也能跑）。按存在的挑，别写死。
HOST_TAG=""
for t in darwin-x86_64 darwin-arm64 linux-x86_64 linux-aarch64; do
  if [ -d "$NDK/toolchains/llvm/prebuilt/$t" ]; then HOST_TAG="$t"; break; fi
done
[ -n "$HOST_TAG" ] || { echo "no usable prebuilt toolchain under $NDK" >&2; exit 1; }

TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
export PATH="$TOOLCHAIN/bin:$PATH"
export ANDROID_NDK_ROOT="$NDK"     # OpenSSL 的 Configurations/15-android.conf 找它

echo "abi           = $ABI"
echo "openssl目标   = $OPENSSL_TARGET"
echo "ndk           = $NDK ($HOST_TAG)"
echo "android api   = $API"
echo "out           = $OUT"
echo

command -v perl >/dev/null || { echo "perl not found (OpenSSL's Configure needs it)" >&2; exit 1; }

# ── configure（**源码外构建**）─────────────────
# 刻意不在 3rdparty/openssl-3.5.8 里就地编：那个目录同时被宿主机的
# ExternalProject 构建引用，留一堆 Makefile/目标文件进去会让人以为宿主构建脏了。
#
# 每次都**从干净目录开始**：OpenSSL 的 configure 会在构建目录里留下
# Makefile/configdata.pm，前一次失败后再跑会读到旧配置，报出来的错指向莫名其妙
# 的地方。这个构建一次几分钟，不值得为增量省那点时间。
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
perl "$SRC/Configure" "$OPENSSL_TARGET" \
     -D__ANDROID_API__="$API" \
     --prefix="$OUT" --openssldir="$OUT" --libdir=lib \
     no-shared \
     > "$BUILD/configure.log" 2>&1 || {
       echo "Configure failed — see $BUILD/configure.log" >&2
       tail -20 "$BUILD/configure.log" >&2
       exit 1
     }

# ── build + install ───────────────────────────
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
make -j"$JOBS" > "$BUILD/make.log" 2>&1 || {
  echo "make failed — see $BUILD/make.log" >&2
  tail -20 "$BUILD/make.log" >&2
  exit 1
}
make install_sw > "$BUILD/install.log" 2>&1 || {
  echo "make install_sw failed — see $BUILD/install.log" >&2
  exit 1
}

# ── 自检：产物真的是 Android 的 arm64 静态库吗？ ──
# 只看文件在不在是不够的 —— "编出了一个能在 macOS 上链接的 libcrypto" 正是
# 上面那段 target 选错时会发生的、且看起来完全正常的事。所以查一下架构。
LIBCRYPTO="$OUT/lib/libcrypto.a"
LIBSLL="$OUT/lib/libssl.a"
for f in "$LIBCRYPTO" "$LIBSLL"; do
  [ -f "$f" ] || { echo "expected artifact missing: $f" >&2; exit 1; }
done

if command -v lipo >/dev/null; then
  ARCH="$(lipo -info "$LIBCRYPTO" 2>&1 || true)"
  echo "libcrypto.a: $ARCH"
  case "$ABI" in
    arm64-v8a)   echo "$ARCH" | grep -q "arm64" || { echo "not an arm64 archive!" >&2; exit 1; } ;;
    armeabi-v7a) echo "$ARCH" | grep -q "arm"   || { echo "not an arm archive!"   >&2; exit 1; } ;;
  esac
fi

echo
echo "OK — OpenSSL for $ABI installed to $OUT"
echo "  headers: $OUT/include"
echo "  libs   : $OUT/lib/libssl.a $OUT/lib/libcrypto.a"
