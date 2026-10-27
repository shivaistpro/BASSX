#!/usr/bin/env bash
# BASSX - build macOS (universal arm64 + x86_64) con release versionata.
# Uso: ./build_macos.sh   (dalla cartella che contiene BASSX/)
# Output: releases/macos/BASSX-v<versione>-b<build>/  + .zip, registro in BUILDS.md
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$PWD"
VER="$(head -n1 BASSX/VERSION | tr -d '[:space:]')"
BN=$(( $(cat build_number.txt 2>/dev/null || echo 0) + 1 ))
REL="BASSX-v${VER}-b${BN}"
OUT="$ROOT/releases/macos/$REL"
JUCE_PATH="${JUCE_PATH:-$ROOT/../JUCE}"

echo "[1/5] Configuro (universal binary)..."
cmake -S BASSX -B build-mac -G Xcode -DJUCE_PATH="$JUCE_PATH" \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DBASSX_BUILD_GUI_RENDERER=ON >/dev/null

echo "[2/5] Compilo VST3 + AU + Standalone + test..."
cmake --build build-mac --config Release --target BASSX_VST3 BASSX_AU BASSX_Standalone BassXAudioTest -- -quiet

ART="build-mac/BASSX_artefacts/Release"
for b in "$ART/VST3/BASSX.vst3/Contents/MacOS/BASSX" "$ART/AU/BASSX.component/Contents/MacOS/BASSX"; do
  lipo -info "$b" | grep -q "x86_64 arm64\|arm64 x86_64" || { echo "ERRORE: $b non e' universal"; exit 1; }
done

echo "[3/5] Test audio..."
mkdir -p test-output
AUDIOTEST=OK
build-mac/BassXAudioTest_artefacts/Release/BassXAudioTest BASSX/tools/neural/reference_io.txt > test-output/audio_test_mac.txt || AUDIOTEST=FALLITO
grep verifiche test-output/audio_test_mac.txt || true

echo "[4/5] pluginval..."
PVRESULT=MANCANTE
PV="$(command -v pluginval || true)"
[ -z "$PV" ] && [ -x "$ROOT/../pluginval/pluginval.app/Contents/MacOS/pluginval" ] && PV="$ROOT/../pluginval/pluginval.app/Contents/MacOS/pluginval"
if [ -n "$PV" ]; then
  "$PV" --strictness-level 10 --validate-in-process --validate "$ART/VST3/BASSX.vst3" > test-output/pluginval_mac.txt 2>&1 && PVRESULT=OK || PVRESULT=FALLITO
fi

echo "[5/5] Release $REL..."
rm -rf "$OUT"; mkdir -p "$OUT"
cp -R "$ART/VST3/BASSX.vst3" "$ART/AU/BASSX.component" "$ART/Standalone/BASSX.app" "$OUT/"
cp "INSTALLATION README.txt" BASSX/CHANGELOG.md "BASSX/packaging/Install BASSX.command" "$OUT/"
chmod +x "$OUT/Install BASSX.command"
bash BASSX/packaging/make_pkg.sh "$OUT" "$VER" "$OUT/BASSX-$VER.pkg"
cp test-output/audio_test_mac.txt "$OUT/test_report.txt"
(cd "$ROOT/releases/macos" && ditto -c -k --keepParent "$REL" "$REL.zip")
rm -rf "$ROOT/releases/macos/latest" && cp -R "$OUT" "$ROOT/releases/macos/latest"
[ -f BUILDS.md ] || printf '# BASSX - registro build\n\n| Release | Piattaforma | Data | Test audio | pluginval |\n|---|---|---|---|---|\n' > BUILDS.md
echo "| $REL | macOS universal | $(date '+%d/%m/%Y %H:%M') | $AUDIOTEST | $PVRESULT |" >> BUILDS.md
echo "$BN" > build_number.txt
echo "Fatto: releases/macos/$REL.zip  (test audio $AUDIOTEST, pluginval $PVRESULT)"
