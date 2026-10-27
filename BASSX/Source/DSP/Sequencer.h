/*
  ==============================================================================

    BASSX — Sequencer.h

    Sequencer a 16 passi, arpeggiatore e motore che li fa suonare, da soli o
    combinati, sempre agganciati al trasporto dell'host.

    Aggancio al trasporto e non a un contatore interno:
      Il passo attivo è una funzione pura della posizione nel brano
      (ppqPosition): la stessa battuta suona identica a ogni riproduzione e i
      loop restano allineati senza resettare nulla. Anche direzione Random e
      probabilità dei passi sono deterministiche — derivano da un hash del
      numero assoluto di passo — quindi un pattern "casuale" suona uguale a
      ogni play e resta in sync dopo un salto della testina.

    Trasporto fermo:
      Il motore suona solo mentre si tengono premuti dei tasti (o con il latch
      dell'arpeggiatore), con un orologio interno che riparte dal passo 0 alla
      prima nota. È il comportamento di un sequencer hardware in modalità
      "key trigger" ed è ciò che serve nello standalone.

    Scheduling:
      Ogni nota produce eventi futuri (note-off a fine gate, ripetizioni dei
      ratchet) che possono cadere in blocchi successivi. Vivono in una coda a
      capacità fissa con tempo assoluto in campioni; a ogni blocco si estraggono
      quelli che cadono dentro il blocco, ordinati per posizione (note-off prima
      dei note-on alla stessa posizione, così una nota ribattuta ritriggera).

    Slide e tie (semantica 303):
      Il flag sta sul passo DI ARRIVO. Quando il passo successivo ha slide o
      tie, il gate del passo corrente resta aperto fino al passo successivo:
      senza questo, il note-off a fine gate chiuderebbe l'inviluppo prima dello
      slide, che è l'errore classico dei sequencer software "alla 303".

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include "Common.h"
#include "LFO.h"

namespace bassx
{

//==============================================================================
struct SeqStep
{
    int   note = 0;          // semitoni rispetto alla nota base
    bool  gate = true;
    bool  accent = false;
    bool  slide = false;
    bool  tie = false;
    float velocity = 0.8f;
    int   ratchet = 1;       // 1..4 ripetizioni dentro il passo
    float probability = 1.0f;

    bool operator== (const SeqStep& o) const noexcept
    {
        return note == o.note && gate == o.gate && accent == o.accent
            && slide == o.slide && tie == o.tie && ratchet == o.ratchet
            && std::abs (velocity - o.velocity) < 1.0e-6f
            && std::abs (probability - o.probability) < 1.0e-6f;
    }
};

//==============================================================================
/** Evento prodotto dal motore, consumato dal processore. */
struct SeqEvent
{
    enum class Type { NoteOn, NoteOff };

    Type  type = Type::NoteOn;
    int   samplePosition = 0;
    int   midiNote = 60;
    float velocity = 0.8f;
    bool  accent = false;
    bool  slide = false;
    float modValue = 0.0f;   // sorgente "Step Seq" della matrice, -1..1
};

//==============================================================================
/** Hash deterministico a 64 bit (finalizzatore di SplitMix64). */
inline uint64_t stepHash (uint64_t x) noexcept
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline float stepHash01 (long long k, uint64_t salt) noexcept
{
    return (float) (stepHash ((uint64_t) k ^ (salt * 0x632BE59BD9B4E019ull)) >> 40) * (1.0f / 16777216.0f);
}

//==============================================================================
/**
    Memoria dei pattern e regole per passare dal numero assoluto di passo al
    passo del pattern. Non conosce il tempo: il motore la interroga.
*/
class Sequencer
{
public:
    static constexpr int kNumSteps = 16;
    static constexpr int kNumPatterns = 8;

    enum class Direction { Forward = 0, Backward, PingPong, Random };

    Sequencer() { loadFactoryPatterns(); }

    //==========================================================================
    void setPattern (int index) noexcept   { patternIndex = juce::jlimit (0, kNumPatterns - 1, index); }
    void setLength (int steps) noexcept    { length = juce::jlimit (1, kNumSteps, steps); }
    void setDirection (int d) noexcept     { direction = (Direction) juce::jlimit (0, 3, d); }

    int getPattern() const noexcept { return patternIndex; }
    int getLength() const noexcept  { return length; }

    int getCurrentStep() const noexcept { return currentStep.load (std::memory_order_relaxed); }
    void setCurrentStep (int s) noexcept { currentStep.store (s, std::memory_order_relaxed); }

    /** Passo del pattern che suona al passo assoluto k. */
    int stepIndexAt (long long k) const noexcept
    {
        const long long L = length;

        switch (direction)
        {
            case Direction::Backward: return (int) (L - 1 - ((k % L) + L) % L);

            case Direction::PingPong:
            {
                if (L < 2) return 0;
                const long long period = 2 * L - 2;
                const long long m = ((k % period) + period) % period;
                return (int) (m < L ? m : period - m);
            }

            case Direction::Random:
                return (int) (stepHash01 (k, 0xD1Bu + (uint64_t) patternIndex) * (float) L) % (int) L;

            case Direction::Forward:
            default:
                return (int) (((k % L) + L) % L);
        }
    }

