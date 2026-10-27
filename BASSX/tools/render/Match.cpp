// Strumento di calibrazione: suona un pattern (JSON) su un preset di BASSX e
// scrive il risultato in WAV, per confrontarlo con registrazioni di hardware.
// Uso: BassXMatch <preset> <pattern.json> <out.wav> [paramID=valore ...]
//   pattern.json: { "notes": [ [t_sec, dur_sec, midiNote, velocity], ... ], "length": sec }
//   velocity >= soglia di accento => nota accentata; note sovrapposte => slide.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "PluginProcessor.h"

int main (int argc, char** argv)
{
    if (argc < 4) { std::printf ("uso: BassXMatch <preset> <pattern.json> <out.wav> [id=val ...]\n"); return 1; }
    juce::ScopedJuceInitialiser_GUI init;

    BassXAudioProcessor proc;
    const double sr = 44100.0;
    const int block = 64;
    proc.setPlayHead (nullptr);
    proc.prepareToPlay (sr, block);
    {
        int found = -1;
        for (int i = 0; i < proc.presetManager.getNumPresets(); ++i)
            if (proc.presetManager.getPresetName (i) == juce::String (argv[1])) found = i;
        if (found < 0) { std::printf ("preset '%s' non trovato\n", argv[1]); return 1; }
        proc.presetManager.loadPreset (found);
    }

    for (int i = 4; i < argc; ++i)   // override di parametri in unità native
    {
        const juce::String kv (argv[i]);
        const auto id = kv.upToFirstOccurrenceOf ("=", false, false);
        const float v = kv.fromFirstOccurrenceOf ("=", false, false).getFloatValue();
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (id)))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
        else { std::printf ("parametro '%s' sconosciuto\n", id.toRawUTF8()); return 1; }
    }
    proc.prepareToPlay (sr, block);

    const auto json = juce::JSON::parse (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    struct Ev { long long t; int note, vel; bool on; };
    std::vector<Ev> evs;
    for (auto& n : *json["notes"].getArray())
    {
        const auto& a = *n.getArray();
        const long long t0 = (long long) std::llround ((double) a[0] * sr);
        const long long t1 = (long long) std::llround (((double) a[0] + (double) a[1]) * sr);
        evs.push_back ({ t0, (int) a[2], (int) a[3], true });
        evs.push_back ({ t1, (int) a[2], 0, false });
    }
    // a parità di istante: note-on prima del note-off, così le note legate scivolano
    std::stable_sort (evs.begin(), evs.end(), [] (const Ev& a, const Ev& b) { return a.t < b.t || (a.t == b.t && a.on && ! b.on); });

    const long long total = (long long) ((double) json["length"] * sr);
    juce::AudioBuffer<float> out (1, (int) total), buf (2, block);
    size_t e = 0;
    for (long long pos = 0; pos < total; pos += block)
    {
        const int n = (int) std::min<long long> (block, total - pos);
        juce::MidiBuffer midi;
        while (e < evs.size() && evs[e].t < pos + n)
        {
            const int off = (int) std::max<long long> (0, evs[e].t - pos);
            midi.addEvent (evs[e].on ? juce::MidiMessage::noteOn (1, evs[e].note, (juce::uint8) evs[e].vel)
                                     : juce::MidiMessage::noteOff (1, evs[e].note), off);
            ++e;
        }
        buf.setSize (2, n, false, false, true);
        buf.clear();
        proc.processBlock (buf, midi);
        for (int i = 0; i < n; ++i)
            out.setSample (0, (int) (pos + i), 0.5f * (buf.getSample (0, i) + buf.getSample (1, i)));
    }

    juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]);
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), sr, 1, 24, {}, 0));
    w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
    std::printf ("scritto %s\n", f.getFullPathName().toRawUTF8());
    return 0;
}
