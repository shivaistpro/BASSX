# BASSX

Sintetizzatore per bassi VST3 / AU / Standalone. Il nucleo è un modello del
Bass Line del 1982 — filtro ladder a diodi, circuito di accento capacitivo,
slide legato — costruito dentro un motore wavetable moderno con unison,
matrice di modulazione, sequencer a 16 passi e rack di effetti.

Dalla 1.1: sezione **Fish** con i controlli stile Devil Fish e uno stadio di
saturazione **neurale** (GRU), **arpeggiatore** combinabile col sequencer,
**catena effetti riordinabile e modulabile**, sezione **Space** per panorama e
immagine stereo, **Ext Mode** per processare audio esterno. Novità e
correzioni complete in `CHANGELOG.md`.

---

## Cosa c'è dentro

### Generazione

| | |
|---|---|
| **3 oscillatori wavetable** | 16 tabelle band-limited con mipmapping a 10 livelli, morphing su 16 frame, interpolazione di Hermite |
| **Unison** | fino a 16 voci per oscillatore, con detune non lineare, blend centro/lati, apertura stereo, randomizzazione di fase |
| **Sub oscillatore** | sinusoide (oscillatore a ricorsione, alias-free per costruzione), triangolo, quadra con polyBLEP; −2, −1 o 0 ottave |
| **Rumore** | bianco e rosa (Voss-McCartney a 6 poli, errore < 0.3 dB da 20 Hz a 20 kHz) |
| **Cross-modulazione** | FM/phase modulation con sorgente e destinazione selezionabili, ring modulation, hard sync con correzione BLEP |
| **Wavefolder** | per oscillatore, applicato prima del mix |

Le 16 wavetable: Analog Saw, Analog Square, Analog Tri, **Acid Saw**,
**Acid Square**, Sine > Saw, Pulse Morph, Formant Bass, Reso Sweep, FM Bass,
Harmonic Stack, Folded Sine, Digital, Growl, Sub Harmonic, Metallic.

Le due "Acid" non sono rampe e quadre ideali: riproducono le imperfezioni
misurate sull'hardware — ritorno non istantaneo del VCO, duty cycle al 48%
invece del 50%, smorzamento delle armoniche alte. Sono quelle imperfezioni a
distinguere il timbro.

### Filtri

Due slot indipendenti, sedici modelli, routing **seriale / parallelo / split**:

- **Diode 18dB** e **Diode 24dB** — ladder a diodi, il modello Bass Line.
  Poli distribuiti (non coincidenti), feedback saturato, perdita di basse
  frequenze con la risonanza regolabile da 0 (carattere originale) a 100%
  (utilizzabile come filtro generico).
- **Moog LP24 / LP12 / BP / HP** — ladder a transistor, risonanza che canta.
- **SVF LP12/24, HP12/24, BP12, Notch, Peak** — state variable TPT.
- **Comb** — risuonatore accordato con smorzamento nel loop.
- **Formant** — tre passa-banda che percorrono A → E → I → O → U.

