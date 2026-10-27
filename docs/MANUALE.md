# BASSX — Manuale d'uso

Versione 1.5. Questo manuale spiega **cosa fa ogni controllo e come lavora il
suono dentro BASSX**, pagina per pagina. Range esatti e valori di default di
tutti i 307 parametri sono in [`PARAMETRI.md`](PARAMETRI.md), generato dal
plugin stesso. Come è fatto il codice: [`ARCHITETTURA.md`](ARCHITETTURA.md).

## Indice

1. [Il percorso del suono](#1-il-percorso-del-suono)
2. [Barra superiore, tastiera e temi](#2-barra-superiore-tastiera-e-temi)
3. [OSC — oscillatori, sub, rumore, cross-modulazione](#3-osc--oscillatori-sub-rumore-cross-modulazione)
4. [FILTER — due filtri e routing](#4-filter--due-filtri-e-routing)
5. [FISH — controlli Devil Fish e Neural Engine](#5-fish--controlli-devil-fish-e-neural-engine)
6. [ENV — quattro inviluppi](#6-env--quattro-inviluppi)
7. [LFO — quattro oscillatori di modulazione](#7-lfo--quattro-oscillatori-di-modulazione)
8. [MOD — matrice di modulazione](#8-mod--matrice-di-modulazione)
9. [SEQ — sequencer a 16 passi ed export WAV](#9-seq--sequencer-a-16-passi-ed-export-wav)
10. [ARP — arpeggiatore e combinazione col sequencer](#10-arp--arpeggiatore-e-combinazione-col-sequencer)
11. [FX — catena effetti a 10 slot](#11-fx--catena-effetti-a-10-slot)
12. [TUBE — preamplificatore valvolare](#12-tube--preamplificatore-valvolare)
13. [SPACE — immagine stereo](#13-space--immagine-stereo)
14. [DUCK — sidechain e controllo di fase](#14-duck--sidechain-e-controllo-di-fase)
15. [Voci, accento, glide e intonazione](#15-voci-accento-glide-e-intonazione)
16. [MIDI e DAW](#16-midi-e-daw)
17. [Preset, sessione e cosa viene salvato](#17-preset-sessione-e-cosa-viene-salvato)

---

## 1. Il percorso del suono

```
 MIDI (DAW / tastiera a schermo)
   │
   ├─► SEQ / ARP (se attivi: i tasti alimentano il motore, che suona al posto tuo)
   │
   ▼
 VOCE  (1 in Mono 303 / Mono Legato, fino a 16 in Poly)
   OSC 1 + OSC 2 + OSC 3 + SUB + NOISE      ← FM, Ring, Sync fra oscillatori
   │   (EXT MODE: l'ingresso sidechain sostituisce tutto questo)
   ▼
   mix normalizzato ─► NEURAL DRIVE ─► FILTRO 1 / FILTRO 2 (oversampling) ─► VCA ─► MUFFLER ─► pan
   │                                     ▲ inviluppi, accento,             ▲ ENV 1 × accento × velocity
   │                                     │ key track, Filter FM, matrice
   ▼
 somma delle voci
   ▼
 TUBE (preamp valvolare, sempre in testa)
   ▼
 CATENA FX — 10 slot riordinabili: Distortion, EQ, Chorus, Phaser, Delay,
             Reverb, Compressor, Space, Glitch, Beat Repeat
   ▼
 DUCK (sidechain)  ─►  OUTPUT (volume)  ─►  LIMITER (lookahead 2 ms)  ─►  uscita stereo
```

Tre idee guidano tutto lo strumento:

- **Tutto è agganciato alla DAW.** Sequencer, arpeggiatore, LFO sincronizzati,
  delay, autopan, glitch, beat repeat e sidechain leggono la posizione nel
  brano: la stessa battuta suona uguale a ogni play e dopo ogni salto della
  testina.
- **Niente click.** Ogni knob che tocca il livello o il filtro viene
  ammorbidito (10–20 ms); gli effetti accesi e spenti fanno crossfade.
- **I layer sono un basso solo.** Tutte le forme d'onda hanno la fondamentale
  nella stessa fase e gli oscillatori ripartono insieme: sommare due
  oscillatori cambia il timbro, non svuota i bassi.

---

## 2. Barra superiore, tastiera e temi

| Elemento | Funzione |
|---|---|
| **‹ / ›** | Preset precedente / successivo (fabbrica, poi utente). |
| **Display preset** | Nome e posizione (es. `1 / 36`) del preset corrente. |
| **PRESETS** | Browser per categorie: Basics, Acid, Reese, Sub, Pluck, FX, Sequencer, Poly, Hardware, Fish, Arp, Space, User. |
| **SAVE** | Salva il suono corrente come preset utente (file `.bassx`). |
| **Tema** (NEON, CYAN, PLASMA, AMBER, ICE, CUSTOM) | Colore dell'interfaccia. CUSTOM apre un selettore di colore. Il tema è salvato nella sessione, non nel preset. |
| **Voice mode** | Mono 303 / Mono Legato / Poly (vedi [§15](#15-voci-accento-glide-e-intonazione)). |
| **– / + (numero)** | Voci massime in Poly (1–16). |
| **Meter** | Livello d'uscita. |

**Fila principale** (sotto la barra, sempre visibile): WAVEFORM (forma d'onda di
OSC 1, con interruttore ON del layer principale), led GATE e ACCENT, e i
controlli di un Bass Line: TUNING, CUT OFF, RESONANCE, ENV MOD, DECAY (decay
di ENV 2, quello del filtro), ACCENT, SLIDE, DRIVE (drive del filtro 1),
VOLUME (uscita).

**Tastiera a schermo**: suona come una tastiera MIDI (velocity dall'altezza del
click). **Ridimensionamento**: trascina l'angolo in basso a destra; tutto scala
in proporzione.

---

## 3. OSC — oscillatori, sub, rumore, cross-modulazione

### Oscillatori 1, 2, 3

Tre oscillatori wavetable identici. Ogni tabella ha 10 versioni band-limited
(niente aliasing anche nelle note alte) e fino a 16 frame fra cui fare morph.

| Controllo | Cosa fa |
|---|---|
| **ON** | Accende il layer. OSC 1 ha anche il pulsante ON nella fila principale. |
| **Forma d'onda** | 16 tabelle di fabbrica + **Sample** (vedi sotto). Analog Saw/Square/Tri e le due **Acid** sono a frame singolo; le altre (Sine > Saw, Pulse Morph, Formant Bass, Reso Sweep, FM Bass, Harmonic Stack, Folded Sine, Digital, Growl, Sub Harmonic, Metallic) hanno 16 frame. Le **Acid** riproducono le imperfezioni dell'oscillatore originale: ritorno della rampa non istantaneo, duty cycle 48 %, armoniche alte smorzate. |
| **POS** | Posizione nella tabella (0–100 %), interpolata fra i frame. Su tabelle a frame singolo non ha effetto. |
| **LEVEL** | Livello del layer nel mix. |
| **PAN** | Posizione stereo del layer (-1 sinistra … +1 destra). Il filtro lavora in mono sul centro: il lato stereo viene aggiunto dopo, quindi il pan resta. |
| **FOLD** | Wavefolder applicato a ogni voce di unison prima del mix: più armoniche, timbro metallico. |
| **OCT / SEMI / FINE** | Intonazione del layer: ±3 ottave, ±12 semitoni, ±100 cent. |
| **UNISON** | Voci di unison per l'oscillatore (1–16). |
| **DETUNE** | Scostamento delle voci di unison, fino a ±50 cent, con curva non lineare (le voci esterne si scostano di più, i battimenti non si ripetono). |
| **BLEND** | 0 % = solo voci esterne (largo, cavo) … 100 % = solo voce centrale (definito). |
| **SPREAD** | Apertura stereo delle voci di unison: le più disaccordate vanno ai lati. |
| **PHASE** | Voce singola: **fase di partenza** dell'onda (0 % = allineata agli altri layer, 50 % = invertita). Con unison > 1: casualità della fase iniziale delle voci, per non sommare tutte le voci in fase al primo campione. |
| **RETRIG** *(solo automazione/preset)* | On: a ogni nota non legata l'oscillatore riparte dalla fase di partenza (attacco identico a ogni nota). Off: gira libero; resta comunque allineato agli altri layer perché tutti partono insieme. |

**Somma dei layer.** I livelli di OSC 1–3, SUB e NOISE vengono sommati; se la
somma supera 1 il mix viene normalizzato, così aggiungere un layer cambia il
timbro senza spingere di più il filtro. Tutte le forme d'onda hanno la
fondamentale allineata (zero in salita a fase 0): due layer alla stessa nota
sommano le fondamentali, non si cancellano.

**Sample → wavetable.** Trascina un file **WAV, AIFF, FLAC, OGG o MP3** su un
oscillatore:

1. vengono letti al massimo 20 s, convertiti in mono, saltando silenzio
   iniziale e finale;
2. se il suono ha un'intonazione (rilevata con un algoritmo YIN fra 30 Hz e
   1 kHz), ogni frame è **un ciclo vero**: la wavetable suona intonata sulla
   tastiera. Altrimenti (rumore, batteria) ogni frame è una finestra di 2048
   campioni e diventa un timbro;
3. 16 frame distribuiti lungo il campione — POS li attraversa, cioè percorre
   l'evoluzione del suono originale;
4. ogni frame è reso band-limited come le tabelle di fabbrica.

L'oscillatore su cui lo lasci passa sulla forma **Sample** e si accende (OSC 2
e 3, se avevano il livello a zero, vanno all'80 %). La wavetable è **unica per istanza**: tutti gli oscillatori su
"Sample" usano l'ultimo campione caricato. Viene salvata nella sessione della
DAW, quindi la sessione si riapre anche se il file originale non c'è più. Non
viene salvata nei preset utente: un preset con "Sample" senza campione
caricato suona la prima tabella (Analog Saw).

### Sub / Noise / Cross Modulation

| Controllo | Cosa fa |
|---|---|
| **SUB** + forma | Sub-oscillatore: Sine (ricorsivo, senza aliasing), Triangle, Square (polyBLEP). In fase con gli oscillatori; riparte con OSC 1. |
| **SUB OCT** | -2, -1, 0 ottave rispetto alla nota. |
| **SUB LVL** | Livello del sub. |
| **NOISE** + colore | Rumore White o Pink (Voss-McCartney, errore < 0.3 dB 20 Hz–20 kHz). |
| **NOISE** (knob) | Livello del rumore. |
| **FM source / target** | Phase modulation: la sorgente (Osc 1, Osc 2, Osc 3 o Sub) modula la fase del target (Osc 1, 2 o 3). Se sorgente = target è **auto-FM** (feedback dal campione precedente). Il Sub può fare da modulatore anche se è spento. |
| **FM** (knob) | Indice di modulazione, fino a mezzo ciclo: oltre, su un basso, si ottiene rumore invece che timbro. |
| **RING** | Osc 1 × Osc 2, in crossfade col mix asciutto (0 % = niente ring, 100 % = solo ring). |
| **SYNC** | Hard sync: OSC 2 riparte a ogni ciclo di **OSC 1**. L'altezza la dà OSC 1; SEMI/FINE di OSC 2 cambiano il timbro (le formanti del sync). Correzione BLEP sul salto, quindi senza aliasing. |

---

## 4. FILTER — due filtri e routing

### Filtro 1 e Filtro 2

| Controllo | Cosa fa |
|---|---|
| **Modello** | Vedi tabella sotto. Filtro 2 parte da **Bypass**. |
| **CUTOFF** | 20 Hz – 18 kHz. Le modulazioni lavorano in ottave (sweep uniforme su tutta la tastiera). |
| **RESO** | Risonanza 0–100 %. Sui modelli ladder si avvicina all'auto-oscillazione. |
| **DRIVE** | Guadagno in ingresso al filtro (1–24): satura gli stadi non lineari. Il livello d'uscita è compensato con la radice del guadagno, quindi cambia il carattere più del volume. |
| **ENV AMT** | Quanto l'inviluppo scelto muove il cutoff: ±100 % = ±6.5 ottave. |
| **ENV SRC** | Quale inviluppo (Env 1–4) muove il cutoff. Default: Env 2 sul filtro 1, Env 3 sul filtro 2. |
| **KEY TRK** | Il cutoff segue la nota: 100 % = un'ottava di cutoff per ogni ottava suonata (negativo: al contrario). |
| **BASS** *(solo filtro 1)* | Compensazione della perdita di bassi con la risonanza: 0 % = carattere originale del ladder a diodi (i bassi calano quando la risonanza sale), 100 % = basso pieno anche a risonanza alta. |

| Modello | Carattere |
|---|---|
| **Diode 18dB / Diode 24dB** | Ladder a diodi, il filtro del Bass Line. Poli distribuiti (pendenza effettiva ~18 dB/ott vicino al cutoff), feedback compresso asimmetricamente dal diodo: lo "squelch", la risonanza che si auto-limita invece di fischiare pulita. |
| **Moog LP24 / LP12 / BP / HP** | Ladder a transistor: risonanza che canta. HP lavora sull'ingresso del ladder, quindi taglia davvero i bassi. |
| **SVF LP12 / LP24 / HP12 / HP24 / BP12 / Notch / Peak** | State variable TPT: pulito, preciso, stabile sotto modulazione veloce. |
| **Comb** | Risonatore accordato (ritardo = periodo del cutoff) con smorzamento nel loop: suoni metallici e di corda. |
| **Formant** | Tre passa-banda che percorrono le vocali A → E → I → O → U muovendo il cutoff. |
| **Bypass** | Nessun filtro. |

Tutti i modelli sono zero-delay-feedback con il feedback risolto da due
iterazioni di Newton-Raphson: la risonanza resta intonata anche a 8 kHz.
Cutoff, risonanza e drive sono ammorbiditi su 10 ms: nessuno zipper.

### Routing

| Controllo | Cosa fa |
|---|---|
| **Serial 1>2** | Il segnale passa nel filtro 1 e poi nel 2. |
| **Parallel** | I due filtri ricevono lo stesso segnale; **BLEND** sceglie la miscela (0 % = solo F1, 100 % = solo F2). |
| **Split (F1 low / F2 high)** | Il filtro 2 riceve ciò che il filtro 1 scarta (ingresso − uscita di F1); **BLEND** dosa quanto di quella parte rientra. |
| **QUALITY** | Oversampling della sola sezione filtro: Off / 2x / 4x / 8x, con decimazione Butterworth del 4° ordine. È lì che nasce l'aliasing (gli oscillatori sono già band-limited). È un'impostazione di sessione. |

### Grafico RESPONSE

Mostra la risposta **misurata** dei filtri con le impostazioni correnti
(modello, cutoff, risonanza, drive, routing e blend): curva verde = totale,
arancio = filtro 1, tenue = filtro 2, con i marker dei cutoff. Non è un
disegno teorico: è un impulso fatto passare nei filtri veri.

---

## 5. FISH — controlli Devil Fish e Neural Engine

Ricostruisce il pannello di un Bass Line modificato (Devil Fish). Alcuni knob
sono gli stessi parametri presenti altrove, raccolti qui come sul pannello
originale.

### Bass Line

| Controllo | Parametro | Cosa fa |
|---|---|---|
| **EXT MODE** | `df_ext_mode` | L'ingresso sidechain dello strumento **sostituisce gli oscillatori** e passa da Neural Drive, filtro e VCA, comandato dalle note. In Ableton: attiva l'ingresso sidechain di BASSX dal menu del dispositivo e scegli la traccia. |
| TUNING | `master_tune` | Accordatura generale ±100 cent (impostazione di sessione). |
| CUT OFF, RESONANCE, ENVMOD | `f1_*` | Filtro 1. |
| DECAY | `env2_decay` | Decay dell'inviluppo del filtro. |
| ACCENT | `accent_amount` | Quantità di accento. |

### Devil Fish

| Controllo | Parametro | Cosa fa |
|---|---|---|
| **SLIDE TIME** | `glide_time` | Tempo dello slide. Curva RC sulla tensione di controllo (in ottave), come l'originale: lo slide copre il 90 % dell'intervallo nel tempo impostato, uguale per ogni intervallo. |
| **SWEEP AMT.** | `accent_filter` | Quanto l'accento apre il filtro (fino a 4 ottave). |
| **ENV. ATTACK** | `df_env_attack` | Attacco morbido (0.3–30 ms) dell'inviluppo d'ampiezza, **solo sulle note non accentate**: le accentate restano secche. |
| **ACC. DECAY** | `accent_decay` | Durata del circuito di accento. |
| **ACC. VOL.** | `accent_vca` | Quanto l'accento alza il volume (fino a +90 %). |
| **VIB. SPEED / VIB. DEPTH** | `df_vib_speed`, `df_vib_depth` | Vibrato dedicato, seno per voce, fino a ±1 semitono. |

### Neural Engine

| Controllo | Parametro | Cosa fa |
|---|---|---|
| **NEURAL DRIVE** | `df_neural_drive` | Preamp a diodi con memoria, modellato da una rete **GRU a 12 stati** condizionata sul drive, **prima del filtro** come l'overdrive di un Bass Line modificato. Livello compensato: spinge la saturazione, non il volume. |
| **FILTER FM** | `df_filter_fm` | L'uscita del VCA modula il cutoff a frequenza audio (fino a 3 ottave), con un campione di ritardo come il circuito reale: ruvido, metallico. |
| **MUFFLER** | `df_muffler` | Saturazione morbida sull'uscita del VCA: arrotonda i picchi degli accenti, il basso resta pieno. |
| **SWEEP SPEED** | `df_sweep_speed` | Velocità di carica del circuito di accento: 0 = istantanea; più alta = le raffiche di accenti ravvicinati "gonfiano" il filtro gradualmente. |
| **EXT GAIN** | `df_ext_gain` | Guadagno dell'ingresso esterno in Ext Mode (±24 dB). |

**Output Scope**: oscilloscopio dell'uscita.

---

## 6. ENV — quattro inviluppi

Quattro inviluppi **DAHDSR**: Env 1 = ampiezza (VCA), Env 2 = filtro (default
del filtro 1), Env 3 e 4 liberi (matrice, filtro 2).

| Controllo | Cosa fa |
|---|---|
| **DELAY** | Attesa prima dell'attacco (0–4 s): gesti ritmici senza usare un LFO. |
| **ATTACK** | 0.2 ms – 8 s. |
| **HOLD** | Tempo al massimo prima del decay (0–4 s). |
| **DECAY** | 0.5 ms – 16 s. |
| **SUSTAIN** | Livello tenuto. |
| **RELEASE** | 0.5 ms – 16 s dopo il note-off. |
| **CRV A / CRV D / CRV R** | Curva di ciascun segmento, da -1 (parte piano e accelera) a +1 (parte forte e rallenta); 0 = scarica di condensatore, come un analogico. La curva cambia la forma, non il livello di sustain. |
| **VEL** | Quanto la velocity scala l'intero inviluppo. |

Le curve sono esponenziali vere: un decay di 200 ms sul cutoff passa il 30 %
del tempo nella metà alta della corsa — è il "morso" seguito dalla coda lunga.
Nota legata (slide): gli inviluppi **non** ripartono.

---

## 7. LFO — quattro oscillatori di modulazione

| Controllo | Cosa fa |
|---|---|
| **Forma** | Sine, Triangle, Saw Up, Saw Down, Square, Pulse 25 %, Sample & Hold, Smooth Random, Exp Ramp, Chaos (mappa logistica al bordo del caos: movimento continuo mai periodico, ma riproducibile). |
| **RATE** | Frequenza libera (fino a 60 Hz). |
| **SYNC** + divisione | Sincronizzato alla DAW su 16 divisioni (8 bars … 1/32, terzinate e puntate). Ancorato alla posizione nel brano, non a un contatore. |
| **DEPTH** | Ampiezza dell'uscita (modulabile dalla matrice per LFO 1 e 2). |
| **PHASE** | Offset di fase (0–100 % = un giro). |
| **Trigger** | **Free** gira sempre; **Note** riparte a ogni nota; **Legato** riparte solo sulla prima nota di una frase legata; **One Shot** fa un solo ciclo per nota e si ferma (diventa un inviluppo). |
| **UNI** | Uscita unipolare 0…1 invece di -1…+1. |
| **FADE** | Fade-in dopo il note-on (0–8 s). |
| **SMOOTH** | Lisciatura dell'uscita in ms: le forme con salti (quadra, rampe, S&H) hanno sempre un fronte, così non fanno click sul cutoff o sul pitch. |

Gli LFO non suonano da soli: si collegano alle destinazioni nella pagina MOD.

---

## 8. MOD — matrice di modulazione

16 slot. Ogni slot: **sorgente → destinazione**, **profondità** (-100 … +100 %)
e una sorgente **VIA** che scala la profondità (es. "LFO 1 sul cutoff, ma solo
quanto dice la mod wheel" in un solo slot).

**Sorgenti (18):** Env 1 (Amp), Env 2 (Filter), Env 3, Env 4, LFO 1–4,
Velocity, Key Track, Mod Wheel, Pitch Bend, Aftertouch, Accent, Random (note)
(bipolare, fisso per tutta la nota), Random (uni), Step Seq (valore del passo
del sequencer), Constant.

**Destinazioni (50)** e scala di una profondità del 100 %:

| Gruppo | Destinazioni | 100 % vale |
|---|---|---|
| Oscillatori | Osc1–3 Pitch, All Osc Pitch | 1 ottava |
| | Osc1–3 Level, Position, Fold, Pan; Sub Level, Noise Level, FM Amount, Ring Mod, Unison Detune | tutta la corsa del parametro |
| Filtri | F1/F2 Cutoff | 8 ottave |
| | F1/F2 Reso, Filter Blend | tutta la corsa |
| | F1/F2 Drive | ×8 / ÷8 |
| Ampiezza | Amp Level, Amp Pan | tutta la corsa |
| Modulatori | LFO1–4 Rate | ±4 ottave di velocità |
| | LFO1–2 Depth | tutta la corsa |
| | Env2/Env3 Decay | ±3 ottave di tempo |
| Effetti | FX Drive, FX Delay Mix, FX Reverb Mix, FX Chorus Depth, FX Phaser Freq, FX Delay Fdbk, Space Pan, Space Width, Autopan Depth | tutta la corsa |
| Fish | Fish Filter FM, Neural Drive, Vibrato Depth | tutta la corsa |

Le destinazioni degli effetti sono globali: le modula la voce suonata più di
recente. Le destinazioni lente (rate degli LFO, tempi di decay, detune) sono
ricalcolate ogni 32 campioni (< 1 ms); tutte le altre a ogni campione.

La matrice è salvata nel preset e nella sessione.

---

## 9. SEQ — sequencer a 16 passi ed export WAV

### Controlli generali

| Controllo | Cosa fa |
|---|---|
| **RUN** | Accende il sequencer. Con RUN acceso i tasti non suonano direttamente: alimentano il motore. |
| **Divisione** | Durata del passo (8 bars … 1/32). Default 1/16. |
| **Direzione** | Forward, Backward, Ping-Pong, Random (casuale ma deterministico: uguale a ogni play). |
| **KEY** | Key Follow. I pattern sono scritti rispetto a **La2** (nota MIDI 45): con KEY acceso l'ultimo tasto premuto prende il posto del La2 e traspone il pattern; spento, il pattern suona sempre in La. |
| **PATTERN** | Pattern 1–8 (ognuno con i suoi 16 passi). |
| **LENGTH** | Passi attivi (1–16). |
| **SWING** | Ritarda il 2°, 4°, 6°… passo: al 75 % di 3/8 di passo. |
| **OCTAVE** | Trasposizione del pattern ±2 ottave. |
| **GATE** | Durata della nota rispetto al passo (5–100 %). |

### Il passo

Ogni colonna è un passo; il led si accende sul passo che suona.

| Elemento | Cosa fa |
|---|---|
| **Barra nota** | Semitoni rispetto alla nota base (-24 … +24). Doppio click = 0. |
| **GATE** | Il passo suona (off = pausa). |
| **ACC** | Accento: velocity 100 % + circuito di accento (filtro e volume). Senza: velocity 75 %. |
| **SLD** | Slide verso questo passo: la nota precedente resta aperta e scivola qui senza ripartire l'inviluppo (semantica 303: il flag sta sul passo **di arrivo**). |
| **TIE** | Lega questo passo al precedente senza nuovo attacco. |
| **xN** | Ratchet: 1–4 ripetizioni dentro il passo (click per ciclare). |
| **Barra verde** | Probabilità che il passo suoni (0–100 %). Deterministica: lo stesso passo nella stessa battuta decide sempre uguale. |

### Trasporto

- **DAW in play:** il passo è funzione della posizione nel brano. Loop, salti
  della testina e render offline sono identici campione per campione.
- **DAW ferma:** suona solo mentre tieni premuti dei tasti (o con il LATCH
  dell'arpeggiatore), con un orologio interno che riparte dal passo 1 alla
  prima nota. Fermando la DAW il motore si ferma con lei.

### EXPORT WAV

1. scegli la lunghezza (1–16 battute);
2. **EXPORT WAV** renderizza il loop in background (non blocca l'audio);
3. trascina il riquadro del file direttamente in una traccia della DAW, o
   cliccalo per aprire la cartella.

Il render usa il BPM della DAW (120 se non disponibile), tutto il suono
corrente (seq/arp, preset, effetti) e le note che stai tenendo (anche in
latch). Fa due giri e tiene il **secondo**, così code di delay e riverbero
del giro precedente sono dentro il loop. WAV stereo 24 bit, al sample rate
della sessione.

---

## 10. ARP — arpeggiatore e combinazione col sequencer

| Controllo | Cosa fa |
|---|---|
| **ON** | Accende l'arpeggiatore. I tasti tenuti diventano l'accordo da arpeggiare. |
| **LATCH** | Le note restano dopo aver rilasciato i tasti; un nuovo accordo sostituisce il precedente. |
| **SLIDE** | Ogni nota scivola nella successiva. |
| **Modo** | Up, Down, Up/Down, Down/Up, As Played (ordine di pressione), Random (deterministico), Chord (tutte insieme). |
| **Divisione** | Velocità (8 bars … 1/32). |
| **Accento** | Off, oppure accento ogni 2…8 note. |
| **OCTAVES** | Estensione su 1–4 ottave. |
| **GATE / SWING** | Come nel sequencer. |

### Seq + Arp (entrambi accesi)

| Modo | Risultato |
|---|---|
| **Layer** | Suonano insieme, ognuno con il suo ritmo. |
| **Seq Rhythm > Arp Notes** | Ritmo, accenti e slide dal sequencer; altezze dall'accordo tenuto, nell'ordine dell'arpeggiatore. |
| **Seq Transposes Arp** | L'arpeggio segue la trasposizione dei passi del sequencer. |

---

## 11. FX — catena effetti a 10 slot

### La catena

La fila in alto ha **10 slot**: ogni slot sceglie un effetto, l'ordine degli
slot è l'ordine della catena (anche automatizzabile). Un effetto assegnato a
più slot suona solo nel primo; un effetto in nessuno slot è fuori catena.
**DEFAULT** ripristina l'ordine: Distortion, EQ, Chorus, Phaser, Delay,
Reverb, Compressor, Space, Glitch, Beat Repeat.

Ogni effetto acceso/spento fa crossfade; delay, riverbero, chorus e phaser
spenti o fuori catena si fermano e alla riaccensione ripartono con le code
vuote (niente frasi vecchie che riaffiorano).

### Distortion

| Controllo | Cosa fa |
|---|---|
| **ON** + tipo | **Soft Clip**, **Tube** (asimmetrica, armoniche pari), **Hard Clip**, **Foldback**, **Wave Shaper** (wavefold), **Bit Crush**, **Diode** (trasparente sotto soglia, feroce sui picchi), **Fuzz** (transistor in saturazione profonda). |
| **DRIVE** | Quantità di saturazione (rampa di 20 ms). |
| **TONE** | Passa-basso dopo la distorsione (200 Hz – 20 kHz). |
| **MIX** | Secco/distorto. |
| **BITS / S.RATE** | Solo per Bit Crush: profondità (1–16 bit) e riduzione di frequenza (1–64×). |

Oversampling 4x FIR a fase lineare: niente aliasing. Introduce latenza,
dichiarata alla DAW.

### Equaliser

Tre bande: **LO** shelf (20 Hz – 1 kHz), **MID** peak con **Q** (40 Hz –
12 kHz, Q 0.2–12), **HI** shelf (1–20 kHz), ognuna ±24 dB. È sempre in
catena (a 0 dB non fa nulla).

### Chorus / Phaser

| Controllo | Cosa fa |
|---|---|
| **CHO** | Chorus a più voci per canale con LFO sfasati e rapporti di frequenza diversi (allarga invece di "ondeggiare"). Ritardo base 12 ms. |
| RATE / DEPTH / SPREAD / MIX | Velocità, profondità, apertura stereo, miscela. Numero di voci (1–3): `chorus_voices`, da automazione/preset. |
| **PHA** | Phaser allpass con feedback dopo l'ultimo stadio (notch strette e risonanti). Stadi 2–12: `phaser_stages`, da automazione/preset. |
| RATE / DEPTH / CENTRE / FEEDBACK / MIX | Velocità, ampiezza dello sweep, frequenza centrale, feedback (±95 %), miscela. |

### Delay

| Controllo | Cosa fa |
|---|---|
| **ON / SYNC / P-P** | Acceso; tempo a divisioni della DAW (L e R separati) o in ms; ping-pong. |
| **Divisioni L / R** | Default 1/8 e 1/8 puntato. In ms: `delay_time_l/r` (1 ms – 3 s). |
| **FEEDBK** | Fino a 105 %: oltre 100 % l'eco cresce, limitato dai filtri nel loop. |
| **MIX** | Miscela. |
| **LPF / HPF** | Filtri **dentro il loop**: ogni ripetizione si scurisce e perde i bassi (HPF default 220 Hz: le ripetizioni non impastano la fondamentale del basso). |

### Reverb

FDN a 8 linee con matrice di Householder. **SIZE**, **DECAY** (0.1–20 s),
**DAMP** (assorbimento degli alti), **LO CUT** (via i bassi dalla coda),
**PRE** (pre-delay fino a 240 ms), **WIDTH**, **MIX**. Modulazione delle
linee: `reverb_mod`, da automazione/preset.

### Comp / Limit

| Controllo | Cosa fa |
|---|---|
| **COMP / RMS** | Compressore feed-forward; RMS on = rilevamento RMS, off = peak. |
| THRESH / RATIO / ATTACK / RELEASE / KNEE / MAKEUP / MIX | Soglia (-60…0 dB), rapporto (1–60), attacco (0.05–200 ms), rilascio (5–2000 ms), ginocchio (0–24 dB), guadagno di recupero, compressione parallela. |
| **LIM / CEILING** | Limiter con lookahead di 2 ms, **sempre ultimo stadio** dopo il volume d'uscita: il picco non supera il tetto (default -0.3 dB). Il tetto è un'impostazione di sessione. |

### Glitch

A ogni passo della griglia (**RATE**), con probabilità **AMOUNT**, il passo
appena suonato viene rimpiazzato da uno di:

- **Stutter** — il primo quarto del passo ripetuto 4 volte;
- **Reverse** — il passo al contrario;
- **Tape Stop** — velocità da 1 a 0, come un nastro che si ferma;
- **Gate** — tagli a ottavi del passo;
- **Random** — ogni passo sceglie uno dei quattro.

**SEED** cambia la sequenza di scelte; con lo stesso seed un loop glitcha
**uguale a ogni giro** (come una clip editata). **MIX** dosa il risultato.
Nessuna latenza (rilegge il passo precedente); bordi con dissolvenza di 3 ms.

### Beat Repeat

Ogni **INTERVAL** (es. 1 bar), con probabilità **CHANCE**, cattura un
frammento lungo **GRID** e lo ripete per **GATE**; **DECAY** abbassa ogni
ripetizione, **MIX** dosa. Il primo frammento passa mentre viene registrato:
nessuna latenza. È il classico rullo di fine battuta.

---

## 12. TUBE — preamplificatore valvolare

Il motore del plugin ETA-1: modelli Koren di triodo e pentodo risolti sulla
retta di carico, sag dell'alimentatore, blocking da grid-leak, trasformatore
d'uscita. Sta **in testa alla catena** effetti, come un preamplificatore: la
valvola vede il basso pulito e la sua compressione dinamica reagisce agli
accenti.

| Controllo | Cosa fa |
|---|---|
| **ON** + modello | 26 profili: 6 voicing da groovebox (SX/MX TUBE, DRIVE, EXTREME) e 20 valvole reali (12AX7/ECC83, 12AT7, 12AU7, 5751, 6DJ8, 6922, EF86, 6SN7, 6SL7, 12BH7A, EL84, EL34, 6L6GC, 6V6GT, KT66, KT77, KT88, 6550, 300B, 2A3). I voicing MX DRIVE e MX EXTREME lasciano i bassi profondi fuori dalla valvola (basso stretto). |
| **DRIVE** | Pilotaggio della griglia. |
| **BIAS** | Punto di lavoro: sposta l'asimmetria (armoniche pari ↔ dispari). |
| **SAG** | Quanto l'alimentatore "cede" sui picchi (compressione dinamica). |
| **CHARACTER** | Quantità di colorazione del modello. |
| **AGE** | Valvola consumata: meno emissione (stadio più debole) e piccoli sbilanciamenti propri di ogni canale. |
| **WARMTH / PRESENCE** | Shelf ±6 dB sui bassi e sui medio-alti. |
| **TONE** | Passa-basso finale (1–20 kHz). |
| **MIX** | Parallelo secco/valvola. Il ramo secco è ritardato esattamente quanto l'oversampling: niente effetto pettine. |
| **OUTPUT** | Livello d'uscita dello stadio (-24…+12 dB). |

Oversampling 4x FIR a fase lineare. Latenza costante anche a stadio spento:
accenderlo non sposta la traccia nel tempo. Il grafico **Transfer** mostra la
curva di trasferimento corrente.

---

## 13. SPACE — immagine stereo

È anche un effetto della catena FX (slot "Space"): la pagina SPACE ne mostra i
controlli e un goniometro.

| Controllo | Cosa fa |
|---|---|
| **ON** | Attiva lo stadio. |
| **PAN** | Posizione dell'intero suono. |
| **WIDTH** | Larghezza M/S: 0 % = mono, 100 % = invariato, 200 % = doppia. |
| **BASS MONO** | Sotto la frequenza scelta il lato stereo viene rimosso (Linkwitz-Riley 24 dB/ott): bassi mono e compatibili col club. Off = disattivo. |
| **HAAS** | Ritardo di un canale (0–30 ms): allargamento per precedenza. |
| **ROTATION** | Rotazione dell'immagine stereo. |
| **Autopan DEPTH + rate** | Pan automatico sincronizzato alla DAW (divisioni come gli LFO). |

**Goniometer**: forma dell'immagine stereo e correlazione di fase.

---

## 14. DUCK — sidechain e controllo di fase

Abbassa il volume con una curva ritmica, come un compressore sidechain sulla
cassa, ma senza cassa: è l'ultimo stadio prima del volume d'uscita.

| Controllo | Cosa fa |
|---|---|
| **ON** | Attiva il ducking. |
| **Trigger** | **Sync**: la curva gira col tempo della DAW. **MIDI ch16**: ogni nota sul **canale MIDI 16** fa ripartire la curva (le note sul canale 16 non suonano). **Audio (Ext In)**: un colpo sull'ingresso sidechain sopra **THRESH** fa ripartire la curva. |
| **Rate** | Durata di un ciclo della curva, a divisioni della DAW (8 bars … 1/32). |
| **Forma** | Classic, Hard Pump, Smooth, Short Tick, Long Swell, Gate, Double, Slow Attack, **Custom**. |
| **DEPTH** | Profondità del ducking (100 % = silenzio al punto più basso). |
| **PHASE** | Sposta la curva sul battito (±50 % del ciclo): il taglio può anticipare o seguire la cassa. |
| **SMOOTH** | Lisciatura della curva (0.1–20 ms): toglie i click sui fronti ripidi. |
| **BAND LO / BAND HI** | Il ducking agisce **solo su questa banda**: es. 20–150 Hz abbassa solo i bassi e lascia passare gli alti. A banda piena (20 Hz – 20 kHz) agisce su tutto. |
| **THRESH** | Soglia del trigger Audio. |

### Curva (editor)

- click su uno spazio vuoto = nuovo punto (fino a 32);
- trascina un punto per spostarlo (il primo e l'ultimo restano ai bordi);
- **tasto destro** (o **Alt**) + trascinamento su un segmento = curvatura;
- doppio click su un punto = cancellalo.

Modificare la curva passa automaticamente la forma su **Custom** partendo da
quella che c'era. La curva Custom è salvata nella sessione.

### Spectrum

Spettro prima (tenue) e dopo il ducking, la curva di guadagno e la testina che
mostra la fase corrente. Il sidechain è un'impostazione di **sessione**:
cambiare preset non lo spegne.

---

## 15. Voci, accento, glide e intonazione

| Controllo | Cosa fa |
|---|---|
| **Voice mode** | **Mono 303**: una voce; una nota che arriva mentre un'altra è premuta scivola senza ripartire (slide); ogni nota staccata riparte da capo, anche se il retrigger dell'oscillatore è spento. **Mono Legato**: una voce, legato sulle note sovrapposte. **Poly**: fino a **POLY VOICES** voci (1–16); quando servono voci nuove ruba prima quelle già in release avanzato. |
| **Ritorno alla nota tenuta** | In mono, rilasciando la nota più recente si torna con uno slide all'ultima nota ancora premuta (un trillo legato non lascia buchi). |
| **Glide mode** | Legato only (solo note legate), Always (ogni nota), Off. |
| **Glide** | Tempo di glide/slide (1 ms – 2 s). |
| **Accent** | Quantità generale di accento. |
| **Accent Threshold** | Velocity MIDI dalla quale una nota è accentata (default 100): si comanda l'accento dal piano roll senza automazione. |
| **Accent > Filter / Accent > VCA** | Quanto l'accento apre il filtro e alza il volume. |
| **Accent Decay** | Durata dell'accento. Il circuito accumula carica fra note ravvicinate (fino a 1.6×): è ciò che fa "pompare" le raffiche di sedicesimi accentati. |
| **Velocity Depth** | Quanto la velocity cambia il volume. |
| **Master Tune** | ±100 cent (sessione). |
| **Bend Range** | Escursione del pitch bend, 0–24 semitoni (sessione). |

---

## 16. MIDI e DAW

| Messaggio | Effetto |
|---|---|
| Note on/off | Suonano (o alimentano SEQ/ARP se attivi). Velocity ≥ soglia di accento = nota accentata. |
| Pitch bend | ± Bend Range semitoni su tutte le voci. |
| CC 1 (mod wheel) | Sorgente Mod Wheel della matrice. |
| Aftertouch (canale o polifonico) | Sorgente Aftertouch della matrice. |
| CC 123 / All Sound Off | Spegne tutte le note. |
| CC 64 (sustain) | Ignorato di proposito (su un mono legato sarebbe ambiguo). |
| Canale 16 | Con DUCK in trigger MIDI: fa ripartire la curva, non suona. |

**Uscita**: stereo (o mono, sommata). **Ingresso sidechain** opzionale: serve a
EXT MODE (FISH) e al trigger Audio di DUCK.

**Latenza**: oversampling di Tube e Distortion + lookahead del limiter;
dichiarata alla DAW, che la compensa.

**Stop della DAW**: seq e arp si fermano con la DAW anche con LATCH attivo; a
trasporto fermo ripartono solo suonando un tasto.

**Determinismo**: a parità di impostazioni e di posizione nel brano, il
render è identico a ogni esportazione (anche Random, probabilità, glitch).

---

## 17. Preset, sessione e cosa viene salvato

**Preset di fabbrica**: 36 in 12 categorie (elenco in
[`PARAMETRI.md`](PARAMETRI.md#preset-di-fabbrica)). Ogni preset porta i suoi
inviluppi, la matrice di modulazione e i pattern del sequencer.

**Preset utente**: SAVE scrive un file `.bassx` in

- Windows: `%APPDATA%\BASSX\Presets`
- macOS: `~/Library/BASSX/Presets`

Un preset utente con lo stesso nome di uno di fabbrica resta un preset distinto
(menu e salvataggio usano l'indice, non il nome).

**Impostazioni di sessione** — non cambiano quando scegli un preset, perché
riguardano il mix o il setup, non il suono:

- tutto il **sidechain** (`sc_*`);
- **oversampling** del filtro, **master tune**, **bend range**, **ceiling** del
  limiter;
- tema e colore dell'interfaccia.

**Sessione della DAW** — salva tutto: parametri, preset corrente (nome e
indice), matrice, pattern, curva del sidechain, wavetable da campione, tema.
I preset **non** sono esposti alla DAW come "programmi": così la DAW non può
ricaricare un preset sopra la sessione riaperta.
