#!/usr/bin/env bash
# Build native APK payload on Linux. Gradle only packages this staged payload.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ABI="${ANDROID_ABI:-arm64-v8a}"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/29.0.14206865}"
WORK="${RETCOMM_ANDROID_WORK:-$ROOT/.cache/android}"
SDL_SRC="${SDL_SOURCE_DIR:-$WORK/SDL}"
RUNTIME="${RETCOMM_RUNTIME_DIR:-$ROOT/third_party/Retro-Runtime}"
JOBS="${JOBS:-4}"
VERSION="${RETCOMM_VERSION:-0.1.1}"
case "$ABI" in
  arm64-v8a) OPENSSL_TARGET=android-arm64 ;;
  x86_64) OPENSSL_TARGET=android-x86_64 ;;
  *) echo "Unsupported Android ABI: $ABI" >&2; exit 1 ;;
esac
test -f "$RUNTIME/CMakeLists.txt"
test -f "$SDL_SRC/CMakeLists.txt"
test -f "$NDK/build/cmake/android.toolchain.cmake"
PREFIX="$WORK/$ABI/prefix"
mkdir -p "$WORK/downloads" "$WORK/$ABI" "$PREFIX"

download() {
  local name="$1" url="$2" sha="$3"
  if [[ ! -f "$WORK/downloads/$name" ]]; then
    curl --fail --location --retry 5 "$url" -o "$WORK/downloads/$name"
  fi
  echo "$sha  $WORK/downloads/$name" | sha256sum --check --status
}
download openssl-3.6.3.tar.gz \
  https://github.com/openssl/openssl/releases/download/openssl-3.6.3/openssl-3.6.3.tar.gz \
  243a86649cf6f23eeb6a2ff2456e09e5d77dd9018a54d3d96b0c6bdd6ba6c7f1
download curl-8.21.0.tar.xz https://curl.se/download/curl-8.21.0.tar.xz \
  aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6

if [[ ! -f "$PREFIX/lib/libssl.a" ]]; then
  tar -xf "$WORK/downloads/openssl-3.6.3.tar.gz" -C "$WORK/$ABI"
  (
    cd "$WORK/$ABI/openssl-3.6.3"
    export ANDROID_NDK_ROOT="$NDK"
    export PATH="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
    ./Configure "$OPENSSL_TARGET" -D__ANDROID_API__=30 no-shared no-tests \
      --prefix="$PREFIX" --libdir=lib
    make -j"$JOBS"
    make install_sw
  )
