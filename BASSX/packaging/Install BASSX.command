#!/bin/bash
# BASSX - installazione macOS (utente corrente, nessuna password).
# Doppio click. Se macOS lo blocca: tasto destro > Apri.
set -euo pipefail
cd "$(dirname "$0")"

VST3="$HOME/Library/Audio/Plug-Ins/VST3"
AU="$HOME/Library/Audio/Plug-Ins/Components"
APPS="$HOME/Applications"
mkdir -p "$VST3" "$AU" "$APPS"

install_one () {   # sorgente, destinazione
  [ -e "$1" ] || return 0
  rm -rf "$2/$(basename "$1")"
  cp -R "$1" "$2/"
  # Plugin non notarizzato: si toglie la quarantena e si rifirma ad-hoc.
  xattr -dr com.apple.quarantine "$2/$(basename "$1")" 2>/dev/null || true
  codesign --force --deep -s - "$2/$(basename "$1")" >/dev/null 2>&1 || true
  echo "  OK  $2/$(basename "$1")"
}

echo "Installo BASSX..."
install_one "BASSX.vst3" "$VST3"
install_one "BASSX.component" "$AU"
install_one "BASSX.app" "$APPS"

# Forza la riscansione degli Audio Unit.
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true

echo
echo "Fatto. Riapri la DAW e riscansiona i plug-in."
read -n 1 -s -r -p "Premi un tasto per chiudere."
