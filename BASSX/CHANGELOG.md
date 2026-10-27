# BASSX — Changelog

## 1.5.1 — documentazione completa, OSC SYNC corretto

- **Documentazione** nella cartella `docs/`: manuale d'uso con ogni controllo
  pagina per pagina (`MANUALE.md`), riferimento di tutti i 307 parametri con
  ID, range e default generato dal plugin stesso (`PARAMETRI.md`,
  `BassXAudioTest --params`), architettura del codice (`ARCHITETTURA.md`).
- **OSC SYNC**: la frequenza del master non veniva mai impostata e restava
  110 Hz fissi: con SYNC acceso OSC 2 suonava sempre in La, su qualunque nota.
  Ora OSC 2 riparte a ogni ciclo di OSC 1 e l'altezza segue la nota.
- Test: 123 verifiche (nuovo: altezza con SYNC su due note).

## 1.5.0 — layer in fase: un basso solo, non sorgenti sovrapposte

- **Causa**: ogni oscillatore partiva da una fase casuale (scelta all'apertura del
  plugin) e ogni wavetable aveva la fondamentale in una fase diversa. Due layer alla
  stessa nota si sommavano sfasati: comb, bassi che si svuotano, timbro diverso a ogni
  sessione — "layer sovrapposti" invece di un suono unico.
- **Ora**: tutte le tabelle (di fabbrica e da campione) hanno la fondamentale allineata
  (+seno, zero in salita a fase 0); OSC 1/2/3 e SUB ripartono insieme a fase 0. Due layer
  qualsiasi alla stessa nota sommano le fondamentali al 100%: il mix cambia il timbro,
  non buca il basso. Anche con RETRIG spento girano liberi ma restano allineati.
- **SUB**: segue l'oscillatore 1 (riparte con lui); triangolo messo in fase col seno.
- **PHASE** (per oscillatore): con voce singola è la fase di partenza (0 = allineato,
  50% = invertito) — sfasare un layer diventa una scelta; con UNISON resta la casualità
  delle voci.
- **WAVEFORM 303**: bottone ON accanto al selettore in alto (layer principale acceso/spento).
- Test: 121 verifiche (fase di tutte le tabelle, somma dei layer, sub sine/tri/square,
  retrigger OFF, PHASE), pluginval strictness 10 OK.

## 1.4.0 — DUCK (sidechain/phase), glitch, beat repeat, export WAV, campione > wavetable

- **Filtri, grafico RESPONSE**: la curva era sempre la stessa. Ora è misurata sul filtro
  vero (impulso attraverso modello, routing serie/parallelo/split e blend): cambiando tipo
  si vede il taglio reale; curva F1 / F2 / totale e marker dei cutoff.
- **Moog HP24**: lasciava passare i bassi (usava l'uscita dell'ultimo stadio sbagliato).
- **LFO PHASE in modalità Free**: non aveva effetto, ora sposta la fase.
- **Pagina DUCK — sidechain / phase control** (stile Kickstart, più completo): 8 forme
  di fabbrica + CUSTOM disegnabile a punti con curvatura per segmento; RATE a tempo
  (1/16 … 4 bar), DEPTH, PHASE (sposta il taglio sul battito), SMOOTH anti-click,
  BAND low/high (ducking solo di una banda, es. i bassi sotto 150 Hz); trigger SYNC
  (host), MIDI (note sul canale 16) o AUDIO (ingresso sidechain esterno con soglia).
  Lo spettro mostra prima/dopo, la curva di gain e la testina di fase in tempo reale.
  Impostazione di sessione: non cambia col preset.
- **FX: Glitch** (Stutter / Reverse / Tape Stop / Gate o Random, griglia a tempo,
  dado deterministico con SEED: il loop glitcha uguale ad ogni giro) e **Beat Repeat**
  (interval, grid, chance, gate, decay, mix). Bordi a 3 ms, nessuna latenza.
  La catena FX passa a 10 slot.
- **SEQ: EXPORT WAV** del loop composto (1–16 battute, 24 bit, BPM dell'host, coda degli
  effetti inclusa perché si registra il secondo giro): bottone o trascina il riquadro
  direttamente nella DAW.
- **OSC: campione > wavetable**: trascina un WAV/AIFF/FLAC/OGG/MP3 su un oscillatore.
  Se il suono ha un'intonazione ogni frame è un ciclo vero (suona intonata sulla
  tastiera), altrimenti diventa un timbro; 16 frame lungo il campione (POS li scorre),
  band-limited senza aliasing. Forma "Sample", salvata nella sessione.
