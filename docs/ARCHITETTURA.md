# BASSX — Architettura del codice

Come è organizzato il sorgente, dove passa il suono, come sono gestiti thread,
stato e test. Per l'uso dello strumento vedi [`MANUALE.md`](MANUALE.md); per
tutti i parametri [`PARAMETRI.md`](PARAMETRI.md).

## 1. Stack

- C++17, **JUCE 8** (testato con 8.0.12), CMake ≥ 3.22.
- Formati: **VST3, AU, Standalone** (AU solo su macOS).
- Codici plugin: manufacturer `Bsxa`, plugin `Bsx1`, sintetizzatore con ingresso
  MIDI e ingresso sidechain opzionale.
- Nessuna dipendenza esterna oltre JUCE. Le texture sono incorporate nel binario
  (`juce_add_binary_data`).

## 2. Mappa dei sorgenti (`BASSX/Source`)

| File | Ruolo |
|---|---|
| `PluginProcessor.h/.cpp` | Processore: parametri, voci, MIDI, sequencer, catena effetti, sidechain, stato, export WAV del loop, wavetable da campione. |
| `PluginEditor.h/.cpp` | Finestra: barra superiore, fila principale, tab delle pagine, tastiera, temi, ridimensionamento. |
| `Parameters.h/.cpp` | ID dei parametri (`ParamID::`), layout APVTS (307 parametri), `SynthParams` = copia POD letta una volta per blocco. |
| `Presets/PresetManager.*` | 36 preset di fabbrica, preset utente `.bassx`, impostazioni di sessione escluse dal cambio preset. |
| `DSP/Voice.h` | Una voce: 3 oscillatori, sub, rumore, FM/ring/sync, Neural Drive, due filtri con oversampling, VCA, accento, glide, vibrato, matrice. |
| `DSP/Oscillator.h` | Oscillatore wavetable con unison (fino a 16 voci), hard sync con BLEP, phase modulation, wavefolder; sub-oscillatore; rumore. |
| `DSP/WavetableBank.*` | 16 tabelle generate via IFFT all'avvio, 10 livelli di mipmap band-limited, fondamentale allineata in fase. |
| `DSP/SampleWavetable.h` | Campione audio → wavetable: rilevamento del periodo (YIN), 16 cicli, mipmap. |
| `DSP/Filters.h` | 16 modelli ZDF (ladder a diodi, Moog, SVF, comb, formant) con feedback risolto da Newton-Raphson. |
| `DSP/Envelope.h` | Inviluppo DAHDSR a curve esponenziali; circuito di accento. |
| `DSP/LFO.h` | LFO a 10 forme, sync alla posizione del brano, trigger, fade, lisciatura. |
| `DSP/ModMatrix.h` | Matrice a 16 slot, 18 sorgenti, 50 destinazioni, sorgente VIA. |
| `DSP/Sequencer.h` | Sequencer 16 passi × 8 pattern, arpeggiatore, motore di riproduzione e coda eventi a capacità fissa. |
| `DSP/NeuralDrive.h` + `NeuralDriveWeights.h` | Inferenza GRU (12 stati) del preamp a diodi; pesi generati da `tools/neural/train_neural_drive.py`. |
| `DSP/TubeStage.h` + `DSP/eta/*` | Stadio valvolare (motore ETA-1): valvole Koren, profili, sag, trasformatore, oversampling FIR. |
| `DSP/Effects.h` | Distortion, Chorus, Phaser, Delay, Reverb (FDN), Compressor, EQ3Band, Space, Limiter. |
| `DSP/Glitch.h` | Glitch e Beat Repeat a tempo, orologio musicale condiviso. |
| `DSP/Sidechain.h` | Ducker a curva (forme di fabbrica + curva libera), trigger sync/MIDI/audio, banda. |
| `DSP/Common.h` | Utility: smoother, interpolazioni, conversioni, RNG, DC blocker, pan. |
| `GUI/Pages.*` | Le 12 pagine (OSC, FILTER, FISH, ENV, LFO, MOD, SEQ, ARP, FX, TUBE, SPACE, DUCK) e i loro display. |
| `GUI/Widgets.h` | Knob, LED, display wavetable, oscilloscopio, meter. |
| `GUI/BassXLookAndFeel.h` | Look "cyber" vettoriale e temi colore. |
| `GUI/Assets.h` | Accesso alle texture incorporate. |

