/*
  ==============================================================================

    BASSX — WavetableBank.h

    Banco di wavetable band-limited con mipmapping.

    Perché mipmapping e non una singola tabella:
      Una wavetable è una somma di armoniche. Se la tabella contiene la 512ª
      armonica e la si legge a 440 Hz va bene (512 * 440 = 225 kHz? no: sopra
      Nyquist). Ogni armonica sopra sr/2 si ripiega dentro la banda audio come
      componente inarmonica. L'unico modo corretto è avere più versioni della
      stessa forma d'onda, ciascuna limitata a un numero massimo di armoniche,
      e scegliere in tempo reale quella più ricca che non produce aliasing alla
      frequenza corrente. Questo è ciò che fanno Serum e Massive, e la ragione
      per cui i loro oscillatori restano puliti su tutta l'estensione.

    Costo: ~2x la memoria di una singola tabella (serie geometrica) e una
    scelta di indice per campione. In cambio, aliasing sotto -100 dB.

    Le tabelle vengono costruite una sola volta per processo (SharedResource) e
    condivise fra tutte le istanze del plugin: 16 tabelle x 16 frame x 10 mip
    sono ~9 MB, inaccettabili se duplicati per traccia.

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <array>
#include <functional>
#include "Common.h"

namespace bassx
{

//==============================================================================

/** Lunghezza di ogni frame. 2048 è il compromesso standard: risoluzione
    sufficiente per 1024 armoniche (limite di Nyquist della tabella stessa) e
    footprint di cache ancora ragionevole. */
inline constexpr int kTableSize   = 2048;

/** Campioni di guardia in coda, copie dei primi campioni, per poter leggere
    i+1 e i+2 dell'interpolazione di Hermite senza modulo. */
inline constexpr int kTableGuard  = 3;

/** 10 livelli: dal pieno (1024 armoniche) a 2 armoniche. Sotto le 2 armoniche
    la forma è indistinguibile da una sinusoide, quindi non serve scendere. */
inline constexpr int kNumMipLevels = 10;

/** Numero di frame per le tabelle morphing. Serum ne usa 256; qui 16 è
    sufficiente perché l'interpolazione fra frame è lineare e le nostre
    transizioni spettrali sono progettate lisce. */
inline constexpr int kFramesPerMorphTable = 16;

//==============================================================================

/** Armoniche massime consentite a un dato livello di mip. */
inline constexpr int mipMaxHarmonics (int level) noexcept
{
    return (kTableSize / 2) >> level;    // 1024, 512, ..., 2
}

//==============================================================================

/** Allineamento di fase dei layer.

    Ogni frame di ogni tabella (di fabbrica o da campione) viene ruotato nel
    tempo in modo che la sua fondamentale sia +sin(2*pi*fase): parte da zero
    in salita a fase 0, come il sub. Cosi' due oscillatori alla stessa nota,
    ripartiti a fase 0, sommano le fondamentali in fase: un basso solo,
    non due layer che si cancellano a caso (comb, buchi di basse).

    sineFundamentalAngle(): angolo del bin 1 (formato FFT reale di JUCE) di
    quella sinusoide. fundamentalShift(): di quanto ruotare la fondamentale;
    l'armonica h ruota di h volte tanto (= ritardo puro, timbro invariato). */
inline float sineFundamentalAngle()
{
    static const float angle = []
    {
        juce::dsp::FFT f (11);
        std::vector<float> d ((size_t) kTableSize * 2, 0.0f);
        for (int i = 0; i < kTableSize; ++i)
            d[(size_t) i] = std::sin (juce::MathConstants<float>::twoPi * (float) i / (float) kTableSize);
        f.performRealOnlyForwardTransform (d.data(), true);
        return std::atan2 (d[3], d[2]);
    }();
    return angle;
}

inline float fundamentalShift (float re1, float im1) noexcept
{
    // senza fondamentale (o quasi) non c'e' niente da allineare
    if (re1 * re1 + im1 * im1 < 1.0e-12f)
        return 0.0f;
    return sineFundamentalAngle() - std::atan2 (im1, re1);
}

//==============================================================================

/** Un singolo frame: la stessa forma d'onda in kNumMipLevels versioni
    band-limited. */
struct WavetableFrame
{
    std::array<std::vector<float>, kNumMipLevels> mip;
};

/** Una wavetable completa: uno o più frame che si possono attraversare con il
    parametro "position". */
struct Wavetable
{
    juce::String name;
    std::vector<WavetableFrame> frames;

    int getNumFrames() const noexcept { return (int) frames.size(); }
};

