/*
  ==============================================================================

    BASSX — WavetableBank.cpp

  ==============================================================================
*/

#include "WavetableBank.h"

namespace bassx
{

//==============================================================================
namespace
{
    /** Fase "seno": le armoniche di una rampa o di un'onda quadra ideali sono
        in fase di seno, non di coseno. Usare la fase di coseno produrrebbe la
        stessa analisi spettrale ma una forma d'onda diversa nel tempo — e
        siccome l'oscillatore viene resettato di fase a ogni nota non slidata,
        la forma nel tempo è udibile nel transiente d'attacco. */
    constexpr float kSinePhase = -kHalfPi;

    /** Finestra di rolloff morbida applicata al bordo dello spettro quando una
        forma deve "aprirsi" progressivamente (morph sinusoide -> rampa).
        Un taglio netto genererebbe ringing di Gibbs nel dominio del tempo, che
        si sente come un pre-eco sul transiente. */
    inline float softEdge (float x, float edge, float width) noexcept
    {
        if (width <= 0.0f)
            return x < edge ? 1.0f : 0.0f;

        const float t = std::clamp ((edge - x) / width + 0.5f, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);       // smoothstep
    }

    /** Picco risonante in scala logaritmica di armonica: emula un filtro
        passa-banda applicato allo spettro. */
    inline float resonantPeak (float harmonic, float centreHarm, float q, float gainDb) noexcept
    {
        const float octaves = std::log2 (std::max (harmonic, 0.5f) / std::max (centreHarm, 0.5f));
        const float bell    = std::exp (-(octaves * octaves) * q);
        return std::pow (10.0f, (gainDb * bell) * 0.05f);
    }
}

//==============================================================================
WavetableBank::WavetableBank()
{
    fftScratch.resize ((size_t) kTableSize * 2, 0.0f);
    tables.reserve (16);

    // L'ordine qui definisce l'indice del parametro "Wave" nella GUI: non
    // riordinare senza bumpare la versione dello stato, altrimenti i preset
    // salvati cambiano suono.
    buildTable ("Analog Saw",     1,                     [this] (Spectrum& s, float p) { specAnalogSaw      (s, p); });
    buildTable ("Analog Square",  1,                     [this] (Spectrum& s, float p) { specAnalogSquare   (s, p); });
    buildTable ("Analog Tri",     1,                     [this] (Spectrum& s, float p) { specAnalogTriangle (s, p); });
    buildTable ("Acid Saw",       1,                     [this] (Spectrum& s, float p) { specAcidSaw        (s, p); });
    buildTable ("Acid Square",    1,                     [this] (Spectrum& s, float p) { specAcidSquare     (s, p); });
    buildTable ("Sine > Saw",     kFramesPerMorphTable,  [this] (Spectrum& s, float p) { specSineToSaw      (s, p); });
    buildTable ("Pulse Morph",    kFramesPerMorphTable,  [this] (Spectrum& s, float p) { specPulseMorph     (s, p); });
    buildTable ("Formant Bass",   kFramesPerMorphTable,  [this] (Spectrum& s, float p) { specFormantBass    (s, p); });
    buildTable ("Reso Sweep",     kFramesPerMorphTable,  [this] (Spectrum& s, float p) { specResoSweep      (s, p); });
    buildTable ("FM Bass",        kFramesPerMorphTable,  [this] (Spectrum& s, float p) { specFMBass         (s, p); });
    buildTable ("Harmonic Stack", kFramesPerMorphTable,  [this] (Spectrum& s, float p) { specHarmonicStack  (s, p); });
    buildTable ("Folded Sine",    kFramesPerMorphTable,  [this] (Spectrum& s, float p) { specFoldedSine     (s, p); });
    buildTable ("Digital",        kFramesPerMorphTable,  [this] (Spectrum& s, float p) { specDigitalBass    (s, p); });
    buildTable ("Growl",          kFramesPerMorphTable,  [this] (Spectrum& s, float p) { specGrowl          (s, p); });
    buildTable ("Sub Harmonic",   kFramesPerMorphTable,  [this] (Spectrum& s, float p) { specSubHarmonic    (s, p); });
    buildTable ("Metallic",       kFramesPerMorphTable,  [this] (Spectrum& s, float p) { specMetallic       (s, p); });
}

juce::StringArray WavetableBank::getTableNames() const
{
    juce::StringArray names;
    for (const auto& t : tables)
        names.add (t.name);
    return names;
}

//==============================================================================
void WavetableBank::buildTable (const juce::String& name, int numFrames,
                                const std::function<void (Spectrum&, float)>& fillSpectrum)
{
    Wavetable table;
    table.name = name;
    table.frames.resize ((size_t) juce::jmax (1, numFrames));

    Spectrum spec;

    for (int f = 0; f < (int) table.frames.size(); ++f)
    {
        const float pos = (table.frames.size() > 1)
                            ? (float) f / (float) (table.frames.size() - 1)
                            : 0.0f;

        spec.clear();
        fillSpectrum (spec, pos);
        renderFrame (spec, table.frames[(size_t) f]);
    }

    tables.push_back (std::move (table));
}

//==============================================================================
void WavetableBank::renderFrame (const Spectrum& spec, WavetableFrame& out)
{
    // Il fattore di normalizzazione viene calcolato UNA VOLTA sul livello di
    // mip più ricco e applicato identico a tutti i livelli. Normalizzare ogni
    // mip separatamente farebbe saltare il livello di uscita quando
    // l'oscillatore cambia mip durante un glissando — un gradino di volume
    // esattamente nel punto in cui la nota si muove.
    float normalisation = 1.0f;

    // fondamentale a +sin: layer in fase fra tabelle diverse (vedi fundamentalShift)
    const float shift = fundamentalShift (spec.magnitude[1] * std::cos (spec.phase[1]),
                                          spec.magnitude[1] * std::sin (spec.phase[1]));

    for (int level = 0; level < kNumMipLevels; ++level)
    {
        const int maxHarm = juce::jmin (mipMaxHarmonics (level), kTableSize / 2 - 1);

        std::fill (fftScratch.begin(), fftScratch.end(), 0.0f);

        for (int h = 1; h <= maxHarm; ++h)
        {
            const float mag = spec.magnitude[(size_t) h];

            if (std::abs (mag) <= 0.0f)
                continue;

            const float ph = spec.phase[(size_t) h] + (float) h * shift;
            const float re = mag * std::cos (ph);
            const float im = mag * std::sin (ph);

            // Simmetria hermitiana: bin h e bin N-h coniugati. Senza il
            // coniugato la trasformata inversa produrrebbe una parte
            // immaginaria non nulla che JUCE scarta, dimezzando l'ampiezza e
            // introducendo un errore di fase dipendente dall'armonica.
            fftScratch[(size_t) (2 * h)]     = re;
            fftScratch[(size_t) (2 * h + 1)] = im;

            const int mirror = kTableSize - h;
            fftScratch[(size_t) (2 * mirror)]     =  re;
            fftScratch[(size_t) (2 * mirror + 1)] = -im;
        }

        fft.performRealOnlyInverseTransform (fftScratch.data());

        auto& dest = out.mip[(size_t) level];
        dest.resize ((size_t) (kTableSize + kTableGuard));

        if (level == 0)
        {
            float peak = 0.0f;
            for (int i = 0; i < kTableSize; ++i)
                peak = juce::jmax (peak, std::abs (fftScratch[(size_t) i]));

            normalisation = (peak > 1.0e-9f) ? (1.0f / peak) : 1.0f;
        }

        for (int i = 0; i < kTableSize; ++i)
            dest[(size_t) i] = fftScratch[(size_t) i] * normalisation;

        // Campioni di guardia: copie circolari dei primi, così l'interpolazione
        // di Hermite può leggere i+1 e i+2 senza test di wrap per campione.
        for (int g = 0; g < kTableGuard; ++g)
            dest[(size_t) (kTableSize + g)] = dest[(size_t) g];
    }
}

//==============================================================================
void WavetableBank::spectrumFromTimeDomain (const std::function<float (float)>& fn, Spectrum& out)
{
    std::fill (fftScratch.begin(), fftScratch.end(), 0.0f);

    for (int i = 0; i < kTableSize; ++i)
        fftScratch[(size_t) i] = fn ((float) i / (float) kTableSize);

    fft.performRealOnlyForwardTransform (fftScratch.data(), true);

    out.clear();

    const float scale = 2.0f / (float) kTableSize;

    for (int h = 1; h < kTableSize / 2; ++h)
    {
        const float re = fftScratch[(size_t) (2 * h)]     * scale;
        const float im = fftScratch[(size_t) (2 * h + 1)] * scale;

        const float mag = std::sqrt (re * re + im * im);

        // Sotto -110 dB rispetto al fondo scala l'armonica è indistinguibile
        // dal rumore di quantizzazione a 24 bit: scartarla accelera la
        // renderizzazione dei mip senza differenza udibile.
        if (mag < 3.0e-6f)
            continue;

        out.set (h, mag, std::atan2 (im, re));
    }
}

//==============================================================================
// Generatori di spettro
//==============================================================================

void WavetableBank::specAnalogSaw (Spectrum& s, float)
{
    // Rampa ideale: ampiezza 1/n, fase di seno. Il segno alternato non serve
    // perché la fase costante di seno produce già la rampa discendente
    // classica; l'ampiezza 1/n è la firma armonica del dente di sega.
    for (int h = 1; h < kTableSize / 2; ++h)
        s.set (h, 1.0f / (float) h, kSinePhase);
}

void WavetableBank::specAnalogSquare (Spectrum& s, float)
{
    for (int h = 1; h < kTableSize / 2; h += 2)
        s.set (h, 1.0f / (float) h, kSinePhase);
}

void WavetableBank::specAnalogTriangle (Spectrum& s, float)
{
    // 1/n^2 sulle sole armoniche dispari, con inversione di fase ogni due
    // armoniche dispari: è quello che distingue una triangolare da una quadra
    // filtrata a 12 dB, che altrimenti avrebbe lo stesso inviluppo spettrale.
    int sign = 1;

    for (int h = 1; h < kTableSize / 2; h += 2)
    {
        s.set (h, 1.0f / ((float) h * (float) h),
               sign > 0 ? kSinePhase : kSinePhase + kPi);
        sign = -sign;
    }
}

void WavetableBank::specAcidSaw (Spectrum& s, float)
{
    // La rampa del 303 non è ideale: il condensatore del VCO si scarica
    // attraverso un transistor, quindi il ritorno non è istantaneo e la
    // pendenza non è perfettamente lineare. Le conseguenze misurabili sono
    // due: un lieve smorzamento delle armoniche alte e una piccola quantità di
    // armoniche pari in più rispetto alla rampa teorica.
    for (int h = 1; h < kTableSize / 2; ++h)
    {
        const float base    = 1.0f / (float) h;
        const float rolloff = std::exp (-(float) h / 420.0f);
        const float evenBoost = (h % 2 == 0) ? 1.12f : 1.0f;

        s.set (h, base * rolloff * evenBoost, kSinePhase);
    }
}

void WavetableBank::specAcidSquare (Spectrum& s, float)
{
    // L'onda "quadra" del 303 è in realtà un impulso al 48% circa, derivato
    // dalla stessa rampa tramite un comparatore. Il duty cycle non simmetrico
    // introduce armoniche pari deboli — è il motivo per cui la quadra del 303
    // suona più "nasale" di una quadra ideale.
    constexpr float duty = 0.48f;

    for (int h = 1; h < kTableSize / 2; ++h)
    {
        const float mag = std::abs (std::sin (kPi * (float) h * duty)) / (float) h;
        const float rolloff = std::exp (-(float) h / 380.0f);
        s.set (h, mag * 2.0f * rolloff, kSinePhase);
    }
}

void WavetableBank::specSineToSaw (Spectrum& s, float pos)
{
    // Il numero di armoniche cresce esponenzialmente con la posizione: una
    // crescita lineare concentrerebbe tutto il cambiamento timbrico
    // percepibile nel primo 10% della corsa.
    const float maxH  = expMap (pos, 1.0f, 512.0f);
    const float width = juce::jmax (1.0f, maxH * 0.35f);

    for (int h = 1; h < kTableSize / 2; ++h)
    {
        const float edge = softEdge ((float) h, maxH, width);

        if (edge <= 1.0e-5f)
            break;

        s.set (h, edge / (float) h, kSinePhase);
    }
}

void WavetableBank::specPulseMorph (Spectrum& s, float pos)
{
    // Duty dal 50% (quadra) al 4% (impulso stretto e brillante).
    const float duty = lerp (0.50f, 0.04f, pos);

    for (int h = 1; h < kTableSize / 2; ++h)
    {
        const float mag = std::abs (std::sin (kPi * (float) h * duty)) / (float) h;

        if (mag < 1.0e-6f)
            continue;

        s.set (h, mag * 2.0f, kSinePhase);
    }
}

void WavetableBank::specFormantBass (Spectrum& s, float pos)
{
    // Tre formanti che percorrono la sequenza vocalica A -> E -> I -> O -> U.
    // Le frequenze sono quelle medie di una voce maschile; espresse in
    // armoniche di una fondamentale di riferimento a 110 Hz, che è il registro
    // in cui un basso passa la maggior parte del tempo.
    struct Vowel { float f1, f2, f3; };

    static const Vowel vowels[5] =
    {
        {  730.0f, 1090.0f, 2440.0f },   // A
        {  530.0f, 1840.0f, 2480.0f },   // E
        {  270.0f, 2290.0f, 3010.0f },   // I
        {  570.0f,  840.0f, 2410.0f },   // O
        {  300.0f,  870.0f, 2240.0f }    // U
    };

    const float t   = std::clamp (pos, 0.0f, 1.0f) * 4.0f;
    const int   i0  = juce::jlimit (0, 4, (int) t);
    const int   i1  = juce::jlimit (0, 4, i0 + 1);
    const float mix = t - (float) i0;

    const float f1 = lerp (vowels[i0].f1, vowels[i1].f1, mix);
    const float f2 = lerp (vowels[i0].f2, vowels[i1].f2, mix);
    const float f3 = lerp (vowels[i0].f3, vowels[i1].f3, mix);

    constexpr float fundamental = 110.0f;

    for (int h = 1; h < kTableSize / 2; ++h)
    {
        const float hz = fundamental * (float) h;

        if (hz > 16000.0f)
            break;

        float mag = 1.0f / (float) h;                       // sorgente: rampa

        mag *= resonantPeak (hz, f1, 3.2f, 14.0f);
        mag *= resonantPeak (hz, f2, 4.0f, 10.0f);
        mag *= resonantPeak (hz, f3, 5.0f,  6.0f);

        s.set (h, mag, kSinePhase);
    }
}

void WavetableBank::specResoSweep (Spectrum& s, float pos)
{
    // Un picco risonante che sale di cinque ottave lungo il morph. Usato come
    // sorgente per bassi "modulati": modulando la position con un LFO o un
    // inviluppo si ottiene uno sweep di filtro che non costa un filtro e non
    // ha il calo di basse frequenze che la risonanza porta con sé.
    const float centre = expMap (pos, 1.5f, 48.0f);

    for (int h = 1; h < kTableSize / 2; ++h)
    {
        const float base = 1.0f / (float) h;
        const float peak = resonantPeak ((float) h, centre, 2.0f, 20.0f);
        const float roll = std::exp (-(float) h / 300.0f);

        const float mag = base * peak * roll;

        if (mag < 1.0e-6f && h > (int) centre * 4)
            break;

        s.set (h, mag, kSinePhase);
    }
}

void WavetableBank::specFMBass (Spectrum& s, float pos)
{
    // Lo spettro della FM è una serie di funzioni di Bessel: scriverlo in
    // forma chiusa richiederebbe J_k(I) per k fino a ~I+10. È più semplice e
    // numericamente più solido generare la forma nel tempo e analizzarla, dato
    // che con indice massimo 9 e rapporto 2:1 l'armonica più alta significativa
    // sta intorno alla 25ª — tre ordini di grandezza sotto il limite della
    // tabella, quindi l'analisi non alias.
    const float index = pos * 9.0f;
    static constexpr float ratio = 2.0f;   // static: MSVC non cattura implicitamente i constexpr locali (C3493)

    spectrumFromTimeDomain ([index] (float phase)
    {
        const float modulator = std::sin (kTwoPi * ratio * phase);
        return std::sin (kTwoPi * phase + index * modulator);
    }, s);
}

void WavetableBank::specHarmonicStack (Spectrum& s, float pos)
{
    // Bilanciamento pari/dispari. A pos = 0 solo dispari (timbro cavo, tipo
    // clarinetto/quadra); a pos = 1 tutte (timbro pieno, tipo rampa). Il
    // passaggio è la trasformazione timbrica più utile su un basso perché
    // cambia il carattere senza cambiare il peso in basso.
    const float evenGain = pos;

    for (int h = 1; h < kTableSize / 2; ++h)
    {
        const float amp = 1.0f / (float) h;
        const float g   = (h % 2 == 1) ? 1.0f : evenGain;
        const float roll = std::exp (-(float) h / 260.0f);

        s.set (h, amp * g * roll, kSinePhase);
    }
}

void WavetableBank::specFoldedSine (Spectrum& s, float pos)
{
    // Wavefolding progressivo di una sinusoide: genera armoniche dispari che
    // crescono in modo non monotono, con "buchi" spettrali che si spostano.
    // È la sorgente classica dei bassi west-coast.
    const float amount = pos;

    spectrumFromTimeDomain ([amount] (float phase)
    {
        const float sine = std::sin (kTwoPi * phase);
        return waveFold (sine, amount);
    }, s);
}

void WavetableBank::specDigitalBass (Spectrum& s, float pos)
{
    // Riduzione progressiva della risoluzione in ampiezza applicata a una
    // rampa. Costruito nel dominio del tempo con una rampa GIÀ band-limited
    // (somma di 64 armoniche) invece che con la rampa ideale: quantizzare una
    // rampa a gradini produrrebbe armoniche fino a Nyquist della tabella che
    // l'analisi ripiegherebbe come rumore inarmonico.
    const float bits = lerp (16.0f, 2.5f, pos);

    spectrumFromTimeDomain ([bits] (float phase)
    {
        float ramp = 0.0f;
        for (int h = 1; h <= 64; ++h)
            ramp += std::sin (kTwoPi * (float) h * phase) / (float) h;

        ramp *= 0.55f;                     // riporta il picco vicino a 1
        return bitCrush (std::clamp (ramp, -1.0f, 1.0f), bits);
    }, s);
}

void WavetableBank::specGrowl (Spectrum& s, float pos)
{
    // Filtro a pettine applicato allo spettro di una rampa: le notch si
    // spostano con la posizione. Modulare la position con un LFO lento produce
    // il classico "growl" dei bassi neuro, senza usare un flanger (che
    // introdurrebbe ritardo e quindi problemi di fase in mono).
    const float combFreq  = lerp (2.0f, 11.0f, pos);
    const float combDepth = 0.92f;

    for (int h = 1; h < kTableSize / 2; ++h)
    {
        const float base  = 1.0f / (float) h;
        const float notch = 1.0f - combDepth * 0.5f * (1.0f - std::cos (kTwoPi * (float) h / combFreq));
        const float roll  = std::exp (-(float) h / 220.0f);

        s.set (h, base * std::abs (notch) * roll, kSinePhase);
    }
}

void WavetableBank::specSubHarmonic (Spectrum& s, float pos)
{
    // Fondamentale dominante più un'armonica superiore che sale lungo il
    // morph. Progettata per il layer di sub: energia concentrata in basso, con
    // giusto quel tanto di secondo parziale da renderla udibile su casse
    // piccole senza sporcare il mix.
    const float partial = 2.0f + pos * 6.0f;
    const int   pLow    = (int) std::floor (partial);
    const float pMix    = partial - (float) pLow;

    s.set (1, 1.0f, kSinePhase);
    s.set (pLow,     0.42f * (1.0f - pMix), kSinePhase);
    s.set (pLow + 1, 0.42f * pMix,          kSinePhase);

    // Terzo parziale debole e fisso: dà definizione all'attacco senza spostare
    // il centro di gravità dello spettro.
    s.set (3, 0.10f, kSinePhase);
}

void WavetableBank::specMetallic (Spectrum& s, float pos)
{
    // Ampiezze pseudo-casuali ma deterministiche su una serie armonica intera:
    // resta rigorosamente periodica (quindi utilizzabile come wavetable) ma
    // percettivamente inarmonica, come una campana. Il seme dipende dal frame,
    // quindi il morph attraversa timbri metallici diversi.
    Xorshift rng (0x5BD1E995u + (uint32_t) (pos * 65535.0f));

    const float density = lerp (0.25f, 0.85f, pos);

    for (int h = 1; h < 96; ++h)
    {
        if (rng.nextUnipolar() > density && h > 1)
            continue;

        const float amp = (0.35f + 0.65f * rng.nextUnipolar()) / std::pow ((float) h, 0.85f);
        s.set (h, amp, rng.nextUnipolar() * kTwoPi);
    }
}

} // namespace bassx
