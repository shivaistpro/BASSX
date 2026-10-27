/*
  ==============================================================================

    BASSX — NeuralDrive.h

    Inferenza della GRU che modella il preamp a diodi (vedi
    tools/neural/train_neural_drive.py per il circuito e l'addestramento).

    Perché una GRU e non una curva statica:
      Il circuito ha memoria — condensatore di accoppiamento in ingresso e
      condensatore di carico sui diodi. La saturazione dipende quindi dalla
      storia recente del segnale: i transienti dell'accento vengono schiacciati
      diversamente da una nota tenuta, e la distorsione si "ammorbidisce" in
      alto. Una waveshaper statica non può farlo; risolvere l'ODE con
      Newton-Raphson per campione può, ma con costo variabile. La rete fa lo
      stesso lavoro a costo fisso: 12 stati, ~520 moltiplicazioni per campione.

    Integrazione:
      Il drive arriva alla rete come secondo ingresso (condizionamento): la
      stessa rete copre tutto il range, da pulito a saturo, senza commutare
      modelli. Il dry/wet a valle permette dosaggi intermedi.

    Tempo reale:
      Nessuna allocazione, stato in array fissi. Tanh e sigmoide in forma
      razionale veloce: l'errore (<1e-3) è sotto l'errore del modello.

  ==============================================================================
*/

#pragma once

#include "Common.h"
#include "NeuralDriveWeights.h"

namespace bassx
{

class NeuralDrive
{
public:
    static constexpr int H = neural::kHidden;

    void reset() noexcept { h.fill (0.0f); }

    /** Un campione. `drive` in 0..1 è l'ingresso di condizionamento. */
    float process (float x, float drive) noexcept
    {
        using namespace neural;

        // Il modello è addestrato su ampiezze fino a ~1: oltre si limita
        // morbidamente l'ingresso invece di estrapolare fuori dal dataset.
        x = softLimit (x);

        std::array<float, 3 * H> gi {}, gh {};

        for (int r = 0; r < 3 * H; ++r)
        {
            gi[(size_t) r] = Wih[r * 2] * x + Wih[r * 2 + 1] * drive + Bih[r];

            float acc = Bhh[r];
            const float* w = Whh + r * H;

            for (int c = 0; c < H; ++c)
                acc += w[c] * h[(size_t) c];

            gh[(size_t) r] = acc;
        }

        float y = Bout;

        for (int j = 0; j < H; ++j)
        {
            const float rGate = sigmoid (gi[(size_t) j] + gh[(size_t) j]);
            const float zGate = sigmoid (gi[(size_t) (H + j)] + gh[(size_t) (H + j)]);
            const float nGate = fastTanhAccurate (gi[(size_t) (2 * H + j)] + rGate * gh[(size_t) (2 * H + j)]);

            h[(size_t) j] = flushDenormal ((1.0f - zGate) * nGate + zGate * h[(size_t) j]);
            y += Wout[j] * h[(size_t) j];
        }

        return y;
    }

private:
    static float sigmoid (float v) noexcept
    {
        return 0.5f + 0.5f * fastTanhAccurate (0.5f * v);
    }

    /** Tanh razionale (Padé 7/6) con clamp: errore < 2e-4 su tutto l'asse. */
    static float fastTanhAccurate (float v) noexcept
    {
        v = std::clamp (v, -4.97f, 4.97f);
        const float v2 = v * v;
        const float num = v * (135135.0f + v2 * (17325.0f + v2 * (378.0f + v2)));
        const float den = 135135.0f + v2 * (62370.0f + v2 * (3150.0f + v2 * 28.0f));
        return num / den;
    }

    static float softLimit (float v) noexcept
    {
        constexpr float knee = 1.2f;
        const float a = std::abs (v);
        return a <= knee ? v : std::copysign (knee + fastTanhAccurate (a - knee) * 0.3f, v);
    }

    std::array<float, H> h {};
};

} // namespace bassx