- GUI: intestazione "Comp / Limit" non più troncata.
- Test: 115 verifiche (impatto audio di tutti i 298 parametri, sidechain, glitch, beat
  repeat, export WAV, campione > wavetable), pluginval strictness 10 OK.

## 1.3.1 — preset, stop della DAW, code degli effetti, anti-zipper

- **Audio "in loop" dopo lo stop della DAW**: con LATCH attivo (o un accordo tenuto) il
  seq/arp ripartiva sull'orologio interno appena il trasporto si fermava e girava
  all'infinito. Ora si ferma con la DAW; a trasporto fermo riparte solo suonando un tasto.
- **Delay / riverbero / chorus / phaser spenti o fuori catena**: il loop del delay (feedback
  fino a 105%) continuava a girare nascosto e alla riaccensione — o cambiando preset —
  riaffiorava una frase vecchia. Ora uno stadio spento si ferma e alla riaccensione riparte
  con le code vuote.
- **Sessione riaperta**: i preset erano esposti all'host come "programmi" e molti host
  (Ableton, Reaper, FL) richiamano il programma dopo aver ripristinato lo stato: il preset
  veniva ricaricato sopra le modifiche salvate. Ora i preset si caricano solo dal browser
  del plugin, e il nome del preset è salvato nella sessione.
- **Cambio preset**: non cambia più tema/colore della GUI, oversampling, accordatura,
  pitch bend range, tetto del limiter (impostazioni, non suono).
- **Preset utente con lo stesso nome di uno di fabbrica**: dal menu e dopo il salvataggio
  veniva caricato quello di fabbrica. Ora menu e salvataggio usano l'indice, non il nome.
