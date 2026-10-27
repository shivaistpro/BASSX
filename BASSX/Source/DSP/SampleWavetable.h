/*
  ==============================================================================

    BASSX — SampleWavetable.h

    Un campione audio trascinato sull'oscillatore diventa una wavetable:

      1. mono, normalizzato, silenzio iniziale saltato;
      2. periodo trovato con autocorrelazione normalizzata (stile YIN) sulla
         parte centrale: se il suono ha un'intonazione, ogni frame e' UN
         ciclo vero e la wavetable suona intonata sulla tastiera;
         se non ce l'ha (rumore, batteria) il frame e' una finestra di 2048
         campioni, che diventa un timbro;
      3. 16 frame distribuiti lungo il campione (POSITION li attraversa:
         l'evoluzione del suono originale), ogni ciclo ricampionato a 2048
         punti, partendo da uno zero in salita, DC tolta, bordo chiuso;
      4. per ogni frame le stesse 10 versioni band-limited del banco di
         fabbrica (FFT, armoniche tagliate per livello): niente aliasing
         anche nelle note alte.

    Nessun JUCE oltre a juce_dsp (FFT). Thread messaggi o di lavoro, mai audio.

  ==============================================================================
*/

#pragma once

#include "WavetableBank.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>

namespace bassx
{

/** Indice del parametro "Wave" che suona la wavetable da campione. */
inline constexpr int kUserWaveIndex = 16;

namespace SampleWavetable
{
    /** Periodo in campioni (0 = nessuna intonazione chiara). */
    inline double detectPeriod (const std::vector<float>& x, double sr)
    {
        const int minLag = (int) (sr / 1000.0), maxLag = (int) (sr / 30.0);
        const int win = std::min ((int) x.size() / 2, maxLag * 3);
        if (win <= maxLag + 16) return 0.0;

        const int start = std::max (0, (int) x.size() / 2 - win / 2);
        std::vector<double> d ((size_t) maxLag + 2, 0.0);

        // differenza al quadrato normalizzata cumulativa (YIN)
        double running = 0.0;
        double best = 0.0; int bestLag = 0;
        for (int lag = 1; lag <= maxLag; ++lag)
        {
            double s = 0.0;
            for (int i = 0; i < win - maxLag; ++i)
            {
                const double diff = x[(size_t) (start + i)] - x[(size_t) (start + i + lag)];
                s += diff * diff;
            }
            running += s;
            d[(size_t) lag] = running > 0.0 ? s * lag / running : 1.0;

            if (lag >= minLag && d[(size_t) lag] < 0.15 && bestLag == 0)
            {
                // primo minimo sotto soglia: scende finche' continua a calare
                int l = lag;
                while (l + 1 <= maxLag)
                {
                    double s2 = 0.0;
                    for (int i = 0; i < win - maxLag; ++i)
                    {
                        const double diff = x[(size_t) (start + i)] - x[(size_t) (start + i + l + 1)];
                        s2 += diff * diff;
                    }
                    const double dn = (running + s2) > 0.0 ? s2 * (l + 1) / (running + s2) : 1.0;
                    if (dn >= d[(size_t) l]) break;
                    running += s2; ++l; d[(size_t) l] = dn;
                }
                bestLag = l; best = d[(size_t) l];
                break;
            }
        }

        if (bestLag == 0 || best > 0.25) return 0.0;

        // interpolazione parabolica del minimo
        const double a = d[(size_t) bestLag - 1], b = d[(size_t) bestLag];
        const double c = (bestLag + 1 < (int) d.size()) ? d[(size_t) bestLag + 1] : b;
        const double den = a - 2.0 * b + c;
        return (double) bestLag + (std::abs (den) > 1e-12 ? 0.5 * (a - c) / den : 0.0);
    }

    /** Le 10 mip band-limited di un ciclo di kTableSize punti. */
    inline void renderMips (const std::vector<float>& cycle, WavetableFrame& out, juce::dsp::FFT& fft,
                            std::vector<float>& scratch, float& normalisation, bool firstFrame)
    {
        std::fill (scratch.begin(), scratch.end(), 0.0f);
        std::copy (cycle.begin(), cycle.end(), scratch.begin());
        fft.performRealOnlyForwardTransform (scratch.data(), true);
        std::vector<float> spectrum (scratch.begin(), scratch.begin() + kTableSize + 2);

        // fondamentale a +sin come le tabelle di fabbrica: si somma in fase con gli altri layer
        const float shift = fundamentalShift (spectrum[2], spectrum[3]);
        for (int h = 1; h < kTableSize / 2; ++h)
        {
            const float c = std::cos ((float) h * shift), s = std::sin ((float) h * shift);
            const float re = spectrum[(size_t) (2 * h)], im = spectrum[(size_t) (2 * h + 1)];
            spectrum[(size_t) (2 * h)]     = re * c - im * s;
            spectrum[(size_t) (2 * h + 1)] = re * s + im * c;
        }

        for (int level = 0; level < kNumMipLevels; ++level)
        {
            const int maxHarm = std::min (mipMaxHarmonics (level), kTableSize / 2 - 1);
            std::fill (scratch.begin(), scratch.end(), 0.0f);
            for (int h = 1; h <= maxHarm; ++h)
            {
                scratch[(size_t) (2 * h)]     = spectrum[(size_t) (2 * h)];
                scratch[(size_t) (2 * h + 1)] = spectrum[(size_t) (2 * h + 1)];
                const int m = kTableSize - h;
                scratch[(size_t) (2 * m)]     =  spectrum[(size_t) (2 * h)];
                scratch[(size_t) (2 * m + 1)] = -spectrum[(size_t) (2 * h + 1)];
            }
            fft.performRealOnlyInverseTransform (scratch.data());

            if (level == 0 && firstFrame)
            {
                float peak = 0.0f;
                for (int i = 0; i < kTableSize; ++i) peak = std::max (peak, std::abs (scratch[(size_t) i]));
                normalisation = peak > 1.0e-9f ? 1.0f / peak : 1.0f;
            }

            auto& dest = out.mip[(size_t) level];
            dest.resize ((size_t) (kTableSize + kTableGuard));
            for (int i = 0; i < kTableSize; ++i) dest[(size_t) i] = scratch[(size_t) i] * normalisation;
            for (int g = 0; g < kTableGuard; ++g) dest[(size_t) (kTableSize + g)] = dest[(size_t) g];
        }
    }