Tutti in topologia zero-delay-feedback con **risoluzione del feedback tramite
Newton-Raphson**: due iterazioni per campione eliminano lo scostamento della
frequenza di risonanza che il feedback ritardato di un campione introduce (a
8 kHz con risonanza alta l'errore supera il semitono).

Oversampling della sola sezione filtro, selezionabile Off / 2x / 4x / 8x, con
decimazione Butterworth del quarto ordine. Solo lì perché solo lì nasce
l'aliasing: gli oscillatori sono già band-limited per costruzione.

### Modulazione

- **4 inviluppi DAHDSR** completi (Delay, Attack, Hold, Decay, Sustain,
  Release) con **curva regolabile per segmento** da −1 a +1. Curve
  esponenziali vere, non rampe lineari.
- **4 LFO** con 10 forme (comprese sample & hold, random smussato e mappa
  caotica), sincronizzabili al tempo dell'host su 16 divisioni,
  con fade-in, offset di fase, modalità di trigger e lisciatura.
- **Matrice di modulazione a 16 slot** con 19 sorgenti, 43 destinazioni e
  sorgente **VIA** che scala la profondità di ciascuno slot.
- **Circuito di accento** con condensatore che accumula carica fra note
  ravvicinate — è quello che fa "pompare" le raffiche di sedicesimi accentati.

### Fish (Devil Fish + motore neurale)

La fila di controlli di una Bass Line modificata: **Slide Time**, **Sweep
Amt.** (accento sul filtro), **Env. Attack** (attacco morbido delle note non
accentate), **Acc. Decay**, **Acc. Vol.**, **Vib. Speed / Vib. Depth**, più:

- **Filter FM** — l'uscita del VCA modula il cutoff a frequenza audio.
- **Muffler** — saturazione morbida sull'uscita del VCA.
- **Sweep Speed** — velocità di carica del circuito di accento.
- **Neural Drive** — preamp a diodi con memoria modellato da una **GRU a 12
  stati**, condizionata sul drive, prima del filtro (error-to-signal ratio
  2.5% sul set di validazione). Addestrata con
  `tools/neural/train_neural_drive.py` sul modello numerico del circuito
  (ODE risolta con Newton-Raphson): costo fisso per campione invece di
  iterazioni. Non è la cattura di un esemplare hardware — per quella servono
  registrazioni input/output dell'unità reale, e lo script è pronto a
  riceverle al posto del simulatore.
- **Ext Mode** — l'ingresso audio dello strumento (sidechain) sostituisce gli
  oscillatori e passa da filtro, neural drive e VCA, comandato dalle note.

### Sequencer e arpeggiatore

Sequencer: 16 passi, 8 pattern, per passo nota, gate, **accento**, **slide**,
**tie**, **ratchet** (1-4) e **probabilità**. Direzione Forward / Backward /
Ping-Pong / Random, swing fino al 75%, gate, ottava, **Key Follow** (il tasto
traspone il pattern).

Arpeggiatore: Up, Down, Up/Down, Down/Up, As Played, Random, Chord; 1-4
ottave, divisione, gate, swing, latch, slide, accento ogni N note.

Combinazioni (sequencer e arpeggiatore attivi insieme):

- **Layer** — suonano insieme, ognuno col suo ritmo;
- **Seq Rhythm > Arp Notes** — ritmo, accenti e slide del sequencer, altezze
  dall'accordo tenuto;
- **Seq Transposes Arp** — l'arpeggio segue la trasposizione del passo.

Sempre agganciati alla DAW: il passo è funzione della posizione nel brano
(`ppqPosition`), e anche Random e probabilità derivano da un hash del numero
di passo, quindi suonano uguali a ogni play e dopo ogni salto della testina.
A trasporto fermo il motore suona finché si tengono i tasti (o col latch).
Gli slide tengono il gate aperto fino al passo legato, come sul 303.

### Space

Pan, Width (M/S), **Bass Mono** (lato stereo rimosso sotto la frequenza
scelta, Linkwitz-Riley 24 dB/ott), Haas, rotazione dell'immagine, autopan
sincronizzato. Goniometro e correlazione nella pagina SPACE.

### Effetti

Distorsione (8 algoritmi, oversampling 4x FIR a fase lineare), EQ 3 bande,
Chorus, Phaser, Delay (sync, ping-pong, filtri nel loop), Riverbero (FDN a 8
linee, matrice di Householder), Compressore (feed-forward, RMS o peak,
ginocchio morbido), Space — in **8 slot riordinabili**: ogni slot sceglie
l'effetto, l'ordine si cambia dall'host o dalla pagina FX. Limiter con
lookahead sempre in fondo.

Le destinazioni FX della matrice (drive, mix di delay e riverbero, feedback,
chorus, phaser, pan, width, autopan) modulano la catena dalla voce suonata più
di recente.

### Voci

- **Mono 303** — legato con slide, come il sequencer originale
- **Mono Legato**
- **Poly** — fino a 16 voci, con furto di voce che privilegia le voci in
  release avanzata invece della più vecchia

---

## Preset

33 preset di fabbrica in 12 categorie: Basics, Acid, Reese, Sub, Pluck, FX,
Sequencer, Poly, Hardware, Fish, Arp, Space. **Ogni preset porta con sé i propri quattro
inviluppi completi**, la matrice di modulazione e i pattern del sequencer.

I preset utente vengono salvati in:

- macOS: `~/Library/BASSX/Presets`
- Windows: `%APPDATA%\BASSX\Presets`

---

## Compilare

### Requisiti

- CMake ≥ 3.22
- Compilatore C++17 (Xcode su macOS, Visual Studio 2022 su Windows)
- JUCE 8 o superiore
- Python 3 con Pillow e NumPy — **solo** se si vogliono rigenerare le texture

JUCE non è nella repo: se `-DJUCE_PATH` non è passato, CMake lo cerca in una
cartella `JUCE` dentro `BASSX/`, accanto alla repo o un livello sopra. Per
scaricarlo:

```bash
git clone --branch 8.0.12 --depth 1 https://github.com/juce-framework/JUCE.git
```

### macOS

```bash
cd BASSX
cmake -B build -DJUCE_PATH=/percorso/di/JUCE -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j8
```

Con `COPY_PLUGIN_AFTER_BUILD TRUE` i binari finiscono anche in
`~/Library/Audio/Plug-Ins/VST3` e `~/Library/Audio/Plug-Ins/Components`.

Il progetto è configurato come **universal binary** (arm64 + x86_64).

### Windows

Doppio click su `build_windows.bat` (cartella sopra a questa): trova Visual
Studio, compila, esegue tutti i test (audio, GUI, pluginval strictness 10),
crea la release versionata e fa commit + tag + push solo se i test passano.

A mano:

```bash
cmake -B build -A x64 -DJUCE_PATH="C:/percorso/di/JUCE"
cmake --build build --config Release
```

### Versioning delle build

- `VERSION` — versione del plugin (la leggono CMake e gli script);
- `build_number.txt` — progressivo, +1 a ogni build riuscita;
- `releases/<piattaforma>/BASSX-v<versione>-b<build>/` (+ `.zip`) — ogni build
  in una cartella sua, `latest/` è l'ultima;
- `BUILDS.md` — registro di tutte le build con esito dei test;
- tag Git `v<versione>`.

### Rigenerare le texture

```bash
pip install pillow numpy
python3 tools/generate_textures.py
cmake -B build -DJUCE_PATH=... && cmake --build build --config Release
```

---

## Interfaccia

Il pannello superiore è in **alluminio spazzolato**, quello inferiore in
**ABS stampato**, i knob sono **cromati con zigrinatura**. Non sono gradienti
e forme geometriche: sono texture generate risolvendo il modello ottico del
materiale — riflessione anisotropa per il metallo spazzolato, mappa d'ambiente
con orizzonte netto per il cromo, microrugosità più speculare acromatica larga
per la plastica, occlusione ambientale nei recessi, ombre proiettate coerenti
con una singola luce a 55°.

I knob sono sprite sheet di 65 fotogrammi su 270°: la riflessione si sposta
correttamente durante la rotazione, cosa che un knob disegnato a vettori non
può fare.

Il layout ricalca la logica di quel tipo di hardware — testata con display,
fila di controlli principali sempre visibile, banda arancione di separazione,
sezione modulare scura sotto — senza riprodurre logo, marchio denominativo,
font o grafiche di pannello di alcun produttore.

Pagine: **OSC**, **FILTER**, **FISH**, **ENV**, **LFO**, **MOD**, **SEQ**,
**ARP**, **FX**, **SPACE**.

---

## Come suonarlo

- **Accento**: velocity ≥ 100 (soglia regolabile). Note accentate ravvicinate
  accumulano carica e si aprono progressivamente.
- **Slide**: sovrapponi le note nel piano roll — fai partire la successiva
  prima che finisca la precedente. Il legato attiva il portamento e non
  ritriggera gli inviluppi.
- **Patch acid di partenza**: Osc 1 su Acid Saw, Cut Off ~340 Hz, Resonance
  0.78, Env Mod 0.72, Decay 190 ms, Accent 0.65, Drive 3.4.
- **Reese**: due oscillatori Analog Saw con unison 7 e detune 0.12, il secondo
  disaccordato di 9 cent, sub al 60%, filtro Moog LP24 aperto.
- Con il **sequencer** attivo, tieni premuta una nota per trasporre il pattern.

---

## Struttura

```
BASSX/
  CMakeLists.txt
  Assets/                    texture PNG incorporate nel binario
  tools/
    generate_textures.py     generatore delle texture
    render/                  utility di verifica (GUI e audio), non nel plugin
  Source/
    PluginProcessor.*        voci, MIDI campione-esatto, catena effetti, stato
    PluginEditor.*           interfaccia principale
    Parameters.*             ~230 parametri APVTS e struttura di lavoro
    DSP/
      Common.h               primitive, saturatori, filtri elementari, delay line
      WavetableBank.*        costruzione delle tabelle band-limited (IFFT)
      Oscillator.h           oscillatore wavetable, unison, sub, rumore
      Filters.h              sei famiglie di filtro + wrapper multi-modello
      Envelope.h             DAHDSR a curve regolabili, circuito di accento
      LFO.h                  LFO multiforma sincronizzabile
      ModMatrix.h            matrice a 16 slot
      Effects.h              rack effetti completo
      Voice.h                una voce completa
      Sequencer.h            sequencer a 16 passi
    GUI/
      Assets.h               cache delle texture
      BassXLookAndFeel.h     look and feel basato su immagini
      Widgets.h              knob, LED, meter, display, visualizzatori
      Pages.*                le sette pagine
    Presets/
      PresetManager.*        preset di fabbrica e utente
```

---

## Note tecniche

**Nessuna allocazione, nessun lock, nessuna eccezione nel percorso audio.**
I parametri vengono letti una volta per blocco in una struttura POD contigua;
i valori che si sentono (cutoff, livelli, volume, tempi del delay) hanno uno
smoother dedicato, quindi il risultato udibile resta continuo.

**MIDI campione-esatto.** Il blocco viene spezzato a ogni evento, MIDI o del
sequencer. Senza, il timing quantizzerebbe alla dimensione del buffer: a 512
campioni e 48 kHz sono ~10 ms di jitter, chiaramente udibili su una linea in
sedicesimi.

**Latenza dichiarata all'host**: somma dell'oversampler della distorsione e
del lookahead del limiter (2 ms). Senza la dichiarazione la traccia risulta in
ritardo rispetto alle altre.

**Denormali neutralizzati** in ogni stato che può decadere verso zero per
minuti (linee di ritardo, coda del riverbero, integratori dei filtri). Su x86
un denormale costa 50-100 cicli per operazione: senza questo, la CPU sale
dopo che l'ultimo suono è finito, non mentre suona.

### Misure

Verificate su questo codice, non dichiarate a stima:

| | |
|---|---|
| Aliasing, rampa a 1760 Hz, filtro in bypass | **−98 dB** rispetto al picco |
| Rapporto armonico / inarmonico | **91.6 dB** |
| Preset di fabbrica senza NaN, silenzio o clipping | **33 / 33** |
| Note del sequencer al campione esatto (blocchi 64…2048 e variabili, swing 0/50%) | **0 errori** |
| Allocazioni in `processBlock` (contatore su `operator new`) | **0** |
| Latenza distorsione accesa = spenta = dichiarata | **sì** |
| Inferenza C++ della GRU vs PyTorch (ESR) | **< 1e-4** |
| Warning del compilatore (flag JUCE raccomandati, `-Wsign-conversion`, `-Wfloat-equal`, `-Wswitch-enum`, `-Wshadow`) | **0** |

Gli strumenti di verifica sono in `tools/render/` e si compilano con:

```bash
cmake -B build -DJUCE_PATH=... -DBASSX_BUILD_GUI_RENDERER=ON
cmake --build build --target BassXAudioTest BassXRenderGui
./build/BassXAudioTest_artefacts/Release/BassXAudioTest
```

---

## Licenza

Tutti i diritti riservati: si può scaricare, compilare e usare BASSX (anche per
musica commerciale), non modificarlo né ridistribuirlo. Testo completo in
[`LICENSE`](../LICENSE).

Nella repo non vanno dati personali né percorsi del proprio PC: li controlla
`tools/check_dati_personali.ps1`, lanciato da `build_windows.bat` prima di ogni
push.

## Nota legale

BASSX non è un prodotto Roland, non è affiliato a Roland Corporation e non ne
riproduce logo, marchi denominativi, font o grafiche di pannello. "TB-303" e
"Roland" sono marchi dei rispettivi titolari e sono citati unicamente a scopo
descrittivo, per indicare la categoria di strumento a cui il modello sonoro si
ispira. Le texture dell'interfaccia sono generate proceduralmente dal codice in
`tools/generate_textures.py` e non derivano da fotografie di prodotti esistenti.