    /** Il passo k suona davvero? Tiene conto di gate e probabilità. La
        probabilità è decisa da un hash di k: stessa scelta a ogni play. */
    bool stepFires (long long k) const noexcept
    {
        const auto& s = getStep (patternIndex, stepIndexAt (k));

        if (! s.gate)
            return false;

        return s.probability >= 0.999f
            || stepHash01 (k, 0x5EEDu + (uint64_t) patternIndex) < s.probability;
    }

    const SeqStep& stepAt (long long k) const noexcept { return getStep (patternIndex, stepIndexAt (k)); }

    //==========================================================================
    SeqStep& getStep (int pattern, int step) noexcept
    {
        return patterns[(size_t) juce::jlimit (0, kNumPatterns - 1, pattern)]
                       [(size_t) juce::jlimit (0, kNumSteps - 1, step)];
    }

    const SeqStep& getStep (int pattern, int step) const noexcept
    {
        return patterns[(size_t) juce::jlimit (0, kNumPatterns - 1, pattern)]
                       [(size_t) juce::jlimit (0, kNumSteps - 1, step)];
    }

    //==========================================================================
    juce::ValueTree toValueTree() const
    {
        juce::ValueTree tree ("SEQUENCER");

        for (int p = 0; p < kNumPatterns; ++p)
        {
            juce::ValueTree pat ("PATTERN");
            pat.setProperty ("index", p, nullptr);

            for (int s = 0; s < kNumSteps; ++s)
            {
                const auto& step = patterns[(size_t) p][(size_t) s];

                juce::ValueTree node ("STEP");
                node.setProperty ("i",    s, nullptr);
                node.setProperty ("note", step.note, nullptr);
                node.setProperty ("gate", step.gate, nullptr);
                node.setProperty ("acc",  step.accent, nullptr);
                node.setProperty ("sld",  step.slide, nullptr);
                node.setProperty ("tie",  step.tie, nullptr);
                node.setProperty ("vel",  step.velocity, nullptr);
                node.setProperty ("rat",  step.ratchet, nullptr);
                node.setProperty ("prob", step.probability, nullptr);

                pat.appendChild (node, nullptr);
            }

            tree.appendChild (pat, nullptr);
        }

        return tree;
    }