- **Anti-zipper**: rampa di 20 ms su mix di Distortion/Chorus/Phaser/Delay/Riverbero/
  Compressore, drive della distorsione (dentro l'oversampling 4x) e feedback del delay;
  10 ms su risonanza e drive dei filtri. Gli interruttori ON/OFF degli effetti fanno
  crossfade.
- **Parametri continui**: tolti i passi 0.001 / 0.0001 sui parametri continui.
- Test nuovi in `BassXAudioTest` (76 verifiche): stop con latch, delay riacceso, preset
  (tema, impostazioni, omonimi, sessione + setCurrentProgram), zipper su 7 controlli.
  Sulla 1.3.0 ne fallivano 10.

## 1.3.0 — restyling "cyber"

- **Interfaccia ridisegnata**: superfici nere opache, moduli ad angoli smussati, tasti/tab/menu/display
  LCD/LED/meter vettoriali. Knob antracite (nuovi asset) con tacche, lancetta e arco LED che si
  accendono col valore.
- **Knob uniformi e compatti** (42 px nelle pagine, 54 px nella fila principale) su una griglia a passo fisso.
- **Pagine riorganizzate** in moduli standard allineati: FILTER (con risposta in frequenza), FISH e ARP
  (con oscilloscopio d'uscita), TUBE e SPACE (display a tutta larghezza).
- **Temi colore**: NEON, CYAN, PLASMA, AMBER, ICE e CUSTOM (colore a scelta dalla tavolozza), dal
  menu THEME; salvati nello stato del plugin.
- Tastiera e sequencer nello stesso stile (barre di vetro con "tappo" LED).

## 1.2.0 — stadio valvolare ETA-1

- **Nuova pagina TUBE**: preamp valvolare con il motore DSP di ETA-1 (`Source/DSP/eta/`):
  modelli Koren triodo/pentodo risolti sulla retta di carico, sag dell'alimentatore, blocking di
  griglia, trasformatore d'uscita, AGE. 26 modelli (6 groovebox + 20 valvole: 12AX7, EF86, EL34, 300B...).
- Parametri: On, Model, Drive, Bias, Sag, Character, Age, Warmth, Presence, Tone, Mix, Output.
- Sta in testa alla catena FX, prima della distorsione. Oversampling 4x FIR a fase lineare, latenza
  costante accesa/spenta (+61 campioni dichiarati all'host), make-up automatico sul drive (livello entro ~1 dB).
- Curva di trasferimento live sulla pagina.
- Nuovi preset Hardware: "Acid Tube 12AX7", "Hard Tube EF86".

## 1.1.1 — calibrazione su registrazioni di TB-303

Confronto su 44 loop di un 303 vero (trascritti nota per nota e risuonati identici da BASSX
con `BassXMatch`, strumenti in `tools/calibration/`).

- **Slide**: ora in dominio logaritmico (come la CV del 303) e `Glide` = tempo al 90% dell'intervallo.
  Prima era in Hz con costante di tempo = Glide: 87 -> 174 Hz arrivava al 90% in ~110 ms, il 303
  registrato in ~25-30 ms. Preset TB/Acid a 30 ms; Acid Deep e preset DF riscalati per suonare come prima.
- **Gate del sequencer**: default 50% -> 60% (il 303 resta a livello pieno per ~60% del passo).
- **Soft attack** 4 ms sulle note non accentate nei preset TB/Acid (Env Attack, come lo stock 303).
- **Nuovo preset "TB Recorded"** (Hardware): timbro medio dei loop registrati — filtro aperto con
  risonanza alta, inviluppo corto, fondamentale -13 dB sotto 190 Hz, distorsione leggera.
- `BassXMatch` (opzione CMake `BASSX_BUILD_MATCH`): render di un pattern JSON su un preset, per la calibrazione.

## 1.1.0

### Nuovo
- **Pagina FISH** — controlli stile Devil Fish: Slide Time, Sweep Amt., Env. Attack (soft attack
  delle note non accentate), Acc. Decay, Acc. Vol., Vib. Speed, Vib. Depth, Filter FM (VCA → cutoff
  a frequenza audio), Muffler (soft clip sul VCA), Sweep Speed (carica lenta dell'accento).
- **Neural Drive** — preamp a diodi con memoria modellato da una GRU a 12 stati condizionata sul
  drive (addestramento in `tools/neural/`).
- **Ext Mode** — ingresso audio stereo (sidechain) processato da filtro, neural drive e VCA.
- **Arpeggiatore** — 7 modi, 1-4 ottave, gate, swing, latch, slide, accento ogni N.
- **Seq + Arp** — Layer, Seq Rhythm > Arp Notes, Seq Transposes Arp.
- **Sequencer** — ratchet 1-4 e probabilità per passo, direzioni Forward/Backward/Ping-Pong/Random
  (deterministiche, in sync con la DAW), Key Follow.
- **Catena FX riordinabile** — 8 slot, ognuno sceglie l'effetto; destinazioni FX della matrice attive.
- **Pagina SPACE** — Pan, Width M/S, Bass Mono LR24, Haas, rotazione, autopan sincronizzato,
  goniometro e correlazione.
- 9 preset nuovi (Fish, Arp, Space); pattern 5-8 con ratchet e probabilità.
- Versioning delle build (`VERSION`, `build_number.txt`, `releases/`, `BUILDS.md`, tag Git).

### Corretto
- **Build Windows/MSVC**: errore C2440 nella init-capture di una lambda annidata (PluginEditor);
  runtime statico (niente VC++ Redistributable), `/utf-8`, copia post-build solo su macOS,
  ricerca automatica di JUCE.
- **macOS**: impostazioni di architettura prima di `project()` — la build usciva solo arm64.
- **Allocazioni sul thread audio**: `updateFrom()` costruiva ~250 `juce::String` per blocco e
  `processBlock` creava un `MidiBuffer` locale. Ora zero allocazioni (verificato dal test).
- **Distorsione**: dry non compensato in latenza (filtro a pettine col mix < 100%) e traccia
  disallineata a distorsione spenta. Ora FIR a fase lineare, dry ritardato, latenza costante.
- **Sequencer**: con lo swing alcuni passi dispari venivano saltati; il note-off di fine gate
  poteva andare perso se cadeva nello stesso blocco del passo successivo; lo slide chiudeva il
  gate prima del passo legato; arrotondamento al campione delle note.
- **Tasti col sequencer attivo**: la nota premuta per trasporre suonava anche lei.
- **Matrice**: 12 destinazioni non facevano nulla (unison detune, drive dei filtri, rate/depth
  LFO, decay inviluppi, tutte le FX) e la sorgente Step Seq non era mai alimentata.
- **Stato**: parametri booleani non ripristinati esattamente da `setStateInformation` (pluginval).
- **Test audio**: cattura in `/tmp` (inesistente su Windows), ora nella cartella temporanea.

## 1.0.0
- Prima versione (build macOS arm64).