//==============================================================================

/**
    Banco condiviso. Costruito una volta al primo utilizzo.

    La costruzione usa una IFFT invece della somma additiva diretta: la somma
    additiva costerebbe O(N * H) per mip, cioè ~4 milioni di operazioni per
    frame, oltre 500 milioni per il banco intero — uno stallo di secondi
    all'apertura del plugin. Con la IFFT il costo scende a O(N log N) per mip,
    ~30 milioni in totale, sotto i 100 ms.
*/
class WavetableBank
{
public:
    WavetableBank();

    int getNumTables() const noexcept { return (int) tables.size(); }

    const Wavetable& getTable (int index) const noexcept
    {
        return tables[(size_t) juce::jlimit (0, (int) tables.size() - 1, index)];
    }

    juce::StringArray getTableNames() const;

    /** Livello di mip da usare per un dato incremento di fase (freq / sr).
        Restituisce l'indice del livello più ricco che non alias. */
    static int selectMipLevel (float phaseIncrement) noexcept
    {
        const float inc = std::abs (phaseIncrement);

        if (inc < 1.0e-7f)
            return 0;

        // Armoniche riproducibili senza aliasing a questo incremento.
        const float allowed = 0.5f / inc;

        if (allowed >= (float) mipMaxHarmonics (0))
            return 0;

        // level = ceil(log2(1024 / allowed))
        const int level = (int) std::ceil (std::log2 ((float) mipMaxHarmonics (0) / std::max (allowed, 1.0f)));
        return juce::jlimit (0, kNumMipLevels - 1, level);
    }

private:
    //==========================================================================
    /** Spettro di lavoro: ampiezza e fase per armonica. */
    struct Spectrum
    {
        std::array<float, kTableSize / 2> magnitude {};
        std::array<float, kTableSize / 2> phase {};

        void clear() noexcept
        {
            magnitude.fill (0.0f);
            phase.fill (0.0f);
        }

        void set (int harmonic, float mag, float ph = 0.0f) noexcept
        {
            if (harmonic >= 1 && harmonic < kTableSize / 2)
            {
                magnitude[(size_t) harmonic] = mag;
                phase[(size_t) harmonic]     = ph;
            }
        }
    };

    //==========================================================================
    void buildTable (const juce::String& name, int numFrames,
                     const std::function<void (Spectrum&, float /*framePos 0..1*/)>& fillSpectrum);

    void renderFrame (const Spectrum& spec, WavetableFrame& out);

    /** Ricava uno spettro analizzando una funzione periodica nel dominio del
        tempo. Usato per le forme la cui espressione analitica dello spettro
        sarebbe una serie di Bessel (FM) o non esiste in forma chiusa
        (wavefolding). Valido solo per funzioni il cui contenuto armonico
        significativo sta ben sotto le 1024 armoniche, condizione verificata
        per tutti gli usi qui. */
    void spectrumFromTimeDomain (const std::function<float (float /*phase 0..1*/)>& fn, Spectrum& out);

    //==========================================================================
    // Generatori di spettro. Ognuno riempie lo spettro per una posizione di
    // frame normalizzata in [0, 1]. Non statici perché alcuni si appoggiano
    // all'analisi FFT nel dominio del tempo, che usa lo scratch dell'istanza.
    void specAnalogSaw      (Spectrum&, float);
    void specAnalogSquare   (Spectrum&, float);
    void specAnalogTriangle (Spectrum&, float);
    void specSineToSaw      (Spectrum&, float);
    void specPulseMorph     (Spectrum&, float);
    void specAcidSaw        (Spectrum&, float);
    void specAcidSquare     (Spectrum&, float);
    void specFormantBass    (Spectrum&, float);
    void specResoSweep      (Spectrum&, float);
    void specFMBass         (Spectrum&, float);
    void specHarmonicStack  (Spectrum&, float);
    void specFoldedSine     (Spectrum&, float);
    void specDigitalBass    (Spectrum&, float);
    void specGrowl          (Spectrum&, float);
    void specSubHarmonic    (Spectrum&, float);
    void specMetallic       (Spectrum&, float);

    //==========================================================================
    std::vector<Wavetable> tables;
    juce::dsp::FFT fft { 11 };            // 2^11 = 2048
    std::vector<float> fftScratch;        // 2 * kTableSize, riusato fra i frame

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WavetableBank)
};

//==============================================================================
/** Accesso al banco condiviso di processo. */
using SharedWavetableBank = juce::SharedResourcePointer<WavetableBank>;

} // namespace bassx