    void fromValueTree (const juce::ValueTree& tree)
    {
        if (! tree.hasType ("SEQUENCER"))
            return;

        for (int p = 0; p < tree.getNumChildren(); ++p)
        {
            const auto pat = tree.getChild (p);
            const int patIndex = (int) pat.getProperty ("index", -1);

            if (patIndex < 0 || patIndex >= kNumPatterns)
                continue;

            for (int s = 0; s < pat.getNumChildren(); ++s)
            {
                const auto node = pat.getChild (s);
                const int stepIndex = (int) node.getProperty ("i", -1);

                if (stepIndex < 0 || stepIndex >= kNumSteps)
                    continue;

                auto& step = patterns[(size_t) patIndex][(size_t) stepIndex];
                step.note        = juce::jlimit (-24, 24, (int) node.getProperty ("note", 0));
                step.gate        = (bool)  node.getProperty ("gate", true);
                step.accent      = (bool)  node.getProperty ("acc", false);
                step.slide       = (bool)  node.getProperty ("sld", false);
                step.tie         = (bool)  node.getProperty ("tie", false);
                step.velocity    = std::clamp ((float) node.getProperty ("vel", 0.8f), 0.0f, 1.0f);
                // Proprietà aggiunte in v1.1: i file vecchi non le hanno e
                // prendono il default, cioè il comportamento di prima.
                step.ratchet     = juce::jlimit (1, 4, (int) node.getProperty ("rat", 1));
                step.probability = std::clamp ((float) node.getProperty ("prob", 1.0f), 0.0f, 1.0f);
            }
        }
    }

private:
    //==========================================================================
    void loadFactoryPatterns()
    {
        // I pattern di fabbrica sono linee acid classiche in La minore.
        // Servono a due cose: dare qualcosa di suonabile al primo avvio, e
        // documentare con l'esempio come accento e slide vanno usati insieme
        // (accento sul passo forte, slide che porta al passo successivo).

        auto set = [this] (int p, int s, int note, bool gate, bool acc, bool sld, int rat = 1, float prob = 1.0f)
        {
            auto& step = patterns[(size_t) p][(size_t) s];
            step.note = note;
            step.gate = gate;
            step.accent = acc;
            step.slide = sld;
            step.tie = false;
            step.velocity = acc ? 1.0f : 0.75f;
            step.ratchet = rat;
            step.probability = prob;
        };

        // --- Pattern 1: "Acid Classic" -----------------------------------------
        const int p1[16][4] =
        {
            //note gate acc sld
            {  0, 1, 1, 0 }, { 12, 1, 0, 0 }, {  0, 1, 0, 1 }, {  3, 1, 0, 0 },
            {  0, 1, 1, 0 }, {  0, 0, 0, 0 }, { 10, 1, 0, 1 }, { 12, 1, 0, 0 },
            {  0, 1, 1, 0 }, {  7, 1, 0, 0 }, {  0, 1, 0, 0 }, {  3, 1, 0, 1 },
            {  5, 1, 0, 0 }, {  0, 0, 0, 0 }, { 12, 1, 1, 0 }, { 10, 1, 0, 0 }
        };

        // --- Pattern 2: "Rolling" -----------------------------------------------
        const int p2[16][4] =
        {
            {  0, 1, 1, 0 }, {  0, 1, 0, 0 }, {  0, 1, 0, 0 }, {  0, 1, 0, 0 },
            { 12, 1, 1, 1 }, { 10, 1, 0, 0 }, {  0, 1, 0, 0 }, {  0, 1, 0, 0 },
            {  3, 1, 1, 0 }, {  0, 1, 0, 0 }, {  0, 1, 0, 0 }, {  7, 1, 0, 1 },
            {  5, 1, 0, 0 }, {  0, 1, 1, 0 }, {  0, 1, 0, 0 }, { 12, 1, 0, 0 }
        };

        // --- Pattern 3: "Sparse" ------------------------------------------------
        const int p3[16][4] =
        {
            {  0, 1, 1, 0 }, {  0, 0, 0, 0 }, {  0, 0, 0, 0 }, {  7, 1, 0, 1 },
            { 10, 1, 0, 0 }, {  0, 0, 0, 0 }, {  0, 1, 1, 0 }, {  0, 0, 0, 0 },
            { 12, 1, 0, 0 }, {  0, 0, 0, 0 }, { 15, 1, 1, 1 }, { 12, 1, 0, 0 },
            {  0, 1, 0, 0 }, {  0, 0, 0, 0 }, {  3, 1, 0, 0 }, {  0, 0, 0, 0 }
        };

        // --- Pattern 4: "Octave Run" ---------------------------------------------
        const int p4[16][4] =
        {
            {  0, 1, 1, 0 }, { 12, 1, 0, 0 }, {  0, 1, 0, 0 }, { 12, 1, 0, 0 },
            {  0, 1, 1, 0 }, { 12, 1, 0, 0 }, {  0, 1, 0, 0 }, { 12, 1, 0, 0 },
            {  3, 1, 1, 0 }, { 15, 1, 0, 0 }, {  3, 1, 0, 0 }, { 15, 1, 0, 0 },
            {  5, 1, 1, 0 }, { 17, 1, 0, 0 }, {  5, 1, 0, 1 }, {  0, 1, 0, 0 }
        };

        const int (*sources[4])[4] = { p1, p2, p3, p4 };

        for (int p = 0; p < kNumPatterns; ++p)
        {
            const auto* src = sources[p % 4];

            for (int s = 0; s < kNumSteps; ++s)
                set (p, s, src[s][0], src[s][1] != 0, src[s][2] != 0, src[s][3] != 0);
        }

        // Pattern 5-8: varianti hardgroove/tekno degli stessi riff, con ratchet
        // e probabilità — mostrano le funzioni nuove senza toccare i primi 4.
        for (int s : { 3, 11 })  patterns[4][(size_t) s].ratchet = 2;
        for (int s : { 6, 14 })  patterns[4][(size_t) s].probability = 0.5f;
        for (int s : { 1, 5, 9, 13 }) patterns[5][(size_t) s].ratchet = 3;
        for (int s : { 2, 7, 10 }) patterns[6][(size_t) s].probability = 0.6f;
        patterns[7][15].ratchet = 4;
        patterns[7][7].ratchet = 2;
    }

    //==========================================================================
    std::array<std::array<SeqStep, kNumSteps>, kNumPatterns> patterns;

    int patternIndex = 0;
    int length = 16;
    Direction direction = Direction::Forward;

    std::atomic<int> currentStep { -1 };
};

//==============================================================================
/**
    Motore di riproduzione: orologio, sequencer, arpeggiatore e loro
    combinazioni.

    Modi di combinazione (quando sequencer e arpeggiatore sono entrambi attivi):
      Layer                  — suonano indipendenti, ciascuno col suo ritmo.
      Seq Rhythm > Arp Notes — il ritmo, gli accenti e gli slide sono del
                               sequencer; le altezze le sceglie l'arpeggiatore
                               dagli accordi tenuti (più l'offset del passo).
      Seq Transposes Arp     — l'arpeggiatore suona al suo ritmo; il passo
                               corrente del sequencer lo traspone.
*/
class PlayEngine
{
public:
    struct Settings
    {
        bool  seqOn = false, arpOn = false;
        int   combine = 0;

        int   seqDivision = 13;
        float seqSwing = 0.0f, seqGate = 0.6f;
        int   seqOctave = 0;
        bool  seqKeyFollow = true;

        int   arpMode = 0, arpOctaves = 1, arpDivision = 13;
        float arpGate = 0.5f, arpSwing = 0.0f;
        bool  arpLatch = false, arpSlide = false;
        int   arpAccentEvery = 0;     // 0 = off