## 3. Percorso del segnale nel codice

`BassXAudioProcessor::processBlock`:

1. **Ext Mode**: copia l'ingresso sidechain prima di azzerare il buffer
   (ingresso e uscita condividono i canali).
2. **Parametri**: `SynthParams::updateFrom` legge tutti i valori APVTS una volta
   per blocco; le voci ricevono `setParameters`.
3. **Sequencer / arpeggiatore**: `playEngine` produce eventi con posizione al
   campione, calcolati dalla `ppqPosition` della DAW.
4. **MIDI dalla GUI** (tastiera a schermo) unito a quello della DAW.
5. **Render campione-esatto**: il blocco è spezzato a ogni evento (MIDI o
   sequencer); fra un evento e l'altro le voci rendono in un colpo solo.
6. **Effetti**: `tube` → `processFxChain` (10 slot nell'ordine scelto, ogni
   effetto al massimo una volta) → `sidechain` (con analisi spettrale prima/dopo
   per la pagina DUCK) → volume d'uscita (smoother 15 ms) → `limiter`.

Dentro `Voice::renderNextBlock`, per ogni campione:

```
sorgenti di modulazione (inviluppi, LFO, accento, …) → ModMatrix::apply
→ intonazione (glide in ottave, vibrato, pitch bend, matrice)
→ sub, rumore, modulatore FM → oscillatori (FM, sync) → ring → mix normalizzato
→ Neural Drive → filtri (oversampling 1/2/4/8x, routing serie/parallelo/split)
→ VCA (Env 1 × accento × velocity × matrice) → Muffler → pan → DC blocker
```

## 4. Thread e real-time

- **Audio thread**: nessuna allocazione e nessun lock bloccante in
  `processBlock`. Code e buffer sono dimensionati in `prepareToPlay`.
- **Parametri**: letti come `std::atomic<float>` di APVTS.
- **Matrice e pattern**: modificati dalla GUI con scritture puntuali sullo stato
  condiviso, letti dall'audio a ogni blocco.
- **Wavetable da campione e curva del sidechain**: doppio buffer con indice
  atomico; la costruzione (lettura file, FFT) avviene fuori dall'audio thread,
  l'audio scambia solo il puntatore.
- **Export WAV**: un'istanza separata del processore renderizza offline su un
  thread di lavoro (`Thread::launch`); il risultato torna alla GUI con
  `MessageManager::callAsync`.
- **Analizzatori** (oscilloscopio, goniometro, spettro DUCK): ring buffer di
  atomici scritti dall'audio, letti dai timer della GUI.

## 5. Determinismo e aggancio alla DAW

- Passo del sequencer, LFO sincronizzati, autopan, glitch, beat repeat e
  sidechain sono **funzioni della posizione nel brano**, non contatori interni.
- Random, probabilità dei passi e scelte del glitch derivano da hash del numero
  assoluto di passo: riproducibili a ogni play e dopo un salto della testina.
- A trasporto fermo un orologio interno parte alla prima nota suonata e si
  ferma con i tasti.

## 6. Stato salvato

`getStateInformation` scrive un albero `BASSXSTATE` (versione 1):

| Nodo / proprietà | Contenuto |
|---|---|
| `presetIndex`, `presetName` | Preset corrente. |
| figlio APVTS | Tutti i parametri + proprietà GUI (`uiTheme`, `uiCustomColour`). |
| matrice | 16 slot: sorgente, via, destinazione, profondità, attivo. |
| sequencer | 8 pattern × 16 passi: nota, gate, accento, slide, tie, velocity, ratchet, probabilità. |
| `USER_WAVETABLE` | Nome e cicli della wavetable da campione (base64). |
| `SIDECHAIN_CURVE` | Punti `PT` (x, y, tensione) della curva libera. |

I preset non sono esposti all'host come programmi (`getNumPrograms() == 1`):
alcuni host richiamano il programma dopo il ripristino della sessione e
sovrascriverebbero le modifiche.

## 7. Build

| Target | Cosa è | Opzione CMake |
|---|---|---|
| `BASSX_VST3`, `BASSX_AU`, `BASSX_Standalone` | Il plugin | — |
| `BassXAudioTest` | Test audio (vedi §8) e generatore di `PARAMETRI.md` | sempre |
| `BassXRenderGui` | Screenshot PNG di ogni pagina della GUI | `-DBASSX_BUILD_GUI_RENDERER=ON` |
| `BassXMatch` | Rende un pattern trascritto (calibrazione sul 303) | `-DBASSX_BUILD_MATCH=ON` |

