/*
  ==============================================================================

    BASSX — Common.h
    Primitive matematiche e blocchi DSP elementari condivisi da tutto il motore.

    Regole di progetto rispettate in tutto il file:
      * nessuna allocazione, nessun lock, nessuna eccezione nel percorso audio;
      * tutte le funzioni chiamate per campione sono `noexcept` e inline;
      * ogni coefficiente dipendente dal sample rate viene ricalcolato in
        prepare(), mai nel loop;
      * i denormali sono neutralizzati esplicitamente dove lo stato può
        decadere verso zero per minuti (linee di ritardo, code di riverbero).

  ==============================================================================
*/

#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <array>
#include <vector>
#include <limits>

namespace bassx
{

//==============================================================================
// Costanti
//==============================================================================

inline constexpr float  kPi        = 3.14159265358979323846f;
inline constexpr float  kTwoPi     = 6.28318530717958647692f;
inline constexpr float  kHalfPi    = 1.57079632679489661923f;
inline constexpr float  kInvPi     = 0.31830988618379067154f;
inline constexpr double kPiD       = 3.14159265358979323846;
inline constexpr float  kSqrt2     = 1.41421356237309504880f;
inline constexpr float  kInvSqrt2  = 0.70710678118654752440f;
inline constexpr float  kLn2       = 0.69314718055994530942f;

/** Soglia sotto la quale un valore viene azzerato per evitare i denormali.
    Su x86 i denormali costano 50-100 cicli per operazione: in un riverbero FDN
    con otto linee di ritardo in decadimento questo basta a far salire la CPU
    del 300% dopo che l'ultimo suono è finito. */
inline constexpr float kDenormalFloor = 1.0e-20f;

//==============================================================================
// Utility scalari
//==============================================================================

inline float flushDenormal (float x) noexcept
{
    return (std::abs (x) < kDenormalFloor) ? 0.0f : x;
}

inline float lerp (float a, float b, float t) noexcept { return a + (b - a) * t; }

/** Interpolazione di Hermite a 4 punti (Catmull-Rom).
    Usata per la lettura delle wavetable e delle linee di ritardo modulate:
    l'interpolazione lineare su una delay line modulata produce una perdita di
    alte frequenze che varia con la modulazione, udibile come "wobble" timbrico
    su chorus e flanger. */
inline float hermite (float xm1, float x0, float x1, float x2, float t) noexcept
{
    const float c  = (x1 - xm1) * 0.5f;
    const float v  = x0 - x1;
    const float w  = c + v;
    const float a  = w + v + (x2 - x0) * 0.5f;
    const float bN = w + a;
    return ((((a * t) - bN) * t + c) * t + x0);
}

/** Wrap in [0, 1). Più veloce e più stabile di fmod per fasi che avanzano
    di piccoli incrementi. */
inline float wrap01 (float x) noexcept
{
    return x - std::floor (x);
}

inline float wrapPhase (float p) noexcept
{
    while (p >= kTwoPi) p -= kTwoPi;
    while (p < 0.0f)    p += kTwoPi;
    return p;
}

/** Da semitoni a rapporto di frequenza. */
inline float semitonesToRatio (float semis) noexcept
{
    return std::exp2 (semis * (1.0f / 12.0f));
}

inline float centsToRatio (float cents) noexcept
{
    return std::exp2 (cents * (1.0f / 1200.0f));
}

inline float midiToHz (float note) noexcept
{
    return 440.0f * std::exp2 ((note - 69.0f) * (1.0f / 12.0f));
}

inline float hzToMidi (float hz) noexcept
{
    return 69.0f + 12.0f * std::log2 (std::max (hz, 1.0e-6f) * (1.0f / 440.0f));
}

inline float dbToGain (float db) noexcept
{
    return std::pow (10.0f, db * 0.05f);
}

inline float gainToDb (float g) noexcept
{
    return 20.0f * std::log10 (std::max (g, 1.0e-9f));
}

/** Mappatura esponenziale 0..1 -> min..max, corretta per range che
    attraversano molte ottave (cutoff, tempi di inviluppo). */
inline float expMap (float t, float minV, float maxV) noexcept
{
    return minV * std::pow (maxV / minV, std::clamp (t, 0.0f, 1.0f));
}

//==============================================================================
// Saturatori
//==============================================================================

/** Approssimazione di tanh con Padé 3/2.

    Perché non std::tanh: viene chiamata fino a otto volte per campione per
    voce nella catena di feedback del ladder. Padé 3/2 ha errore < 1.5e-4 per
    |x| < 3, è monotona ovunque (fondamentale: una funzione non monotona nel
    percorso di feedback ripiega il segnale su se stesso e genera oscillazioni
    parassite), e costa una divisione invece di una exp. */
inline float fastTanh (float x) noexcept
{
    x = std::clamp (x, -4.9f, 4.9f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/** Derivata di fastTanh: serve all'iterazione di Newton nei filtri ZDF. */
inline float fastTanhPrime (float x) noexcept
{
    x = std::clamp (x, -4.9f, 4.9f);
    const float x2  = x * x;
    const float den = 27.0f + 9.0f * x2;
    return (729.0f + 270.0f * x2 - 9.0f * x2 * x2) / (den * den);
}

/** Saturazione asimmetrica tipo triodo: la semionda positiva comprime prima
    della negativa, generando armoniche pari oltre alle dispari. È la
    differenza fra "distorsione digitale" e "calore valvolare". */
inline float tubeSaturate (float x, float bias = 0.18f) noexcept
{
    const float b = x + bias;
    return fastTanh (b) - fastTanh (bias);
}

/** Soft clip cubico con ginocchio a 2/3. Continuo in valore e derivata prima,
    quindi non genera l'esplosione di armoniche alte del clip duro. */
inline float softClip (float x) noexcept
{
    if (x <= -1.0f) return -2.0f / 3.0f;
    if (x >=  1.0f) return  2.0f / 3.0f;
    return x - (x * x * x) * (1.0f / 3.0f);
}

/** Clipper "diode-like" a curvatura variabile.
    k = 0 -> lineare, k -> 1 -> quasi hard clip. */
inline float variableSoftClip (float x, float k) noexcept
{
    k = std::clamp (k, 0.0f, 0.999f);
    const float kk = 2.0f * k / (1.0f - k);
    return (1.0f + kk) * x / (1.0f + kk * std::abs (x));
}

/** Wavefolder di Serge/Buchla: ripiega il segnale invece di troncarlo.
    Produce armoniche che si spostano con l'ampiezza — la sorgente principale
    del timbro "metallico e vivo" dei bassi west-coast. */
inline float waveFold (float x, float amount) noexcept
{
    // amount 0 -> passa-attraverso; amount 1 -> sei-sette ripiegature.
    const float drive = 1.0f + std::clamp (amount, 0.0f, 1.0f) * 6.0f;
    float y = x * drive;

    // Catena seno: sin(pi/2 * y) coincide con y per |y| piccolo e ripiega
    // dolcemente oltre. Tre stadi bastano: oltre, l'aliasing supera il
    // beneficio timbrico anche con 4x di oversampling.
    for (int stage = 0; stage < 3; ++stage)
        y = std::sin (y * kHalfPi);

    // La catena di seni è limitata a ±1 per costruzione, quindi non serve
    // normalizzare l'ampiezza; il crossfade con il segnale asciutto garantisce
    // che amount = 0 sia esattamente trasparente invece di applicare comunque
    // il guadagno di piccolo segnale (pi/2)^3 della catena.
    return lerp (x, y, std::clamp (amount, 0.0f, 1.0f));
}

/** Foldback lineare a specchio: più aggressivo di waveFold, usato nel modulo
    di distorsione. */
inline float foldback (float x, float threshold) noexcept
{
    threshold = std::max (threshold, 0.02f);

    if (std::abs (x) <= threshold)
        return x;

    // Riflessione ripetuta attorno alla soglia.
    const float period = 4.0f * threshold;
    float t = std::fmod (std::abs (x) + threshold, period);
    if (t < 0.0f) t += period;
    t = std::abs (t - 2.0f * threshold) - threshold;
    return (x < 0.0f) ? -t : t;
}

/** Bit crusher + sample rate reducer combinati (quest'ultimo tenuto altrove,
    qui solo la quantizzazione di ampiezza). */
inline float bitCrush (float x, float bits) noexcept
{
    const float steps = std::pow (2.0f, std::clamp (bits, 1.0f, 24.0f)) * 0.5f;
    return std::round (x * steps) / steps;
}

//==============================================================================
// Filtri elementari a un polo
//==============================================================================

/** Passa-basso TPT a un polo (Zavalishin). Topology-preserving: resta corretto
    anche quando il cutoff viene modulato a velocità audio, cosa che la forma
    diretta biquad non garantisce. */
class OnePoleTPT
{
public:
    void prepare (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        setCutoff (cutoffHz);
        reset();
    }

    void reset() noexcept { s = 0.0f; }

    /** Cambia la frequenza di lavoro conservando il cutoff impostato. */
    void setSampleRate (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        setCutoff (cutoffHz);
    }

    void setCutoff (float hz) noexcept
    {
        cutoffHz = std::clamp (hz, 0.05f, (float) sampleRate * 0.49f);
        const float wd = kTwoPi * cutoffHz;
        const float T  = 1.0f / (float) sampleRate;
        const float wa = std::tan (wd * T * 0.5f);
        G = wa / (1.0f + wa);
    }

    float processLowpass (float x) noexcept
    {
        const float v = (x - s) * G;
        const float y = v + s;
        s = flushDenormal (y + v);
        return y;
    }

    float processHighpass (float x) noexcept
    {
        return x - processLowpass (x);
    }

private:
    double sampleRate = 44100.0;
    float  cutoffHz   = 1000.0f;
    float  G = 0.5f, s = 0.0f;
};

/** Passa-basso TPT del secondo ordine con Q regolabile.

    Usato come filtro di decimazione: due sezioni in cascata con le Q di
    Butterworth (0.5412 e 1.3066) danno una risposta del quarto ordine
    massimamente piatta in banda passante. Serve a rimuovere ciò che gli stadi
    saturanti hanno generato sopra la Nyquist di base prima di ributtare via i
    campioni in eccesso: decimare senza filtrare significa ripiegare quel
    contenuto dentro la banda audio, che è esattamente il problema che
    l'oversampling doveva risolvere. */
class TPTLowpass2
{
public:
    void prepare (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        update();
        reset();
    }

    void setSampleRate (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        update();
    }

    void reset() noexcept { s1 = s2 = 0.0f; }

    void setCutoffAndQ (float hz, float q) noexcept
    {
        cutoffHz = std::clamp (hz, 10.0f, (float) sampleRate * 0.49f);
        Q = std::max (q, 0.05f);
        update();
    }

    float process (float x) noexcept
    {
        const float hp = (x - (R2 + g) * s1 - s2) * h;
        const float bp = g * hp + s1;
        s1 = flushDenormal (g * hp + bp);
        const float lp = g * bp + s2;
        s2 = flushDenormal (g * bp + lp);
        return lp;
    }

private:
    void update() noexcept
    {
        g  = std::tan (kPi * cutoffHz / (float) sampleRate);
        R2 = 1.0f / Q;
        h  = 1.0f / (1.0f + R2 * g + g * g);
    }

    double sampleRate = 44100.0;
    float cutoffHz = 18000.0f, Q = 0.7071f;
    float g = 0.1f, R2 = 1.414f, h = 1.0f;
    float s1 = 0.0f, s2 = 0.0f;
};

/** Blocco DC del primo ordine con angolo fisso indipendente dal sample rate.
    Obbligatorio dopo ogni stadio saturante asimmetrico: senza, l'offset si
    accumula, mangia headroom e fa comportare male qualunque limiter a valle. */
class DCBlocker
{
public:
    void prepare (double sr, float cornerHz = 18.0f) noexcept
    {
        R = 1.0f - (kTwoPi * cornerHz / (float) (sr > 0.0 ? sr : 44100.0));
        R = std::clamp (R, 0.5f, 0.99999f);
        reset();
    }

    void reset() noexcept { x1 = y1 = 0.0f; }

    float process (float x) noexcept
    {
        const float y = x - x1 + R * y1;
        x1 = x;
        y1 = flushDenormal (y);
        return y;
    }

private:
    float R = 0.9975f, x1 = 0.0f, y1 = 0.0f;
};

/** Smoother esponenziale a un polo per parametri di controllo.
    Distinto da juce::SmoothedValue perché non ha un tempo di arrivo finito:
    per il cutoff serve un inseguimento continuo, mai un "atterraggio" che
    produca una discontinuità di derivata (udibile come click sul filtro). */
class ParamSmoother
{
public:
    void prepare (double sr, float timeMs) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        setTime (timeMs);
    }

    void setTime (float timeMs) noexcept
    {
        const float tau = std::max (timeMs, 0.01f) * 0.001f;
        a = std::exp (-1.0f / (tau * (float) sampleRate));
    }

    void setTarget (float t) noexcept { target = t; }
    void snap (float v) noexcept { target = current = v; }
    float getCurrent() const noexcept { return current; }
    float getTarget() const noexcept  { return target; }

    float next() noexcept
    {
        current = target + (current - target) * a;
        current = flushDenormal (current - target) + target;
        return current;
    }

private:
    double sampleRate = 44100.0;
    float a = 0.99f, current = 0.0f, target = 0.0f;
};

//==============================================================================
// Generatore pseudo-casuale
//==============================================================================

/** xorshift32: distribuzione sufficiente per rumore e randomizzazione di fase,
    a costo di tre operazioni intere. std::mt19937 non è utilizzabile nel
    percorso audio (stato da 2.5 kB, cache miss garantito). */
class Xorshift
{
public:
    explicit Xorshift (uint32_t seed = 0x9E3779B9u) noexcept : state (seed | 1u) {}

    void setSeed (uint32_t seed) noexcept { state = seed | 1u; }

    uint32_t nextUInt() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    /** Uniforme in [-1, 1). */
    float nextBipolar() noexcept
    {
        return (float) ((int32_t) nextUInt()) * (1.0f / 2147483648.0f);
    }

    /** Uniforme in [0, 1). */
    float nextUnipolar() noexcept
    {
        return (float) (nextUInt() >> 8) * (1.0f / 16777216.0f);
    }

private:
    uint32_t state;
};

//==============================================================================
// Rilevatore di inviluppo
//==============================================================================

/** Rilevatore attacco/rilascio separati, usato dal compressore e dai
    meter della GUI. */
class EnvelopeFollower
{
public:
    void prepare (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        setTimes (attackMs, releaseMs);
        reset();
    }

    void reset() noexcept { envValue = 0.0f; }

    void setTimes (float attMs, float relMs) noexcept
    {
        attackMs  = std::max (attMs, 0.01f);
        releaseMs = std::max (relMs, 0.01f);
        attCoef = std::exp (-1.0f / (attackMs  * 0.001f * (float) sampleRate));
        relCoef = std::exp (-1.0f / (releaseMs * 0.001f * (float) sampleRate));
    }

    float process (float x) noexcept
    {
        const float rectified = std::abs (x);
        const float coef = (rectified > envValue) ? attCoef : relCoef;
        envValue = rectified + (envValue - rectified) * coef;
        envValue = flushDenormal (envValue);
        return envValue;
    }

    float getCurrent() const noexcept { return envValue; }

private:
    double sampleRate = 44100.0;
    float attackMs = 5.0f, releaseMs = 80.0f;
    float attCoef = 0.99f, relCoef = 0.999f;
    float envValue = 0.0f;
};

//==============================================================================
// Linea di ritardo con interpolazione frazionaria
//==============================================================================

/** Delay line circolare a lunghezza fissa allocata in prepare().
    L'interpolazione è di Hermite: su una linea modulata (chorus, flanger,
    riverbero modulato) l'interpolazione lineare agisce da filtro passa-basso
    la cui frequenza di taglio dipende dalla parte frazionaria del ritardo, e
    quindi si muove con la modulazione. Il risultato è un tremolo timbrico che
    si sente immediatamente su un basso sostenuto. */
class DelayLine
{
public:
    void prepare (int maxSamples)
    {
        size = std::max (4, maxSamples + 4);
        buffer.assign ((size_t) size, 0.0f);
        writeIndex = 0;
    }

    void reset() noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

    void write (float x) noexcept
    {
        buffer[(size_t) writeIndex] = flushDenormal (x);
        if (++writeIndex >= size) writeIndex = 0;
    }

    /** Lettura a ritardo frazionario, in campioni. */
    float read (float delaySamples) const noexcept
    {
        delaySamples = std::clamp (delaySamples, 1.0f, (float) (size - 3));

        float readPos = (float) writeIndex - delaySamples;
        while (readPos < 0.0f) readPos += (float) size;

        const int   i0 = (int) readPos;
        const float frac = readPos - (float) i0;

        const int im1 = (i0 - 1 + size) % size;
        const int i1  = (i0 + 1) % size;
        const int i2  = (i0 + 2) % size;

        return hermite (buffer[(size_t) im1], buffer[(size_t) i0],
                        buffer[(size_t) i1],  buffer[(size_t) i2], frac);
    }

    int getSize() const noexcept { return size; }

private:
    std::vector<float> buffer;
    int size = 0;
    int writeIndex = 0;
};

//==============================================================================
// Allpass del primo ordine (phaser, riverbero)
//==============================================================================

class AllpassTPT
{
public:
    void prepare (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        setFrequency (freqHz);
        reset();
    }

    void reset() noexcept { s = 0.0f; }

    void setFrequency (float hz) noexcept
    {
        freqHz = std::clamp (hz, 10.0f, (float) sampleRate * 0.49f);
        const float wa = std::tan (kPi * freqHz / (float) sampleRate);
        G = (wa - 1.0f) / (wa + 1.0f);
    }

    float process (float x) noexcept
    {
        const float y = G * x + s;
        s = flushDenormal (x - G * y);
        return y;
    }

private:
    double sampleRate = 44100.0;
    float freqHz = 1000.0f, G = 0.0f, s = 0.0f;
};

//==============================================================================
// Pan a potenza costante
//==============================================================================

inline void constantPowerPan (float pan, float& lGain, float& rGain) noexcept
{
    pan = std::clamp (pan, -1.0f, 1.0f);
    const float angle = (pan + 1.0f) * 0.25f * kPi;   // 0 .. pi/2
    lGain = std::cos (angle);
    rGain = std::sin (angle);
}

} // namespace bassx