        int   accentThreshold = 100;
    };

    enum class ArpMode { Up = 0, Down, UpDown, DownUp, AsPlayed, Random, Chord };
    enum Combine { Layer = 0, SeqRhythmArpNotes, SeqTransposesArp };

    //==========================================================================
    void prepare (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        reset();
    }

    void reset() noexcept
    {
        queueSize = 0;
        sampleClock = 0;
        freePpq = 0.0;
        freeRunning = false;
        freeRunArmed = false;
        wasPlaying = false;
        expectedPpq = -1.0e9;
        lastSeqK = lastArpK = std::numeric_limits<long long>::min();
        seqSounding.clear();
        arpSounding.clear();
        seqHolding = arpHolding = false;
        arpNoteCounter = 0;
        arpOriginK = 0;
        arpNeedsOrigin = true;
    }

    bool isActive (const Settings& s) const noexcept { return s.seqOn || s.arpOn; }

    //==========================================================================
    /** Tasti premuti, inoltrati dal processore quando il motore è attivo. */
    void keyDown (int note, int velocity, bool latch) noexcept
    {
        if (latch && physicalDown == 0)
            keys.clear();   // nuovo accordo dopo aver rilasciato tutto

        ++physicalDown;
        freeRunArmed = true;    // un tasto vero: a trasporto fermo il motore puo' partire
        keys.remove (note);
        keys.add (note, velocity);
        lastKey = note;
    }

    void keyUp (int note, bool latch) noexcept
    {
        physicalDown = juce::jmax (0, physicalDown - 1);

        if (! latch)
            keys.remove (note);
    }

    /** Disattivare il latch libera le note che non sono più premute. */
    void setLatch (bool latch) noexcept
    {
        if (latchState && ! latch && physicalDown == 0)
            keys.clear();

        latchState = latch;
    }

    void clearKeys() noexcept { keys.clear(); physicalDown = 0; freeRunArmed = false; }

    int getNumKeys() const noexcept { return keys.size; }
    int getKey (int i) const noexcept { return keys.notes[(size_t) juce::jlimit (0, kMaxKeys - 1, i)]; }
    int getLastPlayedArpNote() const noexcept { return lastArpNote.load (std::memory_order_relaxed); }

    //==========================================================================
    /**
        Genera gli eventi del blocco.

        @param hostHasTransport  se falso (standalone senza trasporto) il
                                 motore usa l'orologio interno
    */
    void processBlock (Sequencer& seq, const Settings& s,
                       double bpm, double hostPpq, bool hostPlaying,
                       int numSamples, juce::Array<SeqEvent>& out) noexcept
    {
        settings = s;
        sequencer = &seq;

        const long long blockStart = sampleClock;
        const long long blockEnd   = sampleClock + numSamples;
        sampleClock = blockEnd;

        setLatch (s.arpLatch);

        const double samplesPerBeat = (60.0 / juce::jlimit (20.0, 999.0, bpm)) * sampleRate;

        // --- Sorgente dell'orologio ------------------------------------------
        double ppqStart = 0.0;
        bool run = false;

        if (hostPlaying)
        {
            if (! wasPlaying || std::abs (hostPpq - expectedPpq) > 0.01)
            {
                // Partenza o salto della testina: le note in corso vanno
                // chiuse, altrimenti restano appese su un passo che non esiste.
                flushAll (out, 0);
                lastSeqK = lastArpK = std::numeric_limits<long long>::min();
                arpNeedsOrigin = true;
            }

            ppqStart = hostPpq;
            run = true;
            freeRunning = false;
        }
        else
        {
            if (wasPlaying)
            {
                flushAll (out, 0);
                // La DAW si e' fermata: il motore si ferma con lei. Senza,
                // i tasti in LATCH (o un accordo rimasto tenuto) facevano
                // ripartire subito l'orologio interno e la sequenza girava
                // da sola all'infinito dopo lo stop. Riparte solo con un
                // nuovo tasto suonato a trasporto fermo.
                freeRunArmed = false;
                freeRunning = false;
            }

            const bool keysHeld = keys.size > 0;

            if (keysHeld && ! freeRunning && freeRunArmed)
            {
                freeRunning = true;
                freePpq = 0.0;
                lastSeqK = lastArpK = std::numeric_limits<long long>::min();
                arpNeedsOrigin = true;
            }
            else if (! keysHeld && freeRunning)
            {
                freeRunning = false;
                freeRunArmed = false;
                flushAll (out, 0);
            }

            ppqStart = freePpq;
            run = freeRunning;
        }

        wasPlaying = hostPlaying;
        expectedPpq = hostPpq + (double) numSamples / samplesPerBeat;

        if (! s.seqOn) releaseSource (seqSounding, seqHolding, blockStart);
        if (! s.arpOn) releaseSource (arpSounding, arpHolding, blockStart);

        if (keys.size == 0)
        {
            arpNeedsOrigin = true;

            // Senza tasti l'arpeggiatore tace: chiude subito ciò che suona.
            if (! arpSounding.isEmpty())
                releaseSource (arpSounding, arpHolding, blockStart);
        }

        if (run)
        {
            const bool seqTriggers = s.seqOn && ! (s.arpOn && s.combine == SeqTransposesArp);
            const bool arpTriggers = s.arpOn && ! (s.seqOn && s.combine == SeqRhythmArpNotes);

            if (seqTriggers)
                forEachOnset (ppqStart, samplesPerBeat, numSamples, divisionBeats (s.seqDivision), s.seqSwing, lastSeqK,
                              [&] (long long k, double offset, double stepSamples, double nextOffset)
                              { triggerSeqStep (k, blockStart, offset, stepSamples, nextOffset); });

            if (arpTriggers && keys.size > 0)
                forEachOnset (ppqStart, samplesPerBeat, numSamples, divisionBeats (s.arpDivision), s.arpSwing, lastArpK,
                              [&] (long long k, double offset, double stepSamples, double nextOffset)
                              { triggerArpStep (k, blockStart, offset, stepSamples, nextOffset, ppqStart, samplesPerBeat); });

            if (! hostPlaying)
                freePpq += (double) numSamples / samplesPerBeat;
        }

        drainQueue (blockStart, blockEnd, out);
    }

private:
    //==========================================================================
    static double divisionBeats (int index) noexcept
    {
        return (double) LFO::getSyncDivisionInBeats (index);
    }

