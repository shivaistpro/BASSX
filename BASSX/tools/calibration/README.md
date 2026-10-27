# Calibrazione BASSX su registrazioni di TB-303

1. `transcribe.py <wav> <bpm> <pattern.json>` — trascrive un loop a 16esimi (note, accenti, slide/tie, gate).
2. `BassXMatch "<preset>" pattern.json out.wav [param=valore ...]` — risuona il pattern su BASSX
   (build con `-DBASSX_BUILD_MATCH=ON`).
3. `avgnote.py` — inviluppo medio della nota (303 vs BASSX); `constants.py` — attacco, rilascio, accento, slide.
4. `fit.py` / `fit4.py` — fit dei parametri sullo spettro armonico medio (preset "TB Recorded").

Risultati sui 44 loop (`Acid.Techno.Grooves/TB 303 Sequences`): livello pieno ~60% del passo, attacco
~5 ms, slide 10-90% ~25-30 ms, accento +0.8 dB e brillantezza x2.1. Lo spettro medio ha la
fondamentale ~8-10 dB sotto quella di BASSX: può essere la perdita di bassi del 303 o un taglio fatto
in produzione del sample pack, per questo è nel preset e non nel modello del filtro.
Percorsi: `BASSX_REFS` = cartella dei WAV di riferimento (default `refs/`), `BASSX_MATCH` = eseguibile
`BassXMatch` (default: cercato nel PATH). I pattern trascritti stanno in `bx/` accanto agli script.
