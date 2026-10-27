/*
  ==============================================================================

    BASSX — Envelope.h

    Inviluppo DAHDSR a curve regolabili e circuito di accento.

    Perché non juce::ADSR:
      juce::ADSR usa rampe lineari. Un inviluppo analogico è la carica o la
      scarica di un condensatore attraverso una resistenza, cioè una
      esponenziale. La differenza non è accademica: su un decay di 200 ms
      applicato al cutoff, la versione lineare passa metà del tempo nella metà
      alta della corsa, quella esponenziale ci passa il 30%. È esattamente
      quella asimmetria a produrre lo "squelch" — il morso iniziale seguito
      dalla coda lunga. Un decay lineare della stessa durata suona piatto.

    Il parametro di curva:
      Ogni segmento ha un controllo continuo da -1 (fortemente logaritmico,
      "parte piano e accelera") a +1 (fortemente esponenziale, "parte forte e
      rallenta"), con 0 = comportamento analogico standard. È realizzato
      variando il target asintotico dell'esponenziale, non applicando una
      trasformazione all'uscita: applicarla all'uscita sposterebbe anche il
      livello di sustain, che invece deve restare dove il potenziometro dice.

  ==============================================================================
*/

#pragma once

#include "Common.h"

namespace bassx
{

//==============================================================================
/**
    Inviluppo a sei stadi: Delay, Attack, Hold, Decay, Sustain, Release.

    Delay e Hold sono ciò che permette a un inviluppo di modulazione di
    generare gesti ritmici (un "wow" che arriva 80 ms dopo l'attacco) senza
    doverci sprecare un LFO.
*/
class Envelope
{
public:
    enum class Stage { Idle, Delay, Attack, Hold, Decay, Sustain, Release };

    //==========================================================================
    void prepare (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        recalculate();
        reset();
    }

    void reset() noexcept
    {
        level = 0.0f;
        stage = Stage::Idle;
        stageCounter = 0;
    }

    //==========================================================================
    void setDelay   (float sec) noexcept { delayTime  = std::max (0.0f, sec);      updateCounters(); }
    void setHold    (float sec) noexcept { holdTime   = std::max (0.0f, sec);      updateCounters(); }
    void setAttack  (float sec) noexcept { attackTime = std::max (0.0002f, sec);   recalculate(); }
    void setDecay   (float sec) noexcept { decayTime  = std::max (0.0005f, sec);   recalculate(); }
    void setSustain (float lvl) noexcept { sustain    = std::clamp (lvl, 0.0f, 1.0f); }
    void setRelease (float sec) noexcept { releaseTime= std::max (0.0005f, sec);   recalculate(); }

    /** -1 .. +1 per ciascun segmento. */
    void setCurves (float attackCurve, float decayCurve, float releaseCurve) noexcept
    {
        curveA = std::clamp (attackCurve,  -1.0f, 1.0f);
        curveD = std::clamp (decayCurve,   -1.0f, 1.0f);
        curveR = std::clamp (releaseCurve, -1.0f, 1.0f);
        recalculate();
    }

    /** Guadagno applicato all'uscita. Separato dal sustain perché la velocity
        deve scalare tutto l'inviluppo, non solo il suo plateau. */
    void setVelocityScaling (float amount) noexcept
    {
        velocityAmount = std::clamp (amount, 0.0f, 1.0f);
    }

    //==========================================================================
    /** @param retrigger  false implementa il legato: la nota nuova non
                          ricomincia l'inviluppo. È il comportamento dello slide
                          del sequencer originale, dove una nota legata è un
                          singolo evento continuo.
        @param velocity   0..1. */
    void noteOn (bool retrigger, float velocity) noexcept
    {
        velocityGain = lerp (1.0f, std::clamp (velocity, 0.0f, 1.0f), velocityAmount);

        if (! retrigger && stage != Stage::Idle)
            return;

        stageCounter = delaySamples;
        stage = (delaySamples > 0) ? Stage::Delay : Stage::Attack;

        // Non azzeriamo `level`: se l'inviluppo era ancora in release, ripartire
        // da zero produrrebbe una discontinuità. Ripartire dal valore corrente
        // è quello che fa un circuito analogico ed è anche l'unico modo di
        // evitare il click su note ribattute velocemente.
    }