    /** Chiama fn per ogni inizio di passo che cade nel blocco. Parte dal passo
        precedente a quello corrente: con lo swing l'inizio di un passo
        dispari è ritardato e può cadere nel blocco anche se il suo confine di
        griglia sta prima — saltarlo significherebbe perdere la nota. */
    template <typename Fn>
    void forEachOnset (double ppqStart, double samplesPerBeat, int numSamples,
                       double beatsPerStep, float swing, long long& lastK, Fn&& fn) noexcept
    {
        if (beatsPerStep <= 0.0)
            return;

        const double stepSamples = samplesPerBeat * beatsPerStep;

        if (stepSamples < 2.0)
            return;

        const double sw = (double) std::clamp (swing, 0.0f, 0.75f) * 0.5;

        auto onsetOffset = [&] (long long k)
        {
            const double swingOff = ((k & 1) != 0) ? sw : 0.0;
            return (((double) k + swingOff) * beatsPerStep - ppqStart) * samplesPerBeat;
        };

        long long k = (long long) std::floor (ppqStart / beatsPerStep) - 1;

        for (int guard = 0; guard < 512; ++guard, ++k)
        {
            const double offset = onsetOffset (k);

            if (offset >= (double) numSamples - 0.5)
                break;

            // Tolleranza di mezzo campione contro l'errore di arrotondamento
            // sul confine fra due blocchi; lastK impedisce il doppio trigger.
            if (offset > -0.5 && k != lastK)
            {
                lastK = k;
                fn (k, juce::jmax (0.0, offset), stepSamples, onsetOffset (k + 1));
            }
        }
    }

    //==========================================================================
    int baseNote() const noexcept
    {
        if (settings.seqKeyFollow && lastKey >= 0)
            return lastKey;

        return 45;   // A2: la tonalità dei pattern di fabbrica
    }

    void triggerSeqStep (long long k, long long blockStart, double offset,
                         double stepSamples, double nextOffset) noexcept
    {
        auto& seq = *sequencer;
        const auto& step = seq.stepAt (k);
        const long long onset = blockStart + std::llround (offset);

        seq.setCurrentStep (seq.stepIndexAt (k));

        // Tie: la nota precedente continua; il gate si sposta in avanti.
        if (step.tie && step.gate && ! seqSounding.isEmpty())
        {
            scheduleGateEnd (seqSounding, seqHolding, k, true, onset, stepSamples, nextOffset, blockStart,
                             settings.seqGate, step.ratchet);
            return;
        }

        if (! seq.stepFires (k))
        {
            // Passo muto: se si stava tenendo il gate per uno slide che non
            // arriva (probabilità), la nota va chiusa qui.
            releaseSource (seqSounding, seqHolding, onset);
            return;
        }

        int note = baseNote() + step.note + settings.seqOctave * 12;

        if (settings.arpOn && settings.combine == SeqRhythmArpNotes && keys.size > 0)
            note = nextArpNote (k, nullptr) + step.note + settings.seqOctave * 12;

        note = juce::jlimit (0, 127, note);

        const bool slide = step.slide && ! seqSounding.isEmpty();
        const float mod = std::clamp ((float) step.note / 24.0f, -1.0f, 1.0f);

        playNote (seqSounding, seqHolding, note, step.velocity, step.accent, slide, mod,
                  onset, stepSamples, nextOffset, blockStart, settings.seqGate,
                  juce::jlimit (1, 4, step.ratchet), k, true);
    }