    /** Ricostruisce le mip dai cicli di livello 0 (stato salvato). */
    inline void buildFromCycles (const std::vector<std::vector<float>>& cycles, Wavetable& out)
    {
        juce::dsp::FFT fft (11);
        std::vector<float> scratch ((size_t) kTableSize * 2, 0.0f);
        out.frames.assign (cycles.size(), {});
        float norm = 1.0f;
        for (size_t f = 0; f < cycles.size(); ++f)
            renderMips (cycles[f], out.frames[f], fft, scratch, norm, f == 0);
    }

    /**
        @param cyclesOut  i cicli a kTableSize punti (livello 0, non
                          normalizzati): e' cio' che si salva nello stato
        @return false se il campione e' vuoto o troppo corto
    */
    inline bool build (const juce::AudioBuffer<float>& audio, double sr, const juce::String& name,
                       Wavetable& out, std::vector<std::vector<float>>& cyclesOut, bool& pitched)
    {
        const int n = audio.getNumSamples();
        if (n < 512 || audio.getNumChannels() == 0) return false;

        std::vector<float> x ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            float s = 0.0f;
            for (int ch = 0; ch < audio.getNumChannels(); ++ch) s += audio.getSample (ch, i);
            x[(size_t) i] = s / (float) audio.getNumChannels();
        }

        float peak = 0.0f;
        for (auto v : x) peak = std::max (peak, std::abs (v));
        if (peak < 1.0e-5f) return false;

        // salta il silenzio iniziale e finale (-50 dB)
        int a = 0, b = n - 1;
        while (a < n && std::abs (x[(size_t) a]) < peak * 0.003f) ++a;
        while (b > a && std::abs (x[(size_t) b]) < peak * 0.003f) --b;
        x = std::vector<float> (x.begin() + a, x.begin() + b + 1);
        const int len = (int) x.size();
        if (len < 256) return false;

        const double period = detectPeriod (x, sr);
        pitched = period > 0.0;
        const double cycleLen = pitched ? period : std::min ((double) kTableSize, (double) len - 2.0);

        constexpr int kFrames = kFramesPerMorphTable;
        cyclesOut.assign ((size_t) kFrames, std::vector<float> ((size_t) kTableSize, 0.0f));
        const int usable = std::max (1, len - (int) std::ceil (cycleLen) - 2);

        for (int f = 0; f < kFrames; ++f)
        {
            int start = (int) ((double) usable * f / std::max (1, kFrames - 1));
            // zero in salita vicino all'inizio: frame allineati, niente salti di fase fra frame
            for (int k = 0; k < (int) cycleLen && start + k + 1 < len; ++k)
                if (x[(size_t) (start + k)] <= 0.0f && x[(size_t) (start + k + 1)] > 0.0f) { start += k; break; }
            start = std::min (start, std::max (0, len - (int) std::ceil (cycleLen) - 2));

            auto& cyc = cyclesOut[(size_t) f];
            double mean = 0.0;
            for (int i = 0; i < kTableSize; ++i)
            {
                const double pos = start + cycleLen * i / kTableSize;
                const int i0 = std::min (len - 2, (int) pos);
                const double fr = pos - i0;
                cyc[(size_t) i] = (float) (x[(size_t) i0] + (x[(size_t) i0 + 1] - x[(size_t) i0]) * fr);
                mean += cyc[(size_t) i];
            }
            mean /= kTableSize;
            for (auto& v : cyc) v -= (float) mean;

            // bordo chiuso: gli ultimi 64 punti sfumano verso l'inizio
            constexpr int kSeam = 64;
            for (int i = 0; i < kSeam; ++i)
            {
                const float t = (float) (i + 1) / (float) kSeam;
                auto& v = cyc[(size_t) (kTableSize - kSeam + i)];
                v = v + (cyc[(size_t) i] - v) * t * 0.5f;
            }
        }

        buildFromCycles (cyclesOut, out);
        out.name = name;
        return true;
    }
}

} // namespace bassx