    void noteOff() noexcept
    {
        if (stage != Stage::Idle)
            stage = Stage::Release;
    }

    /** Interrompe immediatamente: usato dal voice stealing. */
    void quickRelease (float seconds = 0.004f) noexcept
    {
        releaseTime = std::max (0.0005f, seconds);
        recalculate();
        stage = Stage::Release;
    }

    bool isActive() const noexcept { return stage != Stage::Idle; }
    bool isInRelease() const noexcept { return stage == Stage::Release; }
    Stage getStage() const noexcept { return stage; }
    float getCurrentLevel() const noexcept { return level * velocityGain; }

    //==========================================================================
    float getNextSample() noexcept
    {
        switch (stage)
        {
            case Stage::Idle:
                return 0.0f;

            case Stage::Delay:
                if (--stageCounter <= 0)
                    stage = Stage::Attack;
                break;

            case Stage::Attack:
                // Il target è oltre 1.0: puntare esattamente a 1.0 con una
                // esponenziale significa non arrivarci mai. Puntare oltre e
                // terminare al superamento dà un attacco che finisce in un
                // tempo definito, che è quello che l'utente si aspetta quando
                // scrive "5 ms".
                level = attackTarget + (level - attackTarget) * attackCoef;

                if (level >= 1.0f)
                {
                    level = 1.0f;
                    stageCounter = holdSamples;
                    stage = (holdSamples > 0) ? Stage::Hold : Stage::Decay;
                }
                break;

            case Stage::Hold:
                level = 1.0f;
                if (--stageCounter <= 0)
                    stage = Stage::Decay;
                break;

            case Stage::Decay:
                level = decayTarget + (level - decayTarget) * decayCoef;

                if (level - sustain < 1.0e-4f)
                {
                    level = sustain;
                    // Sustain a zero significa inviluppo "decay only": in quel
                    // caso l'inviluppo è finito e va liberato, altrimenti la
                    // voce resterebbe allocata a suonare silenzio.
                    stage = (sustain <= 1.0e-5f) ? Stage::Idle : Stage::Sustain;
                }
                break;

            case Stage::Sustain:
                level = sustain;
                break;

            case Stage::Release:
                level = releaseTarget + (level - releaseTarget) * releaseCoef;

                if (level < 1.0e-5f)
                {
                    level = 0.0f;
                    stage = Stage::Idle;
                }
                break;
        }

        return level * velocityGain;
    }

private:
    //==========================================================================
    /** Coefficiente di un filtro a un polo che raggiunge il 99.9% del target in
        `seconds`. */
    float coefFor (float seconds, float numTimeConstants) const noexcept
    {
        const float tau = std::max (seconds / numTimeConstants, 1.0e-6f);
        return std::exp (-1.0f / (tau * (float) sampleRate));
    }

    void updateCounters() noexcept
    {
        delaySamples = (int) (delayTime * (float) sampleRate);
        holdSamples  = (int) (holdTime  * (float) sampleRate);
    }

    void recalculate() noexcept
    {
        updateCounters();

        // --- Attacco ---------------------------------------------------------
        // curveA > 0  -> target più vicino a 1: la curva si appiattisce e
        //                l'attacco sembra "morbido".
        // curveA < 0  -> target molto oltre 1: la curva diventa quasi lineare,
        //                poi quasi a gradino.
        attackTarget = lerp (3.0f, 1.05f, (curveA + 1.0f) * 0.5f);

        // Numero di costanti di tempo per arrivare a 1 partendo da 0 con quel
        // target: t = tau * ln(T / (T - 1)).
        const float attackTCs = std::log (attackTarget / (attackTarget - 1.0f));
        attackCoef = coefFor (attackTime, std::max (attackTCs, 0.05f));

        // --- Decay -----------------------------------------------------------
        // Il target del decay è sotto il sustain: la curva raggiunge il sustain
        // mentre è ancora ripida, dando la caduta veloce caratteristica. Quanto
        // sotto, lo decide curveD.
        const float decayUndershoot = lerp (0.0f, 0.85f, (curveD + 1.0f) * 0.5f);
        decayTarget = sustain - decayUndershoot * (1.0f + sustain);

        const float decaySpan = std::max (1.0f - decayTarget, 1.0e-4f);
        const float decayEnd  = std::max (sustain - decayTarget, 1.0e-4f);
        const float decayTCs  = std::log (decaySpan / decayEnd);
        decayCoef = coefFor (decayTime, std::clamp (decayTCs, 0.3f, 12.0f));

        // --- Release ---------------------------------------------------------
        const float releaseUndershoot = lerp (0.0f, 0.85f, (curveR + 1.0f) * 0.5f);
        releaseTarget = -releaseUndershoot;

        const float relSpan = std::max (1.0f - releaseTarget, 1.0e-4f);
        const float relEnd  = std::max (1.0e-5f - releaseTarget, 1.0e-5f);
        const float relTCs  = std::log (relSpan / relEnd);
        releaseCoef = coefFor (releaseTime, std::clamp (relTCs, 0.3f, 14.0f));
    }