    void triggerArpStep (long long k, long long blockStart, double offset,
                         double stepSamples, double nextOffset,
                         double ppqStart, double samplesPerBeat) noexcept
    {
        if (arpNeedsOrigin)
        {
            arpOriginK = k;
            arpNoteCounter = 0;
            arpNeedsOrigin = false;
        }

        const long long onset = blockStart + std::llround (offset);
        const long long index = k - arpOriginK;

        int velocity = 100;
        int transpose = 0;

        if (settings.seqOn && settings.combine == SeqTransposesArp && sequencer != nullptr)
        {
            // Passo del sequencer alla posizione di questa nota: funzione pura
            // della posizione, quindi nessuno stato da tenere allineato.
            const double ppqHere = ppqStart + offset / samplesPerBeat;
            const long long seqK = (long long) std::floor (ppqHere / divisionBeats (settings.seqDivision));
            const auto& st = sequencer->stepAt (seqK);
            transpose = st.note + settings.seqOctave * 12;
            sequencer->setCurrentStep (sequencer->stepIndexAt (seqK));
        }

        const bool accentByCount = settings.arpAccentEvery > 0
                                && (index % (long long) (settings.arpAccentEvery + 1)) == 0;

        if ((ArpMode) settings.arpMode == ArpMode::Chord)
        {
            // Accordo: tutte le note insieme, l'ottava ciclica a ogni passo.
            const int octave = (int) (index % juce::jmax (1, settings.arpOctaves));
            releaseSource (arpSounding, arpHolding, onset);

            for (int i = 0; i < keys.size; ++i)
            {
                const int n = juce::jlimit (0, 127, keys.notes[(size_t) i] + octave * 12 + transpose);
                const bool acc = accentByCount || keys.velocities[(size_t) i] >= settings.accentThreshold;
                playNote (arpSounding, arpHolding, n, (float) keys.velocities[(size_t) i] / 127.0f, acc, false, 0.0f,
                          onset, stepSamples, nextOffset, blockStart, settings.arpGate, 1, k, false, false);
            }

            lastArpNote.store (keys.notes[0], std::memory_order_relaxed);
            return;
        }

        const int note = juce::jlimit (0, 127, nextArpNote (k, &velocity) + transpose);
        const bool accent = accentByCount || velocity >= settings.accentThreshold;
        const bool slide = settings.arpSlide && ! arpSounding.isEmpty();

        lastArpNote.store (note, std::memory_order_relaxed);

        playNote (arpSounding, arpHolding, note, (float) velocity / 127.0f, accent, slide, 0.0f,
                  onset, stepSamples, nextOffset, blockStart, settings.arpGate, 1, k, false);
    }

    /** Prossima nota dell'arpeggio. */
    int nextArpNote (long long k, int* velocityOut) noexcept
    {
        std::array<int, kMaxArpNotes> list {};
        std::array<int, kMaxArpNotes> vel {};
        const int n = buildArpList (list, vel);

        if (n == 0)
            return baseNote();

        const long long counter = arpNoteCounter++;
        int idx = 0;

        if ((ArpMode) settings.arpMode == ArpMode::Random)
            idx = (int) (stepHash01 (k, 0xA4Bu) * (float) n) % n;
        else
            idx = (int) (counter % n);

        if (velocityOut != nullptr)
            *velocityOut = vel[(size_t) idx];

        return list[(size_t) idx];
    }

    static constexpr int kMaxKeys = 32;
    static constexpr int kMaxArpNotes = kMaxKeys * 4 * 2;

    int buildArpList (std::array<int, kMaxArpNotes>& list, std::array<int, kMaxArpNotes>& vel) const noexcept
    {
        if (keys.size == 0)
            return 0;

        // Base: note premute, ordinate per altezza o per ordine di pressione.
        std::array<int, kMaxKeys> base {};
        std::array<int, kMaxKeys> baseVel {};
        const int nk = keys.size;

        for (int i = 0; i < nk; ++i)
        {
            base[(size_t) i] = keys.notes[(size_t) i];
            baseVel[(size_t) i] = keys.velocities[(size_t) i];
        }

        const auto mode = (ArpMode) settings.arpMode;

        if (mode != ArpMode::AsPlayed)
        {
            // Insertion sort: al massimo 32 elementi, nessuna allocazione.
            for (int i = 1; i < nk; ++i)
                for (int j = i; j > 0 && base[(size_t) j - 1] > base[(size_t) j]; --j)
                {
                    std::swap (base[(size_t) j - 1], base[(size_t) j]);
                    std::swap (baseVel[(size_t) j - 1], baseVel[(size_t) j]);
                }
        }

        const int octaves = juce::jlimit (1, 4, settings.arpOctaves);
        int up = 0;
        std::array<int, kMaxKeys * 4> upList {};
        std::array<int, kMaxKeys * 4> upVel {};

        for (int o = 0; o < octaves; ++o)
            for (int i = 0; i < nk; ++i)
            {
                upList[(size_t) up] = base[(size_t) i] + 12 * o;
                upVel[(size_t) up] = baseVel[(size_t) i];
                ++up;
            }

        int n = 0;
        auto push = [&] (int idx) { list[(size_t) n] = upList[(size_t) idx]; vel[(size_t) n] = upVel[(size_t) idx]; ++n; };

        switch (mode)
        {
            case ArpMode::Down:
                for (int i = up - 1; i >= 0; --i) push (i);
                break;

            case ArpMode::UpDown:
                for (int i = 0; i < up; ++i) push (i);
                for (int i = up - 2; i > 0; --i) push (i);
                break;

            case ArpMode::DownUp:
                for (int i = up - 1; i >= 0; --i) push (i);
                for (int i = 1; i < up - 1; ++i) push (i);
                break;

            case ArpMode::Up:
            case ArpMode::AsPlayed:
            case ArpMode::Random:
            case ArpMode::Chord:
            default:
                for (int i = 0; i < up; ++i) push (i);
                break;
        }

        return n;
    }

