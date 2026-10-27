#!/bin/bash
# BASSX - crea l'installer macOS (.pkg) da VST3 + AU + app gia' compilati.
# Uso: packaging/make_pkg.sh <cartella con BASSX.vst3/.component/.app> <versione> <file .pkg di uscita>
# Funziona solo su macOS (pkgbuild/productbuild). Installa per tutto il sistema:
#   /Library/Audio/Plug-Ins/VST3, /Library/Audio/Plug-Ins/Components, /Applications
set -euo pipefail
SRC="$1"; VER="$2"; OUT="$3"
WORK="$(mktemp -d)"
ROOT="$WORK/root"
mkdir -p "$ROOT/Library/Audio/Plug-Ins/VST3" "$ROOT/Library/Audio/Plug-Ins/Components" "$ROOT/Applications" "$WORK/scripts"

cp -R "$SRC/BASSX.vst3"      "$ROOT/Library/Audio/Plug-Ins/VST3/"
cp -R "$SRC/BASSX.component" "$ROOT/Library/Audio/Plug-Ins/Components/"
[ -d "$SRC/BASSX.app" ] && cp -R "$SRC/BASSX.app" "$ROOT/Applications/"

# Firma ad-hoc: senza certificato Apple Developer e' il minimo perche' Apple Silicon li carichi.
for b in "$ROOT"/Library/Audio/Plug-Ins/*/BASSX.* "$ROOT"/Applications/BASSX.app; do
  [ -e "$b" ] && codesign --force --deep -s - "$b"
done

# Dopo l'installazione: forza la riscansione degli Audio Unit.
cat > "$WORK/scripts/postinstall" <<'POST'
#!/bin/bash
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
exit 0
POST
chmod +x "$WORK/scripts/postinstall"

# I bundle non devono essere "rilocabili", altrimenti l'installer aggiorna una
# eventuale copia altrove invece di scrivere nella cartella dei plug-in.
pkgbuild --analyze --root "$ROOT" "$WORK/components.plist"
i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$WORK/components.plist" >/dev/null 2>&1; do
  /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$WORK/components.plist" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$WORK/components.plist"
  i=$((i + 1))
done

pkgbuild --root "$ROOT" --component-plist "$WORK/components.plist" --scripts "$WORK/scripts" \
         --identifier com.bassxaudio.bassx --version "$VER" --install-location / "$WORK/BASSX-core.pkg"

cat > "$WORK/distribution.xml" <<DIST
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>BASSX $VER</title>
  <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
  <domains enable_localSystem="true"/>
  <choices-outline><line choice="default"><line choice="core"/></line></choices-outline>
  <choice id="default"/>
  <choice id="core" visible="false"><pkg-ref id="com.bassxaudio.bassx"/></choice>
  <pkg-ref id="com.bassxaudio.bassx" version="$VER">BASSX-core.pkg</pkg-ref>
</installer-gui-script>
DIST

productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" "$OUT"
rm -rf "$WORK"
echo "Creato $OUT"