    //==========================================================================
    double sampleRate = 44100.0;

    float delayTime = 0.0f, attackTime = 0.003f, holdTime = 0.0f;
    float decayTime = 0.25f, sustain = 0.0f, releaseTime = 0.08f;
    float curveA = 0.0f, curveD = 0.0f, curveR = 0.0f;

    float attackCoef = 0.0f, decayCoef = 0.0f, releaseCoef = 0.0f;
    float attackTarget = 1.6f, decayTarget = 0.0f, releaseTarget = 0.0f;

    int delaySamples = 0, holdSamples = 0, stageCounter = 0;

    float level = 0.0f;
    float velocityGain = 1.0f, velocityAmount = 0.0f;

    Stage stage = Stage::Idle;
};

//==============================================================================
/**
    Circuito di accento.

    Sulla macchina originale l'accento non è "più forte". La linea di accento
    carica un condensatore attraverso una resistenza; quella tensione viene
    sommata all'ingresso di controllo del VCF e a quello del VCA. Due
    conseguenze musicali:

    1. Lo sweep che aggiunge ha una propria costante di scarica, indipendente
       dall'inviluppo di filtro, quindi una nota accentata ha una coda più
       lunga e più aperta.

    2. Il condensatore NON si scarica completamente fra accenti ravvicinati.
       Una serie di sedicesimi accentati si apre progressivamente. È il
       "pumping" che fa vivere le linee acid, ed è il dettaglio più spesso
       mancante nelle emulazioni.

    Aggiunta rispetto all'hardware: il tempo di scarica è esposto come
    parametro. Sul 303 è fisso a ~200 ms.
*/
class AccentCircuit
{
public:
    void prepare (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        setDecayTime (decayTime);
        reset();
    }

    void reset() noexcept { charge = target = 0.0f; }

    void setDecayTime (float seconds) noexcept
    {
        decayTime = std::max (seconds, 0.005f);
        const float tau = decayTime / 4.6f;
        decayCoef = std::exp (-1.0f / (tau * (float) sampleRate));
    }

    /** Tetto di accumulazione. 1.6 riproduce la saturazione del condensatore
        reale: senza un tetto, una raffica di trentaduesimi accentati porterebbe
        la carica a valori che spalancano il filtro e rendono la sequenza
        ingestibile. */
    void setMaxCharge (float m) noexcept { maxCharge = std::clamp (m, 1.0f, 4.0f); }

    /** Velocità di carica ("sweep speed" del Devil Fish): 0 = istantanea come
        sul 303 originale; tempi lunghi fanno salire l'accento in modo morbido,
        e le raffiche di accenti ravvicinati "gonfiano" il filtro lentamente. */
    void setAttackTime (float seconds) noexcept
    {
        attackCoef = (seconds <= 0.0005f) ? 0.0f
                   : std::exp (-1.0f / ((seconds / 4.6f) * (float) sampleRate));
    }

    void trigger (float amount) noexcept
    {
        target = std::min (maxCharge, target + amount);

        if (attackCoef <= 0.0f)
            charge = target;
    }

    float getNextSample() noexcept
    {
        target = flushDenormal (target * decayCoef);
        charge = flushDenormal (target + (charge - target) * attackCoef);
        return charge;
    }

    float getCurrent() const noexcept { return charge; }

private:
    double sampleRate = 44100.0;
    float charge = 0.0f, target = 0.0f, attackCoef = 0.0f;
    float decayTime = 0.22f;
    float decayCoef = 0.999f;
    float maxCharge = 1.6f;
};

} // namespace bassx