fi
COMMON=(-G Ninja "-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake"
  "-DANDROID_ABI=$ABI" -DANDROID_PLATFORM=30 -DANDROID_STL=c++_shared
  -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$PREFIX"
  "-DCMAKE_PREFIX_PATH=$PREFIX" "-DCMAKE_FIND_ROOT_PATH=$PREFIX"
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
if [[ ! -f "$WORK/curl-8.21.0/CMakeLists.txt" ]]; then
  tar -xf "$WORK/downloads/curl-8.21.0.tar.xz" -C "$WORK"
fi
# No default CA path: Android names its CA files by OpenSSL's old subject hash,
# which OpenSSL 3 never looks up. The launcher hands curl the system store
# itself (http_apply_tls_trust).
cmake -S "$WORK/curl-8.21.0" -B "$WORK/$ABI/curl-build" "${COMMON[@]}" \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
  -DCURL_USE_OPENSSL=ON "-DOPENSSL_ROOT_DIR=$PREFIX" -DOPENSSL_USE_STATIC_LIBS=ON \
  -DHTTP_ONLY=ON -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF \
  -DUSE_NGHTTP2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_ZLIB=OFF \
  -DCURL_CA_PATH=none -DCURL_CA_BUNDLE=none
cmake --build "$WORK/$ABI/curl-build" -j"$JOBS"
cmake --install "$WORK/$ABI/curl-build"
cmake -S "$SDL_SRC" -B "$WORK/$ABI/sdl-build" "${COMMON[@]}" \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF
cmake --build "$WORK/$ABI/sdl-build" -j"$JOBS"
cmake --install "$WORK/$ABI/sdl-build"

BUILD="$ROOT/build-android-$ABI"
cmake -S "$ROOT" -B "$BUILD" "${COMMON[@]}" \
  -DBUILD_TESTING=OFF -DRETCOMM_RELEASE_BUILD=ON \
  "-DRETCOMM_RUNTIME_DIR=$RUNTIME" "-DRETCOMM_VERSION=$VERSION" \
  "-DRETCOMM_COMMIT=$(git -C "$ROOT" rev-parse HEAD)" \
  "-DRETRO_RUNTIME_COMMIT=$(git -C "$RUNTIME" rev-parse HEAD)" \
  "-DRETCOMM_IMGUI_DIR=${RETCOMM_IMGUI_DIR:-$ROOT/../recomp-ui/src/third_party/imgui}" \
  "-DRETRO_RUNTIME_RECOMP_NET_DIR=${RETRO_RUNTIME_RECOMP_NET_DIR:-}"
cmake --build "$BUILD" --target retro-hub retro-core-runner -j"$JOBS"

STAGE="$ROOT/android/app/build/generated/payload"
mkdir -p "$STAGE/jniLibs/$ABI" "$STAGE/java/org/libsdl/app" "$STAGE/assets/retcomm/licenses"
cp "$BUILD/libmain.so" "$BUILD/libretro-core-runner.so" "$PREFIX/lib/libSDL3.so" "$STAGE/jniLibs/$ABI/"
case "$ABI" in
  arm64-v8a) TRIPLE=aarch64-linux-android ;;
  x86_64) TRIPLE=x86_64-linux-android ;;
esac
cp "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$TRIPLE/libc++_shared.so" "$STAGE/jniLibs/$ABI/"
cp "$SDL_SRC"/android-project/app/src/main/java/org/libsdl/app/*.java "$STAGE/java/org/libsdl/app/"
for dir in fonts platforms controllers setup; do
  cp -a "$ROOT/assets/$dir" "$STAGE/assets/retcomm/"
done
cp "$RUNTIME/LICENSE" "$STAGE/assets/retcomm/licenses/Retro-Runtime.txt"
cp "$ROOT/LICENSE" "$STAGE/assets/retcomm/licenses/Retro-Launcher.txt"
cp "$SDL_SRC/LICENSE.txt" "$STAGE/assets/retcomm/licenses/SDL3.txt"
cp "$WORK/curl-8.21.0/COPYING" "$STAGE/assets/retcomm/licenses/curl.txt"
cp "$WORK/$ABI/openssl-3.6.3/LICENSE.txt" "$STAGE/assets/retcomm/licenses/OpenSSL.txt"
IMGUI="$(sed -n 's/^RETCOMM_IMGUI_DIR:[A-Z]*=//p' "$BUILD/CMakeCache.txt")"
cp "$IMGUI/LICENSE.txt" "$STAGE/assets/retcomm/licenses/dear-imgui.txt"
cp "$ROOT/third_party/miniz/LICENSE" "$STAGE/assets/retcomm/licenses/miniz.txt"
cp "$ROOT/third_party/nlohmann/LICENSE.MIT" "$STAGE/assets/retcomm/licenses/nlohmann-json.txt"
sed -n '/^This software is available under 2 licenses/,$p' "$ROOT/third_party/stb/stb_image.h" \
  > "$STAGE/assets/retcomm/licenses/stb.txt"
python3 "$ROOT/scripts/android-manifest.py" "$STAGE/assets/retcomm/build-$ABI.json" \
  "$ROOT" "$RUNTIME" "$SDL_SRC" "$VERSION" "$ABI"
echo "Android native payload: $STAGE ($ABI)"
