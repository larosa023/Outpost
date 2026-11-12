#!/usr/bin/env bash
# Cross-compile Outpost.exe from Linux/macOS with Zig (pip install ziglang, or a zig binary on PATH).
set -euo pipefail
cd "$(dirname "$0")"
./sync-version.sh
if [[ -z "${ZIG:-}" ]]; then
  if command -v zig >/dev/null 2>&1; then ZIG=zig; else ZIG="python3 -m ziglang"; fi
fi
mkdir -p dist build
# miniz (C) once
for f in miniz miniz_tdef miniz_tinfl miniz_zip; do
  if [[ ! -f build/$f.o || src/miniz/$f.c -nt build/$f.o ]]; then
    $ZIG cc -target x86_64-windows-gnu -O2 -DMINIZ_NO_TIME -c src/miniz/$f.c -o build/$f.o
  fi
done
$ZIG c++ -target x86_64-windows-gnu -std=c++17 -O2 -s -municode \
  -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
  -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers \
  ${EXTRA_FLAGS:-} src/*.cpp build/miniz*.o res/outpost.rc -o "${OUT:-dist/Outpost.exe}" \
  -Wl,--subsystem,windows \
  -ld2d1 -ldwrite -lole32 -loleaut32 -luuid -ldwmapi -lshcore -lshell32 -luser32 -lgdi32 -ladvapi32 -lshlwapi \
  -lwindowscodecs -lcomctl32 -luxtheme -lwinhttp -lbcrypt -lversion -lcomdlg32 -lws2_32 -liphlpapi -lpsapi
rm -f dist/Outpost.pdb
ls -la "${OUT:-dist/Outpost.exe}"
