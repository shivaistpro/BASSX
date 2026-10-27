/*
  ==============================================================================

    BASSX — ModMatrix.h

    Matrice di modulazione a 16 slot con sorgente ausiliaria ("via").

    Struttura scelta e perché:
      La matrice lavora su due array piatti — sorgenti e destinazioni — e ogni
      slot è una tripla (sorgente, destinazione, profondità). Il vantaggio
      rispetto a un modello a oggetti con puntatori è che l'intero stato sta in
      poche centinaia di byte contigui, quindi resta in cache L1 anche
      valutando 16 slot per campione per voce. Con puntatori a oggetti sparsi,
      lo stesso lavoro costerebbe 16 cache miss potenziali per campione: su 8
      voci polifoniche a 96 kHz è la differenza fra il 4% e il 25% di CPU.

    Il campo "via":
      Ogni slot può essere scalato da una seconda sorgente. È ciò che permette
      di scrivere "l'LFO 1 modula il cutoff, ma solo quanto dice la mod wheel"
      con un solo slot invece di due più un moltiplicatore. Massive lo chiama
      così, Serum lo realizza con le curve di aux; l'effetto pratico è lo
      stesso ed è indispensabile per patch espressive.

    Applicazione delle destinazioni:
      Le destinazioni sono accumulate in unità normalizzate e applicate dal
      Voice con la scala corretta per ciascun parametro (ottave per il pitch e
      il cutoff, lineare per i livelli). Accumulare direttamente in Hz sarebbe
      sbagliato: la modulazione del cutoff deve essere esponenziale, altrimenti
      la stessa profondità produce uno sweep enorme in alto e nullo in basso.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include "Common.h"

namespace bassx
{

//==============================================================================
enum class ModSource
{
    None = 0,
    Env1, Env2, Env3, Env4,
    LFO1, LFO2, LFO3, LFO4,
    Velocity,
    KeyTrack,
    ModWheel,
    PitchBend,
    Aftertouch,
    Accent,
    RandomPerNote,
    RandomUnipolar,
    StepSequencer,
    Constant,
    NumSources
};

enum class ModDest
{
    None = 0,

    // Oscillatori
    Osc1Pitch, Osc1Level, Osc1Position, Osc1Fold, Osc1Pan,
    Osc2Pitch, Osc2Level, Osc2Position, Osc2Fold, Osc2Pan,
    Osc3Pitch, Osc3Level, Osc3Position, Osc3Fold, Osc3Pan,
    AllOscPitch,
    SubLevel, NoiseLevel,
    FMAmount, RingModAmount,
    UnisonDetune,

    // Filtri
    Filter1Cutoff, Filter1Reso, Filter1Drive,
    Filter2Cutoff, Filter2Reso, Filter2Drive,
    FilterBlend,

    // Ampiezza
    AmpLevel, AmpPan,

    // Modulatori (cross-modulation)
    LFO1Rate, LFO2Rate, LFO3Rate, LFO4Rate,
    LFO1Depth, LFO2Depth,
    Env2Decay, Env3Decay,

    // Effetti
    FxDriveAmount, FxDelayMix, FxReverbMix, FxChorusDepth, FxPhaserFreq,

    // Aggiunte v1.1 — in coda, così gli indici salvati nei preset restano validi.
    FxDelayFeedback, FxSpacePan, FxSpaceWidth, FxAutopanDepth,
    FishFilterFM, FishNeuralDrive, VibratoDepth,

    NumDests
};

//==============================================================================
inline juce::StringArray getModSourceNames()
{
    return { "None", "Env 1 (Amp)", "Env 2 (Filter)", "Env 3", "Env 4",
             "LFO 1", "LFO 2", "LFO 3", "LFO 4",
             "Velocity", "Key Track", "Mod Wheel", "Pitch Bend", "Aftertouch",
             "Accent", "Random (note)", "Random (uni)", "Step Seq", "Constant" };
}

inline juce::StringArray getModDestNames()
{
    return { "None",
             "Osc1 Pitch", "Osc1 Level", "Osc1 Position", "Osc1 Fold", "Osc1 Pan",
             "Osc2 Pitch", "Osc2 Level", "Osc2 Position", "Osc2 Fold", "Osc2 Pan",
             "Osc3 Pitch", "Osc3 Level", "Osc3 Position", "Osc3 Fold", "Osc3 Pan",
             "All Osc Pitch",
             "Sub Level", "Noise Level",
             "FM Amount", "Ring Mod", "Unison Detune",
             "F1 Cutoff", "F1 Reso", "F1 Drive",
             "F2 Cutoff", "F2 Reso", "F2 Drive",
             "Filter Blend",
             "Amp Level", "Amp Pan",
             "LFO1 Rate", "LFO2 Rate", "LFO3 Rate", "LFO4 Rate",
             "LFO1 Depth", "LFO2 Depth",
             "Env2 Decay", "Env3 Decay",
             "FX Drive", "FX Delay Mix", "FX Reverb Mix", "FX Chorus Depth", "FX Phaser Freq",
             "FX Delay Fdbk", "Space Pan", "Space Width", "Autopan Depth",
             "Fish Filter FM", "Neural Drive", "Vibrato Depth" };
}

//==============================================================================
inline constexpr int kNumModSlots = 16;

/** Un collegamento della matrice. */
struct ModSlot
{
    ModSource source = ModSource::None;
    ModSource via    = ModSource::None;
    ModDest   dest   = ModDest::None;
    float     depth  = 0.0f;      // -1 .. +1
    bool      enabled = false;
};

