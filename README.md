# BASSX

Sintetizzatore per bassi VST3 / AU / Standalone (JUCE): modello del Bass Line
del 1982 con filtro ladder a diodi, accento e slide, dentro un motore wavetable
moderno con sequencer, arpeggiatore, sidechain e rack di effetti.

- **Manuale d'uso** — ogni controllo, pagina per pagina: [`docs/MANUALE.md`](docs/MANUALE.md)
- **Riferimento parametri** — tutti i 307 parametri con ID, range e default: [`docs/PARAMETRI.md`](docs/PARAMETRI.md)
- **Architettura** — sorgenti, percorso del segnale, thread, stato, test: [`docs/ARCHITETTURA.md`](docs/ARCHITETTURA.md)
- Panoramica tecnica, build e note DSP: [`BASSX/README.md`](BASSX/README.md)
- Novità: [`BASSX/CHANGELOG.md`](BASSX/CHANGELOG.md)
- Installazione dei binari: [`INSTALLATION README.txt`](INSTALLATION%20README.txt)
- Licenza: [`LICENSE`](LICENSE) — tutti i diritti riservati

## Build veloce

```bash
git clone https://github.com/<proprietario>/BASSX.git
git clone --branch 8.0.12 --depth 1 https://github.com/juce-framework/JUCE.git
cmake -S BASSX/BASSX -B build -DJUCE_PATH=JUCE -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Su Windows `build_windows.bat` fa build, test, pluginval e release in un colpo;
`mac_build_github.bat` avvia la build macOS su GitHub Actions.