    //==========================================================================
    struct Sounding
    {
        std::array<int, kMaxKeys> notes {};
        int size = 0;

        bool isEmpty() const noexcept { return size == 0; }
        void clear() noexcept { size = 0; }
        void add (int n) noexcept { if (size < kMaxKeys) notes[(size_t) size++] = n; }
    };

    /** Suona una nota (con eventuali ratchet) e programma la fine del gate. */
    void playNote (Sounding& sounding, bool& holding, int note, float velocity, bool accent, bool slide,
                   float mod, long long onset, double stepSamples, double nextOffset, long long blockStart,
                   float gate, int ratchet, long long k, bool isSeq, bool releasePrevious = true) noexcept
    {
        const int sub = juce::jmax (1, ratchet);
        const double subLen = stepSamples / (double) sub;

        if (slide)
        {
            // Legato: nuova nota prima, vecchia chiusa dopo — il voice mono
            // scivola senza ritriggerare, quello poly non resta appeso.
            push (onset, makeEvent (SeqEvent::Type::NoteOn, note, velocity, accent, true, mod));

            for (int i = 0; i < sounding.size; ++i)
                if (sounding.notes[(size_t) i] != note)
                    push (onset, makeEvent (SeqEvent::Type::NoteOff, sounding.notes[(size_t) i]));

            sounding.clear();
        }
        else
        {
            if (releasePrevious)
                releaseSource (sounding, holding, onset);

            push (onset, makeEvent (SeqEvent::Type::NoteOn, note, velocity, accent, false, mod));
        }

        sounding.add (note);
        holding = false;

        // Ripetizioni del ratchet: note-off e note-on alla stessa posizione,
        // la successiva ritriggera gli inviluppi.
        for (int r = 1; r < sub; ++r)
        {
            const long long t = onset + (long long) (subLen * r);
            const long long gateEnd = onset + (long long) (subLen * (r - 1) + subLen * (double) gate);
            push (juce::jmin (gateEnd, t), makeEvent (SeqEvent::Type::NoteOff, note));
            push (t, makeEvent (SeqEvent::Type::NoteOn, note, velocity, accent, false, mod));
        }

        const long long lastSubOnset = onset + (long long) (subLen * (sub - 1));
        scheduleGateEnd (sounding, holding, k, false, lastSubOnset, subLen, nextOffset, blockStart, gate, 1, isSeq);
    }

    /** Programma il note-off di fine gate, oppure tiene aperto il gate se il
        passo successivo è legato (slide o tie). */
    void scheduleGateEnd (Sounding& sounding, bool& holding, long long k, bool fromTie,
                          long long onset, double len, double nextOffset, long long blockStart,
                          float gate, int /*ratchet*/, bool isSeq = true) noexcept
    {
        if (sounding.isEmpty())
            return;

        const long long nextOnset = blockStart + std::llround (nextOffset);

        bool holdThrough = false;

        if (isSeq && sequencer != nullptr)
        {
            const auto& next = sequencer->stepAt (k + 1);
            holdThrough = next.gate && (next.tie || next.slide) && sequencer->stepFires (k + 1);
        }
        else if (! isSeq)
        {
            holdThrough = settings.arpSlide && keys.size > 0;
        }

        if (holdThrough)
        {
            holding = true;
            return;
        }

        juce::ignoreUnused (fromTie);

        // Fine gate, mai oltre l'inizio del passo successivo: con lo swing un
        // passo dispari lungo sconfinerebbe nel successivo e il voice mono
        // leggerebbe la sovrapposizione come legato.
        long long end = onset + (long long) (len * (double) gate);
        end = juce::jmin (end, nextOnset - 1);
        end = juce::jmax (end, onset + 1);

        for (int i = 0; i < sounding.size; ++i)
            push (end, makeEvent (SeqEvent::Type::NoteOff, sounding.notes[(size_t) i]));

        // Le note restano "sounding" fino al passo successivo solo per la
        // logica di slide; il note-off è già in coda.
        pendingClearAt (sounding, end);
    }

    void pendingClearAt (Sounding& sounding, long long) noexcept
    {
        // Una nota il cui note-off è già programmato non deve ricevere un
        // secondo note-off da releaseSource(): la si toglie dall'elenco.
        sounding.clear();
    }