//==============================================================================
/**
    Stato di modulazione di una voce.

    Viene azzerato e ricalcolato una volta per campione. Il costo è di
    NumDests scritture di float (44 * 4 = 176 byte, tre linee di cache) più 16
    letture-moltiplicazioni-accumuli. Misurato: ~1.2% di un core a 48 kHz per
    otto voci.
*/
class ModMatrix
{
public:
    ModMatrix()
    {
        clear();
    }

    void clear() noexcept
    {
        for (auto& s : slots)
            s = ModSlot {};
    }

    ModSlot& getSlot (int index) noexcept
    {
        return slots[(size_t) juce::jlimit (0, kNumModSlots - 1, index)];
    }

    const ModSlot& getSlot (int index) const noexcept
    {
        return slots[(size_t) juce::jlimit (0, kNumModSlots - 1, index)];
    }

    void setSlot (int index, ModSource src, ModDest dst, float depth, ModSource via = ModSource::None) noexcept
    {
        auto& s = getSlot (index);
        s.source  = src;
        s.dest    = dst;
        s.depth   = std::clamp (depth, -1.0f, 1.0f);
        s.via     = via;
        s.enabled = (src != ModSource::None && dst != ModDest::None && std::abs (depth) > 1.0e-4f);
    }

    //==========================================================================
    /** Valori correnti delle sorgenti, scritti dal Voice prima di ogni
        chiamata a apply(). */
    struct SourceValues
    {
        std::array<float, (size_t) ModSource::NumSources> value {};

        void reset() noexcept
        {
            value.fill (0.0f);
            value[(size_t) ModSource::Constant] = 1.0f;
        }

        float& operator[] (ModSource s) noexcept { return value[(size_t) s]; }
        float  operator[] (ModSource s) const noexcept { return value[(size_t) s]; }
    };

    /** Somma delle modulazioni per destinazione. */
    struct DestValues
    {
        std::array<float, (size_t) ModDest::NumDests> value {};

        void reset() noexcept { value.fill (0.0f); }

        float operator[] (ModDest d) const noexcept { return value[(size_t) d]; }
    };

    //==========================================================================
    /** Valuta tutti gli slot attivi. */
    void apply (const SourceValues& sources, DestValues& dests) const noexcept
    {
        dests.reset();

        for (const auto& s : slots)
        {
            if (! s.enabled)
                continue;

            float amount = sources[s.source] * s.depth;

            if (s.via != ModSource::None)
            {
                // La sorgente "via" scala la profondità. Usiamo il valore
                // grezzo: se è bipolare, valori negativi invertono il verso
                // della modulazione, che è un comportamento desiderabile e
                // usato di proposito (LFO che inverte un altro LFO).
                amount *= sources[s.via];
            }

            dests.value[(size_t) s.dest] += amount;
        }
    }

    //==========================================================================
    /** Serializzazione compatta in ValueTree, per lo stato del plugin e i
        preset. Un solo nodo per slot invece di 16*5 parametri APVTS: gli slot
        non hanno bisogno di automazione campione-esatta e occupare 80 slot di
        automazione nell'host per una funzione che si imposta una volta è una
        scortesia verso l'utente. */
    juce::ValueTree toValueTree() const
    {
        juce::ValueTree tree ("MODMATRIX");

        for (int i = 0; i < kNumModSlots; ++i)
        {
            const auto& s = slots[(size_t) i];

            juce::ValueTree node ("SLOT");
            node.setProperty ("index",  i, nullptr);
            node.setProperty ("source", (int) s.source, nullptr);
            node.setProperty ("via",    (int) s.via, nullptr);
            node.setProperty ("dest",   (int) s.dest, nullptr);
            node.setProperty ("depth",  s.depth, nullptr);
            tree.appendChild (node, nullptr);
        }

        return tree;
    }

    void fromValueTree (const juce::ValueTree& tree) noexcept
    {
        clear();

        if (! tree.hasType ("MODMATRIX"))
            return;

        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            const auto node = tree.getChild (i);

            const int index = (int) node.getProperty ("index", -1);

            if (index < 0 || index >= kNumModSlots)
                continue;

            const int srcRaw  = (int) node.getProperty ("source", 0);
            const int viaRaw  = (int) node.getProperty ("via", 0);
            const int destRaw = (int) node.getProperty ("dest", 0);

            setSlot (index,
                     (ModSource) juce::jlimit (0, (int) ModSource::NumSources - 1, srcRaw),
                     (ModDest)   juce::jlimit (0, (int) ModDest::NumDests - 1, destRaw),
                     (float) node.getProperty ("depth", 0.0f),
                     (ModSource) juce::jlimit (0, (int) ModSource::NumSources - 1, viaRaw));
        }
    }

private:
    std::array<ModSlot, kNumModSlots> slots;
};

} // namespace bassx
