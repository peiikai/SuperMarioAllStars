#!/bin/sh
# Downloads the SDL2 source the macOS build compiles (third_party/SDL2-2.32.10-src),
# the same release and checksum as android/fetch_sdl.ps1. With it CMake builds SDL
# into the app (static, universal); without it an installed SDL2 is used.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
dest="$root/third_party/SDL2-2.32.10-src"
if [ -f "$dest/CMakeLists.txt" ]; then echo "SDL2 source already in $dest"; exit 0; fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/sdl.zip" https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.zip
hash=$(shasum -a 256 "$tmp/sdl.zip" | cut -d' ' -f1)
if [ "$hash" != "12b2dc2eb8f2836100a7916b5d394a0c82f1f7e32693f95f98305403af242f08" ]; then
  echo "SDL2 download has an unexpected checksum ($hash)" >&2
  exit 1
fi
mkdir "$tmp/x"
unzip -q "$tmp/sdl.zip" -d "$tmp/x"
mv "$tmp/x/SDL2-2.32.10" "$dest"
echo "SDL2 source ready in $dest"