JUCE si passa con `-DJUCE_PATH=…`; senza, CMake lo cerca in `BASSX/JUCE`,
accanto alla repo o un livello sopra.

- **Windows**: `build_windows.bat` in radice — Visual Studio, build, test
  audio, screenshot GUI, release versionata `releases/windows/BASSX-v<ver>-b<n>`,
  pluginval strictness 10, controllo dati personali, commit e push.
  `build_windows.bat win` pubblica solo il branch `build/v<ver>-b<n>`.
- **macOS**: GitHub Actions (`.github/workflows/build.yml`) su tag `v*` o branch
  `release/**`; `mac_build_github.bat` avvia la build e scarica l'installer.
  `build_macos.sh` per build locale su Mac.
- **Versione**: unica, in `BASSX/VERSION` (la leggono CMake e gli script).

## 8. Test (`BassXAudioTest`)

Rende il plugin vero, offline e deterministico. Esce con codice ≠ 0 se una
verifica fallisce; `build_windows.bat` non pubblica se fallisce.

| Gruppo | Cosa verifica |
|---|---|
| Preset | Ogni preset suona, niente NaN, picco e RMS nei limiti; tema/impostazioni di sessione intatti al cambio preset; preset omonimi; sessione + `setCurrentProgram`. |
| Real-time | `processBlock` senza allocazioni; 12 catene FX casuali con seq/arp/fish/space senza NaN, silenzio o clipping; Ext Mode passa solo con la nota. |
| Sequencer | Timing al campione con blocchi di 64…2048 campioni e variabili, con e senza swing; nessun legato involontario; note-on/off bilanciati; stop della DAW con latch; trasporto fermo. |
| Distortion / Tube | Latenza dichiarata = latenza reale, niente pettine nel mix parallelo. |
| Space | Width, Bass Mono, pan. |
| Neural | L'inferenza GRU coincide con il riferimento Python (`tools/neural/reference_io.txt`). |
| Code FX | Delay/riverbero spenti non fanno riaffiorare audio vecchio. |
| Zipper | Salti dei knob (drive, mix, risonanza) senza click. |
| Filtri | Ogni modello ha la risposta attesa (es. Moog HP taglia i bassi); grafico RESPONSE = filtro reale. |
| Sidechain | Sync sul battito, fase, banda, trigger MIDI ch16 e audio. |
| Glitch / Beat Repeat | Determinismo, ripetizioni, nessun click. |
| Export WAV | Lunghezza esatta, 24 bit, non muto, seq e arp. |
| Sample wavetable | Periodo trovato, nota intonata, salvataggio nella sessione. |
| Osc sync | Con SYNC l'altezza è quella di OSC 1 su ogni nota. |
| Layer | Fondamentali in fase su tutte le tabelle, somma senza cancellazioni, sub in fase, retrigger off, PHASE. |
| Impatto | **Ognuno dei 298 parametri sonori cambia davvero l'audio**; render bit-identico fra due esecuzioni. |

### Rigenerare la documentazione dei parametri

```bash
BassXAudioTest --params docs/PARAMETRI.md
```

Da lanciare dopo ogni modifica a `Parameters.cpp` o ai preset di fabbrica.

## 9. Strumenti (`BASSX/tools`)

| Cartella | Contenuto |
|---|---|
| `render/` | `AudioTest.cpp`, `RenderGui.cpp`, `Match.cpp`. |
| `neural/` | Addestramento della GRU sul modello numerico del circuito (ODE con Newton-Raphson) e I/O di riferimento per il test. |
| `calibration/` | Trascrizione di loop di TB-303 registrati e fit dei parametri (preset "TB Recorded"). Percorsi da `BASSX_REFS` e `BASSX_MATCH`. |
| `gui/`, `generate_textures.py` | Generazione di texture e strip dei knob. |

## 10. Regole del repository

- Niente dati personali né percorsi del proprio PC nei file versionati:
  `tools/check_dati_personali.ps1` lo verifica prima di ogni push.
- Una build = un branch `build/v<versione>-b<numero>`; le versioni sono tag
  `v<versione>`.
- Licenza: [`LICENSE`](../LICENSE) (tutti i diritti riservati).