    void releaseSource (Sounding& sounding, bool& holding, long long time) noexcept
    {
        for (int i = 0; i < sounding.size; ++i)
            push (time, makeEvent (SeqEvent::Type::NoteOff, sounding.notes[(size_t) i]));

        sounding.clear();
        holding = false;
    }

    /** Chiude tutto subito: note in corso e note-on già programmati. */
    void flushAll (juce::Array<SeqEvent>& out, int position) noexcept
    {
        // Dalla coda si tengono solo i note-off (vanno comunque emessi), i
        // note-on futuri si scartano.
        int w = 0;

        for (int i = 0; i < queueSize; ++i)
        {
            if (queue[(size_t) i].ev.type == SeqEvent::Type::NoteOff)
            {
                auto e = queue[(size_t) i].ev;
                e.samplePosition = position;
                out.add (e);
            }
        }

        queueSize = w;

        for (auto* s : { &seqSounding, &arpSounding })
        {
            for (int i = 0; i < s->size; ++i)
            {
                auto e = makeEvent (SeqEvent::Type::NoteOff, s->notes[(size_t) i]);
                e.samplePosition = position;
                out.add (e);
            }

            s->clear();
        }

        seqHolding = arpHolding = false;
    }

    //==========================================================================
    struct Scheduled
    {
        long long time = 0;
        SeqEvent ev;
    };

    static SeqEvent makeEvent (SeqEvent::Type type, int note, float vel = 0.0f,
                               bool acc = false, bool slide = false, float mod = 0.0f) noexcept
    {
        SeqEvent e;
        e.type = type;
        e.midiNote = note;
        e.velocity = vel;
        e.accent = acc;
        e.slide = slide;
        e.modValue = mod;
        return e;
    }

    void push (long long time, const SeqEvent& e) noexcept
    {
        if (queueSize < kQueueSize)
            queue[(size_t) queueSize++] = { time, e };
        else
            jassertfalse;   // coda piena: impossibile con i limiti di ratchet e note
    }

    void drainQueue (long long blockStart, long long blockEnd, juce::Array<SeqEvent>& out) noexcept
    {
        const int firstOut = out.size();
        int w = 0;

        for (int i = 0; i < queueSize; ++i)
        {
            const auto& item = queue[(size_t) i];

            if (item.time < blockEnd)
            {
                auto e = item.ev;
                e.samplePosition = (int) juce::jmax (0LL, item.time - blockStart);
                out.add (e);
            }
            else
            {
                queue[(size_t) w++] = item;
            }
        }

        queueSize = w;

        // Ordinamento stabile per posizione, note-off prima dei note-on alla
        // stessa posizione. Insertion sort: pochi elementi, già quasi ordinati,
        // nessuna allocazione (std::stable_sort può allocare).
        auto before = [] (const SeqEvent& a, const SeqEvent& b)
        {
            if (a.samplePosition != b.samplePosition)
                return a.samplePosition < b.samplePosition;

            return a.type == SeqEvent::Type::NoteOff && b.type == SeqEvent::Type::NoteOn;
        };

        for (int i = firstOut + 1; i < out.size(); ++i)
            for (int j = i; j > firstOut && before (out.getReference (j), out.getReference (j - 1)); --j)
                out.swap (j, j - 1);
    }

    //==========================================================================
    struct KeySet
    {
        std::array<int, kMaxKeys> notes {};
        std::array<int, kMaxKeys> velocities {};
        int size = 0;

        void clear() noexcept { size = 0; }

        void add (int n, int v) noexcept
        {
            if (size < kMaxKeys)
            {
                notes[(size_t) size] = n;
                velocities[(size_t) size] = v;
                ++size;
            }
        }

        void remove (int n) noexcept
        {
            int w = 0;

            for (int i = 0; i < size; ++i)
                if (notes[(size_t) i] != n)
                {
                    notes[(size_t) w] = notes[(size_t) i];
                    velocities[(size_t) w] = velocities[(size_t) i];
                    ++w;
                }

            size = w;
        }
    };

    static constexpr int kQueueSize = 512;

    double sampleRate = 44100.0;
    Settings settings;
    Sequencer* sequencer = nullptr;

    std::array<Scheduled, kQueueSize> queue {};
    int queueSize = 0;
    long long sampleClock = 0;

    double freePpq = 0.0, expectedPpq = -1.0e9;
    bool freeRunning = false, wasPlaying = false;
    bool freeRunArmed = false;   // tasto suonato a trasporto fermo dopo l'ultimo stop

    long long lastSeqK = std::numeric_limits<long long>::min();
    long long lastArpK = std::numeric_limits<long long>::min();

    Sounding seqSounding, arpSounding;
    bool seqHolding = false, arpHolding = false;

    KeySet keys;
    int physicalDown = 0;
    int lastKey = -1;
    bool latchState = false;

    long long arpNoteCounter = 0, arpOriginK = 0;
    bool arpNeedsOrigin = true;
    std::atomic<int> lastArpNote { -1 };
};

} // namespace bassx
