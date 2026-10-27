/*
  ==============================================================================

    BASSX — TubeStage.h

    Stadio valvolare: il motore DSP di ETA-1 (eta::TubeEngine, modelli Koren
    triodo/pentodo risolti sulla retta di carico, sag dell'alimentatore,
    blocking da grid-leak, trasformatore d'uscita) dentro BASSX.

    Sta in testa alla catena effetti, come un preamplificatore: la valvola
    deve vedere il basso pulito, perché la sua compressione dinamica (sag)
    reagisce agli accenti — è lì che il 303 "respira".

    Oversampling 4x FIR a fase lineare: latenza intera, quindi il ramo
    asciutto viene ritardato esattamente e il mix parallelo non fa pettine.
    Latenza costante anche a stadio spento: la traccia non si sposta.

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>
#include "Common.h"
#include "eta/TubeEngine.h"

namespace bassx
{

class TubeStage
{
public:
    static juce::StringArray getModelNames()
    {
        juce::StringArray names;
        for (const auto& p : eta::profiles())
            names.add (p.name);
        return names;
    }

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        maxBlock = (int) spec.maximumBlockSize;

        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
            2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
        oversampler->initProcessing ((size_t) maxBlock);

        latencySamples = (int) std::lround (oversampler->getLatencyInSamples());

        for (auto& d : dryDelay) d.prepare (latencySamples + 4);
        dryBuffer.setSize (2, maxBlock, false, true, false);

        engine.prepare (spec.sampleRate * 4.0);
        for (auto& d : dcOut) d.prepare (spec.sampleRate, 10.0f);

        mixSmoother.prepare (spec.sampleRate, 20.0f);
        gainSmoother.prepare (spec.sampleRate, 20.0f);
        mixSmoother.snap (mix);
        gainSmoother.snap (outGain * juce::Decibels::decibelsToGain (makeupDb));

        reset();
    }

    void reset() noexcept
    {
        if (oversampler != nullptr) oversampler->reset();
        engine.reset();
        for (auto& d : dryDelay) d.reset();
        for (auto& d : dcOut) d.reset();
    }

    void setParams (const eta::EngineParams& p) noexcept
    {
        engine.setParams (p);
        // Make-up: il motore comprime (comp = 1/sqrt(g)), quindi a drive alto
        // l'RMS cala fino a -12 dB su un basso a -6 dBFS. 12 dB * drive^2
        // tiene il livello entro ~1 dB: accendere la valvola cambia il
        // timbro, non il volume.
        makeupDb = 12.0f * p.drive * p.drive;
        gainSmoother.setTarget (outGain * juce::Decibels::decibelsToGain (makeupDb));
    }
    void setMix (float m) noexcept    { mix = std::clamp (m, 0.0f, 1.0f); mixSmoother.setTarget (mix); }
    void setOutputDb (float db) noexcept
    {
        outGain = juce::Decibels::decibelsToGain (std::clamp (db, -24.0f, 12.0f));
        gainSmoother.setTarget (outGain * juce::Decibels::decibelsToGain (makeupDb));
    }

    int getLatencySamples() const noexcept { return latencySamples; }
    float getRail() const noexcept { return engine.getRail(); }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        if (oversampler == nullptr)
            return;

        const int n = (int) block.getNumSamples();
        for (int start = 0; start < n; start += maxBlock)
        {
            auto sub = block.getSubBlock ((size_t) start, (size_t) std::min (maxBlock, n - start));
            processChunk (sub);
        }
    }

private:
    void processChunk (juce::dsp::AudioBlock<float>& block) noexcept
    {
        const int n = (int) block.getNumSamples();
        const int numCh = juce::jmin (2, (int) block.getNumChannels());

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* src = block.getChannelPointer ((size_t) ch);
            auto* dst = dryBuffer.getWritePointer (ch);
            auto& line = dryDelay[(size_t) ch];
            for (int i = 0; i < n; ++i)
            {
                line.write (src[i]);
                dst[i] = line.read ((float) latencySamples + 1.0f);
            }
        }

        // Spento e a regime: solo il ramo asciutto ritardato, CPU quasi zero.
        if (mix <= 0.0001f && mixSmoother.getCurrent() <= 0.0001f)
        {
            for (int ch = 0; ch < numCh; ++ch)
                juce::FloatVectorOperations::copy (block.getChannelPointer ((size_t) ch),
                                                   dryBuffer.getReadPointer (ch), n);
            return;
        }

        auto up = oversampler->processSamplesUp (block);
        float* ptrs[2] = { up.getChannelPointer (0),
                           numCh > 1 ? up.getChannelPointer (1) : nullptr };
        engine.process (ptrs, numCh, (int) up.getNumSamples());
        oversampler->processSamplesDown (block);

        auto* wl = block.getChannelPointer (0);
        auto* wr = numCh > 1 ? block.getChannelPointer (1) : nullptr;
        const auto* dl = dryBuffer.getReadPointer (0);
        const auto* dr = numCh > 1 ? dryBuffer.getReadPointer (1) : nullptr;

        for (int i = 0; i < n; ++i)
        {
            const float m = mixSmoother.next();
            const float g = gainSmoother.next() * m;
            wl[i] = dl[i] * (1.0f - m) + dcOut[0].process (wl[i]) * g;
            if (wr != nullptr)
                wr[i] = dr[i] * (1.0f - m) + dcOut[1].process (wr[i]) * g;
        }
    }

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    eta::TubeEngine engine;
    std::array<DelayLine, 2> dryDelay;
    std::array<DCBlocker, 2> dcOut;
    juce::AudioBuffer<float> dryBuffer;
    ParamSmoother mixSmoother, gainSmoother;

    int   maxBlock = 512, latencySamples = 0;
    float mix = 0.0f, outGain = 1.0f, makeupDb = 3.0f;
};

} // namespace bassx
