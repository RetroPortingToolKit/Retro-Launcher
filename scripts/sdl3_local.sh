# shellcheck shell=bash
# SDL3 for a local build, shared by packaging/linux/build-local-appimage.sh and
# scripts/build-local.sh. Source this file; it defines functions only.
#
#   sdl3_find <cache prefix>
#       0 when SDL3 is findable: a CMAKE_PREFIX_PATH that holds it, the cache
#       prefix (then prepended to CMAKE_PREFIX_PATH), or pkg-config's sdl3.
#   sdl3_build <cache prefix> <jobs> [<tag>]
#       Builds SDL3 <tag> (default $SDL_TAG, else release-3.2.16, the release
#       CI pins) from source into the cache prefix, shared, and prepends it to
#       CMAKE_PREFIX_PATH. The checkout and build tree sit beside the prefix.

sdl3_find() {
  local cache="$1" p
  if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
    local IFS=':'
    for p in ${CMAKE_PREFIX_PATH}; do
      if [[ -f "${p}/lib/cmake/SDL3/SDL3Config.cmake" || -f "${p}/lib64/cmake/SDL3/SDL3Config.cmake" ]]; then
        return 0
      fi
    done
  fi
  if [[ -f "${cache}/lib/cmake/SDL3/SDL3Config.cmake" || -f "${cache}/lib64/cmake/SDL3/SDL3Config.cmake" ]]; then
    export CMAKE_PREFIX_PATH="${cache}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
    return 0
  fi
  if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl3 2>/dev/null; then
    return 0
  fi
  return 1
}

sdl3_build() {
  local cache="$1" jobs="$2" tag="${3:-${SDL_TAG:-release-3.2.16}}"
  local src build
  src="$(dirname "${cache}")/SDL-src"
  build="$(dirname "${cache}")/sdl-build"
  command -v git >/dev/null || { echo "sdl3_build: git is required" >&2; return 1; }
  echo "==> Building SDL3 ${tag} -> ${cache}" >&2
  if [[ ! -d "${src}/.git" ]]; then
    rm -rf "${src}"
    git clone --depth 1 --branch "${tag}" https://github.com/libsdl-org/SDL.git "${src}" >&2
  else
    git -C "${src}" fetch --depth 1 origin "refs/tags/${tag}:refs/tags/${tag}" >&2 2>/dev/null || true
    git -C "${src}" checkout -q "${tag}"
  fi
  local gen=()
  command -v ninja >/dev/null && gen=(-G Ninja)
  cmake "${gen[@]}" -S "${src}" -B "${build}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${cache}" \
    -DSDL_SHARED=ON \
    -DSDL_STATIC=OFF >&2
  cmake --build "${build}" -j"${jobs}" >&2
  cmake --install "${build}" >&2
  export CMAKE_PREFIX_PATH="${cache}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
}
