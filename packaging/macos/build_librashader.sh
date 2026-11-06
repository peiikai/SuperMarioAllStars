#!/bin/sh
# Builds librashader (the slang shader presets' runtime) for the macOS app from
# source, universal (Apple silicon and Intel), into
# third_party/librashader/librashader.dylib: v0.12.0, the version of the
# vendored librashader.h and librashader.dll. (Its released macOS binaries are
# Apple silicon only.) Needs Rust through rustup. Without the library the app
# runs on its built-in filters.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/third_party/librashader/librashader.dylib"
if [ -f "$out" ]; then echo "librashader already in $out"; exit 0; fi
src="$root/third_party/librashader-src"
if [ ! -d "$src" ]; then
  git -c advice.detachedHead=false clone --depth 1 --branch librashader-v0.12.0 \
    https://github.com/SnowflakePowered/librashader.git "$src"
fi
commit=$(git -C "$src" rev-parse HEAD)
if [ "$commit" != "87e8a97b50516d997defeaa168173dcd185d4022" ]; then
  echo "librashader source is at an unexpected commit ($commit)" >&2
  exit 1
fi
cd "$src"
rustup target add aarch64-apple-darwin x86_64-apple-darwin
for t in aarch64-apple-darwin x86_64-apple-darwin; do
  MACOSX_DEPLOYMENT_TARGET=11.0 cargo run -p librashader-build-script -- --profile optimized --target "$t"
done
lipo -create target/aarch64-apple-darwin/optimized/librashader.dylib \
  target/x86_64-apple-darwin/optimized/librashader.dylib -output "$out"
# (the app loads it by full path, then librashader_ld.h's dlopen("librashader.dylib") finds it by this name)
install_name_tool -id @rpath/librashader.dylib "$out"
codesign --force --sign - "$out"
lipo -info "$out"
