// Verifica offline del motore: suona una sequenza su ogni preset di fabbrica e
// controlla che l'uscita sia sensata (non silenziosa, priva di NaN/inf, entro
// il tetto del limiter). Non fa parte del plugin.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "PluginProcessor.h"
#include "DSP/NeuralDrive.h"
#include "GUI/Pages.h"
#include <fstream>
#include <map>

//==============================================================================
// Contatore di allocazioni: verifica che processBlock non allochi.
static std::atomic<bool> gTrackAllocations { false };
static std::atomic<long> gAllocations { 0 };

void* operator new (std::size_t n)
{
    if (gTrackAllocations.load (std::memory_order_relaxed))
        gAllocations.fetch_add (1, std::memory_order_relaxed);

    if (void* p = std::malloc (n == 0 ? 1 : n))
        return p;

    throw std::bad_alloc();
}

void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void* operator new[] (std::size_t n) { return operator new (n); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

//==============================================================================
struct FakePlayHead : public juce::AudioPlayHead
{
    double bpm = 120.0, ppq = 0.0;
    bool playing = true;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setPpqPosition (ppq);
        info.setIsPlaying (playing);
        return info;
    }
};

static int gChecks = 0, gCheckFailures = 0;

static void check (bool ok, const juce::String& what)
{
    ++gChecks;

    if (! ok)
    {
        ++gCheckFailures;
        std::printf ("FAIL  %s\n", what.toRawUTF8());
    }
    else
    {
        std::printf ("ok    %s\n", what.toRawUTF8());
    }
}

//==============================================================================
/** Fa girare il motore su più battute e raccoglie gli eventi con tempo assoluto. */
struct EngineRun
{
    struct Ev { long long t; bassx::SeqEvent e; };
    std::vector<Ev> events;
};

static EngineRun runEngine (bassx::Sequencer& seq, const bassx::PlayEngine::Settings& s,
                            double sr, double bpm, int blockSize, double bars,
                            std::vector<int> keys = {}, bool playing = true)
{
    bassx::PlayEngine engine;
    engine.prepare (sr);

    for (int k : keys)
        engine.keyDown (k, 100, s.arpLatch);

    EngineRun run;
    juce::Array<bassx::SeqEvent> out;
    out.ensureStorageAllocated (1024);

    const double samplesPerBeat = 60.0 / bpm * sr;
    const long long total = (long long) (bars * 4.0 * samplesPerBeat);
    long long t = 0;
    int b = 0;

    while (t < total)
    {
        // Blocchi di dimensione variabile, come certi host.
        const int n = (blockSize > 0) ? blockSize : 32 + (b * 97) % 900;
        out.clearQuick();
        engine.processBlock (seq, s, bpm, (double) t / samplesPerBeat, playing, n, out);

        for (auto& e : out)
            if (t + e.samplePosition < total)   // l'ultimo blocco sconfina oltre le battute richieste
                run.events.push_back ({ t + e.samplePosition, e });

        t += n;
        ++b;
    }

    return run;
}

static void testSequencer()
{
    std::printf ("\n--- Sequencer / arpeggiatore ---\n");

    const double sr = 48000.0, bpm = 128.0;
    const double stepSamples = 60.0 / bpm * sr * 0.25;   // 1/16

    for (float swing : { 0.0f, 0.5f })
    for (int block : { 64, 333, 512, 2048, 0 })
    {
        bassx::Sequencer seq;
        seq.setPattern (0);

        bassx::PlayEngine::Settings s;
        s.seqOn = true;
        s.seqSwing = swing;
        s.seqGate = 0.5f;

        auto run = runEngine (seq, s, sr, bpm, block, 4.0);

        // Note attese: passi con gate, esclusi quelli in cui il passo
        // precedente tiene il gate per uno slide (note-on legato comunque emesso).
        int expected = 0;
        for (long long k = 0; k < 64; ++k)
            if (seq.stepFires (k) && ! seq.stepAt (k).tie)
                ++expected;

        int ons = 0, timingErrors = 0, balance = 0, legatoErrors = 0;
        std::map<int, int> soundingCount;

        for (auto& ev : run.events)
        {
            if (ev.e.type == bassx::SeqEvent::Type::NoteOn)
            {
                ++ons;
                const double kExact = (double) ev.t / stepSamples;
                const long long k = (long long) std::llround (kExact - ((((long long) std::floor (kExact)) & 1) ? swing * 0.5 : 0.0));
                const double expectedT = ((double) k + ((k & 1) ? swing * 0.5 : 0.0)) * stepSamples;

                if (std::abs ((double) ev.t - expectedT) > 0.51)
                    ++timingErrors;

                // Un note-on su un passo NON slide deve trovare il voice
                // libero: se qualche nota suona ancora, il mono la
                // interpreterebbe come legato.
                const auto& st = seq.stepAt (k);
                int sounding = 0;
                for (auto& kv : soundingCount) sounding += kv.second;
                if (! st.slide && sounding > 0)
                    ++legatoErrors;

                ++soundingCount[ev.e.midiNote];
                ++balance;
            }
            else
            {
                if (soundingCount[ev.e.midiNote] > 0) --soundingCount[ev.e.midiNote];
                --balance;
            }
        }

        if (ons != expected)
            for (auto& ev : run.events)
                if (ev.e.type == bassx::SeqEvent::Type::NoteOn)
                    std::printf ("   on t=%lld step=%.3f note=%d slide=%d\n", ev.t, (double) ev.t / stepSamples, ev.e.midiNote, (int) ev.e.slide);

        const juce::String tag = "seq swing=" + juce::String (swing, 2) + " block=" + (block > 0 ? juce::String (block) : juce::String ("var"));
        check (ons == expected, tag + ": note " + juce::String (ons) + "/" + juce::String (expected));
        check (timingErrors == 0, tag + ": timing al campione (" + juce::String (timingErrors) + " errori)");
        check (legatoErrors == 0, tag + ": nessun legato involontario (" + juce::String (legatoErrors) + ")");
        check (balance >= 0 && balance <= 1, tag + ": note-on/off bilanciati (" + juce::String (balance) + ")");
    }

    // --- Slide: nessun note-off fra il passo e lo slide successivo ----------------
    {
        bassx::Sequencer seq;
        seq.setPattern (0);
        bassx::PlayEngine::Settings s;
        s.seqOn = true;
        s.seqGate = 0.3f;
        auto run = runEngine (seq, s, sr, bpm, 256, 1.0);

        int slideOk = 0, slideTotal = 0;
        for (size_t i = 0; i < run.events.size(); ++i)
        {
            const auto& ev = run.events[i];
            if (ev.e.type == bassx::SeqEvent::Type::NoteOn && ev.e.slide)
            {
                ++slideTotal;
                // l'evento precedente non deve essere un note-off: il gate è rimasto aperto
                bool prevOff = false;
                for (size_t j = i; j-- > 0;)
                {
                    if (run.events[j].t < ev.t) { prevOff = run.events[j].e.type == bassx::SeqEvent::Type::NoteOff; break; }
                }
                if (! prevOff) ++slideOk;
            }
        }
        check (slideTotal > 0 && slideOk == slideTotal, "slide: gate tenuto fino al passo legato (" + juce::String (slideOk) + "/" + juce::String (slideTotal) + ")");
    }

    // --- Arpeggiatore Up su Do-Mi-Sol --------------------------------------------
    {
        bassx::Sequencer seq;
        bassx::PlayEngine::Settings s;
        s.arpOn = true;
        s.arpMode = 0;
        s.arpDivision = 13;
        auto run = runEngine (seq, s, sr, bpm, 512, 1.0, { 64, 60, 67 });

        std::vector<int> notes;
        for (auto& ev : run.events)
            if (ev.e.type == bassx::SeqEvent::Type::NoteOn)
                notes.push_back (ev.e.midiNote);

        const int cycle[] = { 60, 64, 67 };
        bool pattern = notes.size() == 16;
        for (size_t i = 0; i < notes.size(); ++i)
            pattern = pattern && notes[i] == cycle[i % 3];

        check (pattern, "arp Up: 16 note in una battuta, ciclo 60-64-67 (" + juce::String ((int) notes.size()) + ")");
    }

    // --- Arp 2 ottave Up/Down -----------------------------------------------------
    {
        bassx::Sequencer seq;
        bassx::PlayEngine::Settings s;
        s.arpOn = true; s.arpMode = 2; s.arpOctaves = 2;
        auto run = runEngine (seq, s, sr, bpm, 512, 1.0, { 60, 64 });
        std::vector<int> notes;
        for (auto& ev : run.events)
            if (ev.e.type == bassx::SeqEvent::Type::NoteOn) notes.push_back (ev.e.midiNote);
        const int expect[] = { 60, 64, 72, 76, 72, 64 };
        bool ok = notes.size() == 16;
        for (size_t i = 0; i < notes.size(); ++i) ok = ok && notes[i] == expect[i % 6];
        check (ok, "arp Up/Down 2 ottave: 60-64-72-76-72-64");
    }

    // --- Combinazioni ---------------------------------------------------------------
    for (int combine = 0; combine < 3; ++combine)
    {
        bassx::Sequencer seq;
        seq.setPattern (1);
        bassx::PlayEngine::Settings s;
        s.seqOn = s.arpOn = true;
        s.combine = combine;
        auto run = runEngine (seq, s, sr, bpm, 0, 2.0, { 57, 60, 64 });

        int ons = 0, offs = 0;
        for (auto& ev : run.events)
            (ev.e.type == bassx::SeqEvent::Type::NoteOn ? ons : offs)++;

        check (ons > 8 && offs >= ons - 3, "combinazione " + juce::String (combine) + ": " + juce::String (ons) + " note");
    }

    // --- Trasporto fermo: suona solo con i tasti -------------------------------------
    {
        bassx::Sequencer seq;
        bassx::PlayEngine::Settings s;
        s.seqOn = true;
        auto silent = runEngine (seq, s, sr, bpm, 512, 1.0, {}, false);
        auto keyed  = runEngine (seq, s, sr, bpm, 512, 1.0, { 45 }, false);
        check (silent.events.empty() && ! keyed.events.empty(), "trasporto fermo: muto senza tasti, suona col tasto");
    }

    // --- Ratchet --------------------------------------------------------------------
    {
        bassx::Sequencer seq;
        seq.setPattern (0);
        for (int i = 0; i < 16; ++i) { auto& st = seq.getStep (0, i); st.gate = (i == 0); st.ratchet = 3; st.slide = st.tie = false; st.probability = 1.0f; }
        bassx::PlayEngine::Settings s;
        s.seqOn = true;
        auto run = runEngine (seq, s, sr, bpm, 100, 1.0);
        int ons = 0;
        for (auto& ev : run.events) if (ev.e.type == bassx::SeqEvent::Type::NoteOn) ++ons;
        check (ons == 3, "ratchet x3: 3 note sul passo (" + juce::String (ons) + ")");
    }
}

//==============================================================================
static void testNeuralInference (const char* referencePath)
{
    std::printf ("\n--- Neural Drive ---\n");

    std::ifstream in (referencePath);
    if (! in)
    {
        std::printf ("skip  riferimento non trovato: %s\n", referencePath);
        return;
    }

    bassx::NeuralDrive nd;
    double err = 0.0, sig = 0.0;
    float x, y;
    int n = 0;

    while (in >> x >> y)
    {
        const float out = nd.process (x, 0.7f);
        err += (double) (out - y) * (out - y);
        sig += (double) y * y;
        ++n;
    }

    const double esr = err / juce::jmax (1.0e-12, sig);
    check (n > 1000 && esr < 1.0e-4, "inferenza C++ = PyTorch (ESR " + juce::String (esr, 8) + ", " + juce::String (n) + " campioni)");
}

//==============================================================================
static void testDistortionLatency()
{
    std::printf ("\n--- Latenza distorsione ---\n");

    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
    bassx::Distortion d;
    d.prepare (spec);
    d.setDrive (0.0f);

    auto impulsePeak = [&] (float mix)
    {
        d.reset();
        d.setMix (mix);
        juce::AudioBuffer<float> b (2, 512);
        b.clear();
        b.setSample (0, 10, 0.05f);
        b.setSample (1, 10, 0.05f);
        juce::dsp::AudioBlock<float> block (b);
        d.process (block);
        int idx = 0; float best = 0.0f;
        for (int i = 0; i < 512; ++i)
            if (std::abs (b.getSample (0, i)) > best) { best = std::abs (b.getSample (0, i)); idx = i; }
        return idx;
    };

    const int lat = d.getLatencySamples();
    const int off = impulsePeak (0.0f);
    const int on  = impulsePeak (1.0f);
    check (off == 10 + lat && std::abs (on - off) <= 1,
           "distorsione: stessa latenza accesa/spenta (" + juce::String (off - 10) + "/" + juce::String (on - 10) + ", dichiarata " + juce::String (lat) + ")");
}

static void testTube()
{
    std::printf ("\n--- Valvola (ETA-1) ---\n");

    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
    bassx::TubeStage t;
    t.prepare (spec);
    const int lat = t.getLatencySamples();

    // Parametri a regime prima di misurare: i mix sono smussati (20 ms).
    auto settle = [&] (float mix, float drive)
    {
        eta::EngineParams p; p.profile = 6; p.drive = drive;
        t.setParams (p); t.setMix (mix); t.reset();
        juce::AudioBuffer<float> z (2, 8192); z.clear();
        juce::dsp::AudioBlock<float> zb (z);
        t.process (zb);
    };

    auto impulsePeak = [&] (float mix)
    {
        settle (mix, 0.0f);
        juce::AudioBuffer<float> b (2, 512);
        b.clear();
        b.setSample (0, 10, 0.05f); b.setSample (1, 10, 0.05f);
        juce::dsp::AudioBlock<float> block (b);
        t.process (block);
        int idx = 0; float best = 0.0f;
        for (int i = 0; i < 512; ++i)
            if (std::abs (b.getSample (0, i)) > best) { best = std::abs (b.getSample (0, i)); idx = i; }
        return idx;
    };

    const int off = impulsePeak (0.0f), on = impulsePeak (1.0f);
    check (off == 10 + lat && std::abs (on - off) <= 2,
           "valvola: stessa latenza accesa/spenta (" + juce::String (off - 10) + "/" + juce::String (on - 10) + ", dichiarata " + juce::String (lat) + ")");

    // Drive alto su sinusoide 55 Hz: armoniche presenti, uscita finita e limitata.
    // Blocco da 2048 su spec 512: verifica anche lo spezzettamento interno.
    settle (1.0f, 0.85f);
    juce::AudioBuffer<float> b (2, 2048);
    for (int i = 0; i < 2048; ++i)
        for (int c = 0; c < 2; ++c)
            b.setSample (c, i, 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 55.0f * (float) i / 48000.0f));
    juce::AudioBuffer<float> ref (b);
    juce::dsp::AudioBlock<float> block (b);
    t.process (block);

    bool finite = true; float peak = 0.0f; double diff = 0.0, energy = 0.0;
    for (int i = lat; i < 2048; ++i)
    {
        const float y = b.getSample (0, i), x = ref.getSample (0, i - lat);
        finite = finite && std::isfinite (y);
        peak = std::max (peak, std::abs (y));
        diff += (double) (y - x) * (y - x); energy += (double) x * x;
    }
    check (finite && peak < 2.5f, "valvola: uscita finita e limitata (picco " + juce::String (peak, 2) + ")");
    const double distDb = 10.0 * std::log10 (diff / energy + 1e-12);

    for (float dr : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        settle (1.0f, dr);
        juce::AudioBuffer<float> g (ref);
        juce::dsp::AudioBlock<float> gb (g);
        t.process (gb);
        double eo = 0.0, ei = 0.0;
        for (int i = 1024; i < 2048; ++i) { eo += (double) g.getSample (0, i) * g.getSample (0, i); ei += (double) ref.getSample (0, i) * ref.getSample (0, i); }
        std::printf ("info  valvola drive %.2f: guadagno %.1f dB\n", dr, 10.0 * std::log10 (eo / ei));
    }
    check (distDb > -30.0, "valvola: drive 85% colora il segnale (residuo " + juce::String (distDb, 1) + " dB)");
}

static void testSpace()
{
    std::printf ("\n--- Space ---\n");

    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
    bassx::Space sp;
    sp.prepare (spec);

    auto run = [&] (std::function<void()> setup, float freq, float sideAmount)
    {
        sp.reset();
        setup();
        sp.reset();
        juce::AudioBuffer<float> b (2, 512);
        double sumSide = 0.0, sumIn = 0.0;
        for (int blk = 0; blk < 40; ++blk)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float t = (float) (blk * 512 + i) / 48000.0f;
                const float m = std::sin (bassx::kTwoPi * freq * t) * 0.3f;
                const float sd = std::sin (bassx::kTwoPi * freq * t + 1.0f) * 0.3f * sideAmount;
                b.setSample (0, i, m + sd);
                b.setSample (1, i, m - sd);
                if (blk >= 20) sumIn += (double) sd * sd;
            }
            juce::dsp::AudioBlock<float> block (b);
            sp.process (block);
            if (blk >= 20)
                for (int i = 0; i < 512; ++i)
                {
                    const float s = 0.5f * (b.getSample (0, i) - b.getSample (1, i));
                    sumSide += (double) s * s;
                }
        }
        return std::sqrt (sumSide / juce::jmax (1.0e-12, sumIn));
    };

    const double mono = run ([&] { sp.setWidth (0.0f); sp.setMonoFrequency (20.0f); sp.setPan (0.0f); }, 300.0f, 1.0f);
    check (mono < 1.0e-4, "width 0 = mono (lato residuo " + juce::String (mono, 6) + ")");

    const double bassLow = run ([&] { sp.setWidth (1.0f); sp.setMonoFrequency (150.0f); }, 40.0f, 1.0f);
    const double bassHigh = run ([&] { sp.setWidth (1.0f); sp.setMonoFrequency (150.0f); }, 2000.0f, 1.0f);
    check (bassLow < 0.1 && bassHigh > 0.95,
           "bass mono 150 Hz: lato a 40 Hz " + juce::String (bassLow, 3) + ", a 2 kHz " + juce::String (bassHigh, 3));
}

//==============================================================================
//==============================================================================
// Regressioni v1.3.1: stop della DAW, code degli effetti, preset, zipper.
//==============================================================================
static void setParam (BassXAudioProcessor& proc, const juce::String& id, float value)
{
    if (auto* p = proc.apvts.getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

static float getParam (BassXAudioProcessor& proc, const juce::String& id)
{
    return proc.apvts.getRawParameterValue (id)->load();
}

/** Il motore si ferma con la DAW anche con LATCH: prima ripartiva da solo
    sull'orologio interno e la sequenza girava all'infinito dopo lo stop. */
static void testStopWithLatch()
{
    const double sr = 48000.0, bpm = 120.0, spb = 60.0 / bpm * sr;
    bassx::Sequencer seq;
    bassx::PlayEngine::Settings s;
    s.arpOn = true; s.arpLatch = true;

    bassx::PlayEngine engine;
    engine.prepare (sr);
    juce::Array<bassx::SeqEvent> out;
    out.ensureStorageAllocated (1024);

    long long t = 0;
    int onsWhilePlaying = 0, onsAfterStop = 0;
    const int n = 512;

    engine.keyDown (60, 100, true);
    engine.keyDown (64, 100, true);
    engine.keyUp (60, true);
    engine.keyUp (64, true);               // latch: l'accordo resta

    for (int b = 0; b < 400; ++b, t += n)  // ~4.3 s: 2 s in play, poi stop
    {
        const bool playing = (double) t < 2.0 * sr;
        out.clearQuick();
        engine.processBlock (seq, s, bpm, (double) t / spb, playing, n, out);
        for (auto& e : out)
            if (e.type == bassx::SeqEvent::Type::NoteOn)
                (playing ? onsWhilePlaying : onsAfterStop) += 1;
    }

    check (onsWhilePlaying > 4, "latch: in play l'arp suona (" + juce::String (onsWhilePlaying) + " note)");
    check (onsAfterStop == 0, "stop della DAW con LATCH: il motore si ferma (" + juce::String (onsAfterStop) + " note dopo lo stop)");

    // a trasporto fermo un tasto nuovo lo fa ripartire
    engine.keyDown (67, 100, true);
    int onsKeyed = 0;
    for (int b = 0; b < 100; ++b, t += n)
    {
        out.clearQuick();
        engine.processBlock (seq, s, bpm, 0.0, false, n, out);
        for (auto& e : out) if (e.type == bassx::SeqEvent::Type::NoteOn) ++onsKeyed;
    }
    check (onsKeyed > 0, "trasporto fermo: un tasto nuovo fa ripartire il motore");
}

static double blockRms (const juce::AudioBuffer<float>& b)
{
    double r = 0.0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i) r += (double) b.getSample (ch, i) * b.getSample (ch, i);
    return std::sqrt (r / (double) juce::jmax (1, b.getNumChannels() * b.getNumSamples()));
}

/** Delay spento: niente loop nascosto che riaffiora alla riaccensione. */
static void testDelayNoStaleLoop()
{
    const double sr = 48000.0; const int bs = 256;
    BassXAudioProcessor proc;
    proc.setPlayHead (nullptr);
    proc.presetManager.loadPresetByName ("Init");
    setParam (proc, bassx::ParamID::delayOn, 1.0f);
    setParam (proc, bassx::ParamID::delaySync, 0.0f);
    setParam (proc, bassx::ParamID::delayTimeL, 0.3f);
    setParam (proc, bassx::ParamID::delayTimeR, 0.3f);
    setParam (proc, bassx::ParamID::delayFeedback, 1.05f);   // autooscillazione
    setParam (proc, bassx::ParamID::delayMix, 0.5f);
    proc.prepareToPlay (sr, bs);

    juce::AudioBuffer<float> buf (2, bs);
    juce::MidiBuffer midi;
    auto run = [&] (int blocks, int noteOnAt = -1, int noteOffAt = -1)
    {
        double peak = 0.0;
        for (int b = 0; b < blocks; ++b)
        {
            midi.clear();
            if (b == noteOnAt)  midi.addEvent (juce::MidiMessage::noteOn (1, 45, (juce::uint8) 110), 0);
            if (b == noteOffAt) midi.addEvent (juce::MidiMessage::noteOff (1, 45), 0);
            buf.clear();
            proc.processBlock (buf, midi);
            peak = juce::jmax (peak, (double) buf.getMagnitude (0, bs));
        }
        return peak;
    };

    run (200, 0, 40);                                    // nota + eco che si autoalimenta
    setParam (proc, bassx::ParamID::delayOn, 0.0f);
    run (400);                                            // ~2 s spento, nessuna nota
    setParam (proc, bassx::ParamID::delayOn, 1.0f);
    const double back = run (200);                        // riacceso, ancora nessuna nota
    check (back < 1.0e-3, "delay riacceso senza note: niente loop vecchio che riaffiora (picco " + juce::String (back, 5) + ")");
}

/** Preset: tema, impostazioni di sessione, indici, stato della sessione. */
static void testPresets()
{
    BassXAudioProcessor proc;
    auto& pm = proc.presetManager;

    proc.apvts.state.setProperty ("uiTheme", 3, nullptr);
    setParam (proc, bassx::ParamID::oversampling, 2.0f);
    setParam (proc, bassx::ParamID::masterTune, 25.0f);

    pm.loadPreset (2);
    check ((int) proc.apvts.state.getProperty ("uiTheme", 0) == 3, "cambio preset: il tema della GUI resta");
    check ((int) getParam (proc, bassx::ParamID::oversampling) == 2
           && std::abs (getParam (proc, bassx::ParamID::masterTune) - 25.0f) < 0.01f,
           "cambio preset: oversampling e accordatura restano");

    // preset utente con lo stesso nome di uno di fabbrica
    const auto factoryName = pm.getPresetName (1);
    pm.loadPreset (1);
    setParam (proc, bassx::ParamID::f1Cutoff, 1234.0f);
    proc.apvts.state.setProperty ("uiTheme", 5, nullptr);
    const bool saved = pm.saveUserPreset (factoryName);
    const int userIndex = pm.getCurrentPresetIndex();
    check (saved && userIndex >= 0 && userIndex != 1, "preset utente omonimo: indice proprio (" + juce::String (userIndex) + ")");

    proc.apvts.state.setProperty ("uiTheme", 1, nullptr);
    pm.loadPreset (0);
    const auto userIdx = pm.getPresetIndices ("User");
    check (userIdx.contains (userIndex), "menu User: indice del preset utente");
    pm.loadPreset (userIndex);
    check (std::abs (getParam (proc, bassx::ParamID::f1Cutoff) - 1234.0f) < 1.0f,
           "preset utente omonimo: si carica lui, non quello di fabbrica");
    check ((int) proc.apvts.state.getProperty ("uiTheme", 0) == 1, "preset utente: non cambia il tema");
    pm.deleteUserPreset (factoryName);

    // sessione: modifica dopo il preset, salva, riapri; l'host richiama
    // setCurrentProgram dopo lo stato (Ableton/Reaper): la modifica resta.
    pm.loadPreset (3);
    const auto presetName = pm.getCurrentPresetName();
    setParam (proc, bassx::ParamID::f1Cutoff, 777.0f);
    juce::MemoryBlock state;
    proc.getStateInformation (state);

    BassXAudioProcessor reopened;
    reopened.setStateInformation (state.getData(), (int) state.getSize());
    reopened.setCurrentProgram (0);
    reopened.setCurrentProgram (reopened.getCurrentProgram());
    check (std::abs (getParam (reopened, bassx::ParamID::f1Cutoff) - 777.0f) < 1.0f,
           "sessione riaperta + setCurrentProgram dell'host: le modifiche restano");
    check (reopened.presetManager.getCurrentPresetName() == presetName,
           "sessione riaperta: il nome del preset e' quello giusto (" + reopened.presetManager.getCurrentPresetName() + ")");
}

/** Automazione a gradino 0 <-> 1: niente click. Energia della derivata seconda
    nei 4 ms dopo il salto, contro la stessa a manopola ferma. */
static void testZipper()
{
    const double sr = 48000.0; const int bs = 256;

    struct Case { const char* id; float lo, hi; const char* enable; };
    const Case cases[] = {
        { bassx::ParamID::distDrive,  0.0f, 1.0f, bassx::ParamID::distOn },
        { bassx::ParamID::distMix,    0.0f, 1.0f, bassx::ParamID::distOn },
        { bassx::ParamID::delayMix,   0.0f, 0.8f, bassx::ParamID::delayOn },
        { bassx::ParamID::reverbMix,  0.0f, 0.8f, bassx::ParamID::reverbOn },
        { bassx::ParamID::chorusMix,  0.0f, 1.0f, bassx::ParamID::chorusOn },
        { bassx::ParamID::compMix,    0.0f, 1.0f, bassx::ParamID::compOn },
        { bassx::ParamID::f1Reso,     0.0f, 0.95f, nullptr },
    };

    for (const auto& c : cases)
    {
        auto render = [&] (bool jump, float fixed)
        {
            BassXAudioProcessor proc;
            proc.setPlayHead (nullptr);
            proc.presetManager.loadPresetByName ("Init");
            if (c.enable != nullptr) setParam (proc, c.enable, 1.0f);
            setParam (proc, bassx::ParamID::limiterOn, 0.0f);
            setParam (proc, c.id, jump ? c.lo : fixed);
            proc.prepareToPlay (sr, bs);

            juce::AudioBuffer<float> out (2, bs * 160), buf (2, bs);
            for (int b = 0; b < 160; ++b)
            {
                juce::MidiBuffer midi;
                if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 40, (juce::uint8) 90), 0);
                if (jump && b >= 40 && b % 16 == 0) setParam (proc, c.id, ((b / 16) & 1) ? c.hi : c.lo);
                buf.clear();
                proc.processBlock (buf, midi);
                for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, b * bs, buf, ch, 0, bs);
            }
            return out;
        };

        const auto jumped = render (true, 0.0f), still0 = render (false, c.lo), still1 = render (false, c.hi);
        auto d2 = [&] (const juce::AudioBuffer<float>& a, int from)
        {
            double e = 0.0;
            const int win = (int) (0.004 * sr);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = from + 2; i < from + win; ++i)
                {
                    const double v = (double) a.getSample (ch, i) - 2.0 * a.getSample (ch, i - 1) + a.getSample (ch, i - 2);
                    e += v * v;
                }
            return e;
        };

        double atJump = 0.0, steady = 0.0;
        for (int b = 48; b < 160; b += 16)
        {
            atJump += d2 (jumped, b * bs);
            steady += juce::jmax (d2 (still0, b * bs), d2 (still1, b * bs));
        }
        const double db = 10.0 * std::log10 (juce::jmax (1.0e-30, atJump) / juce::jmax (1.0e-30, steady));
        check (db < 3.0, juce::String ("zipper ") + c.id + ": click al gradino " + juce::String (db, 1) + " dB sopra il fermo");
    }
}

/** Ogni modello di filtro deve tagliare dove dice il suo nome. Saw a 55 Hz,
    cutoff 1 kHz, inviluppo del filtro a zero: si confronta l'energia delle
    armoniche in tre bande con lo stesso suono in Bypass. */
static void testFilterModels()
{
    const double sr = 48000.0; const int bs = 256;
    const int nBlocks = (int) (1.0 * sr / bs);

    std::vector<double> lastHarm;
    auto bands = [&] (int model, float reso = 0.3f)
    {
        BassXAudioProcessor proc;
        proc.setPlayHead (nullptr);
        proc.presetManager.loadPresetByName ("Init");
        setParam (proc, bassx::ParamID::f1Model, (float) model);
        setParam (proc, bassx::ParamID::f2Model, (float) (int) bassx::MultiFilter::Model::Bypass);
        setParam (proc, bassx::ParamID::filterRouting, 0.0f);
        setParam (proc, bassx::ParamID::f1Cutoff, 1000.0f);
        setParam (proc, bassx::ParamID::f1Reso, reso);
        setParam (proc, bassx::ParamID::f1EnvAmount, 0.0f);
        setParam (proc, bassx::ParamID::f1KeyTrack, 0.0f);
        setParam (proc, bassx::ParamID::f1Drive, 1.0f);
        setParam (proc, bassx::ParamID::env (0, "sustain"), 1.0f);
        setParam (proc, bassx::ParamID::limiterOn, 0.0f);
        proc.prepareToPlay (sr, bs);

        std::vector<float> mono;
        juce::AudioBuffer<float> buf (2, bs);
        for (int b = 0; b < nBlocks; ++b)
        {
            juce::MidiBuffer midi;
            if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 33, (juce::uint8) 80), 0);
            buf.clear();
            proc.processBlock (buf, midi);
            for (int i = 0; i < bs; ++i) mono.push_back (0.5f * (buf.getSample (0, i) + buf.getSample (1, i)));
        }

        // DFT sulle armoniche di 55 Hz, finestra di Hann su 0.4 s a regime
        const size_t from = (size_t) (0.4 * sr), n = (size_t) (0.4 * sr);
        auto mag = [&] (double hz)
        {
            double re = 0, im = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (double) i / (double) (n - 1));
                const double ph = 2.0 * juce::MathConstants<double>::pi * hz * (double) i / sr;
                re += mono[from + i] * w * std::cos (ph);
                im -= mono[from + i] * w * std::sin (ph);
            }
            return re * re + im * im;
        };
        std::array<double, 4> e {};   // < 300 Hz, 700..1400 Hz, > 3 kHz, armonica a 990 Hz
        e[3] = mag (990.0);
        for (int k = 1; k * 55.0 < 9000.0; ++k)
        {
            const double f = 55.0 * k, m = mag (f);
            if (f < 300.0) e[0] += m; else if (f > 700.0 && f < 1400.0) e[1] += m; else if (f > 3000.0) e[2] += m;
        }
        lastHarm.assign (80, 0.0);
        for (int k = 1; k < 80 && k * 55.0 < 4000.0; ++k) lastHarm[(size_t) k] = mag (55.0 * k);
        return e;
    };

    using M = bassx::MultiFilter::Model;
    const auto ref = bands ((int) M::Bypass);
    const auto refHarm = lastHarm;
    auto rel = [&] (const std::array<double, 4>& e, int i) { return 10.0 * std::log10 (juce::jmax (1.0e-30, e[(size_t) i]) / juce::jmax (1.0e-30, ref[(size_t) i])); };

    const auto names = bassx::MultiFilter::getModelNames();
    for (int m = 0; m < (int) M::Bypass; ++m)
    {
        // il pettine si sente con il feedback (= risonanza) alto
        const auto e = bands (m, (M) m == M::Comb ? 0.85f : 0.3f);
        const auto harm = lastHarm;
        const double lo = rel (e, 0), mid = rel (e, 1), hi = rel (e, 2);
        bool ok = true;
        switch ((M) m)
        {
            case M::Diode303: case M::Diode303_24: case M::MoogLP24: case M::MoogLP12:
            case M::SvfLP12: case M::SvfLP24:
                ok = hi < lo - 12.0; break;                       // passa-basso
            case M::MoogHP: case M::SvfHP12: case M::SvfHP24:
                ok = lo < hi - 12.0; break;                       // passa-alto
            case M::MoogBP: case M::SvfBP12:
                ok = mid > lo + 6.0 && mid > hi + 6.0; break;     // passa-banda
            case M::SvfNotch:
                ok = rel (e, 3) < -12.0 && std::abs (lo) < 3.0 && std::abs (hi) < 3.0; break;  // buco stretto su 1 kHz
            case M::SvfPeak:
                ok = mid > lo + 3.0 && mid > hi + 3.0; break;     // campana su 1 kHz
            case M::Comb: case M::Formant: default:
            {
                // pettine / vocale: le singole armoniche si spostano di parecchio
                double dev = 0.0;
                for (int k = 1; k * 55.0 < 4000.0; ++k)
                    dev = juce::jmax (dev, std::abs (10.0 * std::log10 (juce::jmax (1.0e-30, harm[(size_t) k]) / juce::jmax (1.0e-30, refHarm[(size_t) k]))));
                ok = dev > 6.0;
                break;
            }
        }
        check (ok, "filtro " + names[m] + ": basse " + juce::String (lo, 1) + " dB, 1k " + juce::String (mid, 1)
                     + " dB, alte " + juce::String (hi, 1) + " dB (rispetto a Bypass)");
    }
}

/** Il grafico RESPONSE della pagina filtri segue modello e routing. */
static void testFilterGraph()
{
    using M = bassx::MultiFilter::Model;
    const std::vector<float> hz { 60.0f, 1000.0f, 8000.0f };
    std::vector<float> tot, a, b;
    auto shape = [&] (M m1, M m2, int routing)
    {
        bassx::FilterResponse::Settings s;
        s.m1 = (int) m1; s.m2 = (int) m2; s.routing = routing;
        s.c1 = 1000.0f; s.r1 = 0.3f; s.c2 = 1000.0f; s.r2 = 0.3f; s.d1 = 1.0f; s.d2 = 1.0f; s.blend = 0.5f;
        bassx::FilterResponse::measure (s, hz, tot, a, b);
        return tot;
    };
    auto lp = shape (M::SvfLP24, M::Bypass, 0), hp = shape (M::SvfHP24, M::Bypass, 0);
    auto mhp = shape (M::MoogHP, M::Bypass, 0), diode = shape (M::Diode303, M::Bypass, 0);
    check (lp[2] < lp[0] - 30.0f, "grafico: SVF LP24 taglia le alte (" + juce::String (lp[0], 1) + " / " + juce::String (lp[2], 1) + " dB)");
    check (hp[0] < hp[2] - 30.0f, "grafico: SVF HP24 taglia le basse (" + juce::String (hp[0], 1) + " / " + juce::String (hp[2], 1) + " dB)");
    check (mhp[0] < mhp[2] - 30.0f, "grafico: Moog HP taglia le basse (" + juce::String (mhp[0], 1) + " / " + juce::String (mhp[2], 1) + " dB)");
    check (diode[2] < diode[0] - 20.0f, "grafico: Diode taglia le alte");
    auto serial = shape (M::SvfHP24, M::SvfLP24, 0);
    check (serial[1] > serial[0] + 20.0f && serial[1] > serial[2] + 20.0f, "grafico: HP > LP in serie = passa-banda");
}

//==============================================================================
/** IMPATTO: ogni parametro, portato da minimo a massimo, deve cambiare
    l'audio. Contesto acceso apposta quando serve (LFO instradato, effetto ON,
    oscillatore attivo...). Scena: nota accentata, nota legata (glide),
    accordo con pitch bend e mod wheel, transport della DAW in play. */
static juce::AudioBuffer<float> renderScene (const std::map<juce::String, float>& norm,
                                             const std::vector<std::pair<bassx::ModSource, bassx::ModDest>>& links,
                                             bool feedExt = false)
{
    const double sr = 48000.0; const int bs = 256;
    BassXAudioProcessor proc;
    FakePlayHead head; head.bpm = 120.0; head.ppq = 0.0; head.playing = true;
    proc.setPlayHead (&head);
    proc.presetManager.loadPresetByName ("Init");

    if (feedExt)
    {
        auto layout = proc.getBusesLayout();
        layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        proc.setBusesLayout (layout);
    }

    for (const auto& [id, v] : norm)
        if (auto* p = proc.apvts.getParameter (id))
            p->setValueNotifyingHost (v);

    for (size_t i = 0; i < links.size(); ++i)
        proc.modMatrix.setSlot ((int) i, links[i].first, links[i].second, 0.8f);

    proc.prepareToPlay (sr, bs);

    const int blocks = (int) (2.5 * sr / bs);
    juce::AudioBuffer<float> out (2, blocks * bs), buf (2, bs);
    auto at = [&] (int b, double t) { return (int) (t * sr) >= b * bs && (int) (t * sr) < (b + 1) * bs; };

    for (int b = 0; b < blocks; ++b)
    {
        juce::MidiBuffer midi;
        using MM = juce::MidiMessage;
        if (at (b, 0.00)) midi.addEvent (MM::noteOn (1, 36, (juce::uint8) 120), 0);
        if (at (b, 0.30)) midi.addEvent (MM::noteOn (1, 43, (juce::uint8) 70), 0);
        if (at (b, 0.60)) midi.addEvent (MM::noteOff (1, 36), 0);
        if (at (b, 0.80)) midi.addEvent (MM::noteOff (1, 43), 0);
        if (at (b, 1.00)) { midi.addEvent (MM::noteOn (1, 48, (juce::uint8) 90), 0);
                            midi.addEvent (MM::noteOn (1, 52, (juce::uint8) 60), 0);
                            midi.addEvent (MM::noteOn (1, 55, (juce::uint8) 100), 0);
                            midi.addEvent (MM::controllerEvent (1, 1, 100), 0); }
        if (at (b, 1.20)) midi.addEvent (MM::pitchWheel (1, 12000), 0);
        if (at (b, 1.30)) midi.addEvent (MM::channelPressureChange (1, 90), 0);
        if (at (b, 1.60)) { midi.addEvent (MM::noteOff (1, 48), 0); midi.addEvent (MM::noteOff (1, 52), 0);
                            midi.addEvent (MM::noteOff (1, 55), 0); midi.addEvent (MM::pitchWheel (1, 8192), 0); }
        if (at (b, 1.80)) midi.addEvent (MM::noteOn (1, 31, (juce::uint8) 127), 0);
        if (at (b, 2.00)) midi.addEvent (MM::noteOff (1, 31), 0);

        buf.clear();
        if (feedExt)
            for (int i = 0; i < bs; ++i)
            {
                const float v = 0.3f * std::sin (bassx::kTwoPi * 110.0f * (float) (b * bs + i) / (float) sr);
                buf.setSample (0, i, v); buf.setSample (1, i, v);
            }
        proc.processBlock (buf, midi);
        head.ppq += bs / (60.0 / head.bpm * sr);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, b * bs, buf, ch, 0, bs);
    }
    proc.setPlayHead (nullptr);
    return out;
}

static double relativeDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    double d = 0.0, ea = 0.0, eb = 0.0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
        {
            const double x = a.getSample (ch, i), y = b.getSample (ch, i);
            d += (x - y) * (x - y); ea += x * x; eb += y * y;
        }
    return std::sqrt (d / juce::jmax (1.0e-30, juce::jmax (ea, eb)));
}

static void testEveryParameterImpact()
{
    using S = bassx::ModSource;
    using D = bassx::ModDest;
    namespace P = bassx::ParamID;

    BassXAudioProcessor ref;
    auto norm = [&] (const juce::String& id, float real)
    {
        auto* p = ref.apvts.getParameter (id);
        return p != nullptr ? p->convertTo0to1 (real) : 0.0f;
    };

    const auto base0 = renderScene ({}, {}), base1 = renderScene ({}, {});
    const double floorDiff = relativeDiff (base0, base1);
    check (floorDiff < 1.0e-6, "impatto: render deterministico (" + juce::String (floorDiff, 8) + ")");

    int muted = 0, tested = 0;
    juce::StringArray mutedList;

    for (auto* prm : ref.getParameters())
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm);
        if (rp == nullptr) continue;
        const juce::String id = rp->getParameterID();

        // l'ingresso esterno si prova con un segnale in ingresso
        const bool ext = id.startsWith ("df_ext") || id == P::scThreshold;

        std::map<juce::String, float> ctx;
        std::vector<std::pair<S, D>> links;

        auto on = [&] (const juce::String& p) { ctx[p] = 1.0f; };
        if (id.startsWith ("osc2_") || id.startsWith ("fm_") || id == P::ringAmount || id == P::syncEnable) { on (P::osc (1, "enable")); ctx[P::osc (1, "level")] = 1.0f; }
        if (id.startsWith ("osc3_")) { on (P::osc (2, "enable")); ctx[P::osc (2, "level")] = 1.0f; }
        if (id.startsWith ("fm_") && id != P::fmAmount) ctx[P::fmAmount] = norm (P::fmAmount, 0.7f);
        if (id.startsWith ("osc") && id.contains ("uni_") && ! id.contains ("voices")) ctx[P::osc (id.substring (3, 4).getIntValue() - 1, "uni_voices")] = 1.0f;
        if (id.startsWith ("sub_"))   { on (P::subEnable); ctx[P::subLevel] = norm (P::subLevel, 0.8f); }
        if (id.startsWith ("noise_")) { on (P::noiseEnable); ctx[P::noiseLevel] = norm (P::noiseLevel, 0.6f); }
        if (id.startsWith ("f2_") && id != P::f2Model) ctx[P::f2Model] = norm (P::f2Model, (float) (int) bassx::MultiFilter::Model::SvfLP12);
        if (id == P::filterBlend) { ctx[P::filterRouting] = norm (P::filterRouting, 1.0f); ctx[P::f2Model] = norm (P::f2Model, (float) (int) bassx::MultiFilter::Model::SvfHP12); }
        if (id == P::filterRouting) ctx[P::f2Model] = norm (P::f2Model, (float) (int) bassx::MultiFilter::Model::SvfHP12);
        if (id.startsWith ("lfo"))
        {
            const int l = id.substring (3, 4).getIntValue() - 1;
            links.push_back ({ (S) ((int) S::LFO1 + l), D::Filter1Cutoff });
            if (id.contains ("fade") || id.contains ("trigger")) ctx[P::lfo (l, "rate")] = norm (P::lfo (l, "rate"), 3.0f);
            if (id.contains ("division")) on (P::lfo (l, "sync"));
        }
        if (id.startsWith ("env3") || id.startsWith ("env4"))
            links.push_back ({ id.startsWith ("env3") ? S::Env3 : S::Env4, D::Filter1Cutoff });
        if (id.startsWith ("seq_") && id != P::seqEnable) on (P::seqEnable);
        if (id.startsWith ("arp_") && id != P::arpEnable) on (P::arpEnable);
        if (id == P::playCombine) { on (P::seqEnable); on (P::arpEnable); }
        if (id == P::polyVoices) ctx[P::voiceMode] = norm (P::voiceMode, 2.0f);
        if (id == P::dfVibSpeed) ctx[P::dfVibDepth] = norm (P::dfVibDepth, 0.8f);
        if (id.startsWith ("dist_") && id != P::distOn)     on (P::distOn);
        if (id.startsWith ("dist_bits") || id.startsWith ("dist_downsample")) ctx[P::distAlgo] = norm (P::distAlgo, 5.0f);
        if (id.startsWith ("tube_") && id != P::tubeOn)     on (P::tubeOn);
        if (id.startsWith ("chorus_") && id != P::chorusOn) on (P::chorusOn);
        if (id.startsWith ("phaser_") && id != P::phaserOn) on (P::phaserOn);
        if (id.startsWith ("delay_") && id != P::delayOn)   { on (P::delayOn); ctx[P::delayMix] = norm (P::delayMix, 0.5f); }
        if (id.startsWith ("delay_div")) ctx[P::delaySync] = 1.0f;
        if (id.startsWith ("delay_time")) ctx[P::delaySync] = 0.0f;
        if (id.startsWith ("reverb_") && id != P::reverbOn) { on (P::reverbOn); ctx[P::reverbMix] = norm (P::reverbMix, 0.5f); }
        if (id.startsWith ("comp_") && id != P::compOn)     { on (P::compOn); ctx[P::compThreshold] = norm (P::compThreshold, -30.0f); }
        if (id.startsWith ("space_") && id != P::spaceOn)   on (P::spaceOn);
        if (id == P::spaceAutopanDiv) ctx[P::spaceAutopan] = norm (P::spaceAutopan, 0.8f);
        if (id == P::eqLowFreq)  ctx[P::eqLowGain]  = norm (P::eqLowGain, 12.0f);
        if (id == P::eqMidFreq || id == P::eqMidQ) ctx[P::eqMidGain] = norm (P::eqMidGain, 12.0f);
        if (id == P::eqHighFreq) ctx[P::eqHighGain] = norm (P::eqHighGain, 12.0f);
        if (id.startsWith ("limiter_")) ctx[P::outputGain] = norm (P::outputGain, 12.0f);
        if (id.startsWith ("fx_slot")) { on (P::distOn); on (P::reverbOn); ctx[P::reverbMix] = norm (P::reverbMix, 0.5f); }
        if (id == P::dfExtGain) on (P::dfExtMode);
        if (id.startsWith ("sc_") && id != P::scOn) on (P::scOn);
        if (id.startsWith ("glitch_") && id != P::glitchOn) { on (P::glitchOn); if (id != P::glitchAmount) ctx[P::glitchAmount] = 1.0f; }
        if (id == P::glitchOn) ctx[P::glitchAmount] = 1.0f;
        if (id.startsWith ("brep_") && id != P::brepOn) { on (P::brepOn); if (id != P::brepInterval) ctx[P::brepInterval] = norm (P::brepInterval, 7.0f); }
        if (id == P::brepOn) ctx[P::brepInterval] = norm (P::brepInterval, 7.0f);
        if (id == P::scThreshold) { ctx[P::scTrigger] = norm (P::scTrigger, 2.0f); }   // trigger audio sull'ingresso esterno
        // tabella a piu' frame: con una tabella a frame singolo la posizione non esiste
        if (id.endsWith ("_position")) ctx[id.upToFirstOccurrenceOf ("_", true, false) + "wave"] = norm (id.upToFirstOccurrenceOf ("_", true, false) + "wave", 6.0f);
        if (id == P::fmSource) { ctx[P::fmTarget] = norm (P::fmTarget, 1.0f); }
        if (id == P::f2EnvSource) ctx[P::f2EnvAmount] = norm (P::f2EnvAmount, 0.8f);
        if (id.startsWith ("env1_")) ctx[P::env (0, "sustain")] = norm (P::env (0, "sustain"), 0.2f);
        if (id.startsWith ("env") && id.endsWith ("curve_r")) ctx[id.replace ("curve_r", "release")] = norm (id.replace ("curve_r", "release"), 0.6f);
        if (id.startsWith ("env") && id.endsWith ("curve_a")) ctx[id.replace ("curve_a", "attack")] = norm (id.replace ("curve_a", "attack"), 0.4f);
        if (id.startsWith ("env") && id.endsWith ("curve_d")) ctx[id.replace ("curve_d", "decay")] = norm (id.replace ("curve_d", "decay"), 0.6f);
        if (id.startsWith ("lfo") && id.endsWith ("_phase")) ctx[id.replace ("phase", "trigger")] = norm (id.replace ("phase", "trigger"), 1.0f);
        if (id == P::delayOn)  ctx[P::delayMix]  = norm (P::delayMix, 0.5f);
        if (id == P::reverbOn) ctx[P::reverbMix] = norm (P::reverbMix, 0.5f);
        if (id == P::delaySync) { ctx[P::delayTimeL] = norm (P::delayTimeL, 0.6f); ctx[P::delayTimeR] = norm (P::delayTimeR, 0.6f); }
        if (id.startsWith ("space_"))
        {
            // contenuto stereo, altrimenti larghezza e mono-bass non hanno niente da fare
            ctx[P::osc (0, "uni_voices")] = 1.0f;
            ctx[P::osc (0, "uni_spread")] = 1.0f;
            if (id == P::spaceOn) ctx[P::spacePan] = norm (P::spacePan, 0.8f);
            if (id == P::spaceMonoFreq) ctx[P::spaceWidth] = norm (P::spaceWidth, 1.5f);
        }
        // l'ordine della catena ha una prova dedicata (sotto)
        if (id.startsWith ("fx_slot") && id != P::fxSlot (0)) continue;

        if (id == P::f2EnvSource) ctx[P::env (3, "attack")] = norm (P::env (3, "attack"), 0.4f);
        if (id.startsWith ("env2_") && id.endsWith ("curve_r")) ctx[P::env (0, "release")] = norm (P::env (0, "release"), 0.6f);

        auto lo = ctx, hi = ctx;
        lo[id] = 0.0f; hi[id] = 1.0f;
        // la fase e' un cerchio: 0 e 1 sono lo stesso punto, si prova mezzo giro
        if (id.startsWith ("lfo") && id.endsWith ("_phase")) hi[id] = 0.5f;
        if (id == P::scPhase) hi[id] = 0.75f;
        if (id.endsWith ("_phase_rand")) hi[id] = 0.5f;                       // voce singola: fase, 1 = 0                                  // anche PHASE del sidechain gira
        if (id == P::scShape) hi[id] = norm (P::scShape, 7.0f);                // CUSTOM di default = Classic
        if (id.endsWith ("_wave")) hi[id] = norm (id, 15.0f);                 // 16 = Sample: senza campione caricato suona la tabella 0
        const double d = relativeDiff (renderScene (lo, links, ext), renderScene (hi, links, ext));
        ++tested;
        if (d < 0.005) { ++muted; mutedList.add (id + " (" + juce::String (20.0 * std::log10 (juce::jmax (1.0e-9, d)), 1) + " dB)"); }
    }

    // Ordine della catena: distorsione prima o dopo il riverbero suona diverso.
    {
        std::map<juce::String, float> a { { P::distOn, 1.0f }, { P::reverbOn, 1.0f }, { P::reverbMix, norm (P::reverbMix, 0.5f) },
                                          { P::distDrive, 1.0f } };
        auto b = a;
        b[P::fxSlot (0)] = norm (P::fxSlot (0), 5.0f);   // reverb nello slot 1
        b[P::fxSlot (5)] = norm (P::fxSlot (5), 0.0f);   // distorsione nello slot 6
        const double d = relativeDiff (renderScene (a, {}), renderScene (b, {}));
        check (d > 0.005, "catena FX: l'ordine degli slot cambia il suono (" + juce::String (20.0 * std::log10 (d), 1) + " dB)");
    }

    for (auto& m : mutedList) std::printf ("      muto: %s\n", m.toRawUTF8());
    check (muted == 0, "impatto: " + juce::String (tested - muted) + "/" + juce::String (tested) + " parametri cambiano davvero l'audio");
}

//==============================================================================
/** Sidechain: sync sul battito, fase, banda, trigger MIDI ch16, preset. */
static void testSidechain()
{
    using namespace bassx;
    const double sr = 48000.0; const int bs = 128;
    const double spb = 0.5 * sr;                     // 120 BPM

    struct Run { std::vector<float> low, high, all; };
    auto render = [&] (std::function<void (BassXAudioProcessor&)> setup, bool ch16Kicks = false, int seconds = 2)
    {
        BassXAudioProcessor proc;
        FakePlayHead head; head.bpm = 120.0; head.ppq = 0.0; head.playing = true;
        proc.setPlayHead (&head);
        proc.presetManager.loadPresetByName ("Init");
        setParam (proc, ParamID::env (0, "sustain"), 1.0f);
        setParam (proc, ParamID::f1Cutoff, 6000.0f);
        setParam (proc, ParamID::f1EnvAmount, 0.0f);
        setParam (proc, ParamID::limiterOn, 0.0f);
        setup (proc);
        proc.prepareToPlay (sr, bs);

        juce::dsp::LinkwitzRileyFilter<float> split;
        split.prepare ({ sr, (juce::uint32) bs, 1 });
        split.setCutoffFrequency (90.0f);

        Run r;
        juce::AudioBuffer<float> buf (2, bs);
        const int blocks = (int) (seconds * sr / bs);
        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer midi;
            if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 33, (juce::uint8) 90), 0);
            if (ch16Kicks && b * bs % (int) spb < bs) midi.addEvent (juce::MidiMessage::noteOn (16, 36, (juce::uint8) 100), 0);
            buf.clear();
            proc.processBlock (buf, midi);
            head.ppq += bs / spb;
            for (int i = 0; i < bs; ++i)
            {
                const float x = 0.5f * (buf.getSample (0, i) + buf.getSample (1, i));
                float lo, hi; split.processSample (0, x, lo, hi);
                r.all.push_back (x); r.low.push_back (lo); r.high.push_back (hi);
            }
        }
        proc.setPlayHead (nullptr);
        return r;
    };

    // rms in una finestra di 20 ms a una frazione del battito, mediata sui battiti dopo il primo
    auto rmsAt = [&] (const std::vector<float>& v, double frac, double halfWin = 0.004)
    {
        double e = 0.0; int n = 0;
        for (int beat = 1; beat < 3; ++beat)
        {
            const int c = (int) ((beat + frac) * spb), w = (int) (halfWin * sr);
            for (int i = c - w; i < c + w; ++i) { e += (double) v[(size_t) i] * v[(size_t) i]; ++n; }
        }
        return 10.0 * std::log10 (juce::jmax (1.0e-20, e / juce::jmax (1, n)));
    };

    auto on = [] (BassXAudioProcessor& p) { setParam (p, ParamID::scOn, 1.0f); setParam (p, ParamID::scShape, 0.0f); };
    const auto off  = render ([] (BassXAudioProcessor&) {});
    const auto sync = render (on);
    const double dip = rmsAt (sync.all, 0.01) - rmsAt (sync.all, 0.7);
    check (dip < -10.0, "sidechain sync 1/4: taglio sul battito (" + juce::String (dip, 1) + " dB rispetto a meta' battito)");
    check (std::abs (rmsAt (off.all, 0.03) - rmsAt (off.all, 0.7)) < 1.0, "sidechain spento: nessun ducking");

    // PHASE +25%: il taglio si sposta di un quarto di battito
    const auto shifted = render ([&] (BassXAudioProcessor& p) { on (p); setParam (p, ParamID::scPhase, 0.25f); });
    check (rmsAt (shifted.all, 0.26) < rmsAt (shifted.all, 0.03) - 10.0,
           "sidechain PHASE 25%: il taglio cade un quarto di battito dopo");

    // BAND 20..150 Hz: si abbassano solo le basse
    // forma GATE (chiusa per il primo quarto): finestre lunghe, la fondamentale a 55 Hz ha periodo 18 ms
    const auto band = render ([&] (BassXAudioProcessor& p) { on (p); setParam (p, ParamID::scShape, 5.0f); setParam (p, ParamID::scBandHigh, 150.0f); });
    const double lowDip  = rmsAt (band.low, 0.12, 0.04) - rmsAt (band.low, 0.6, 0.04);
    const double highDip = rmsAt (band.high, 0.12, 0.04) - rmsAt (band.high, 0.6, 0.04);
    check (lowDip < -12.0 && highDip > -4.0, "sidechain BAND 20-150 Hz: basse " + juce::String (lowDip, 1)
             + " dB, alte " + juce::String (highDip, 1) + " dB");

    // MIDI ch16: la cassa del piano roll comanda il ducking e non suona
    const auto midi = render ([&] (BassXAudioProcessor& p) { on (p); setParam (p, ParamID::scTrigger, 1.0f); }, true);
    check (rmsAt (midi.all, 0.02) - rmsAt (midi.all, 0.7) < -10.0, "sidechain MIDI ch16: taglio sulle note del canale 16");

    // la curva CUSTOM arriva al DSP
    const auto custom = render ([&] (BassXAudioProcessor& p) {
        on (p);
        SidechainDucker::Curve c; c.numPoints = 3;
        c.points[0] = { 0.0f, 1.0f, 0.0f }; c.points[1] = { 0.5f, 0.0f, 0.0f }; c.points[2] = { 1.0f, 1.0f, 0.0f };
        p.setSidechainCustomCurve (c);
        setParam (p, ParamID::scShape, (float) SidechainDucker::kCustomShape); });
    check (rmsAt (custom.all, 0.5) < rmsAt (custom.all, 0.05) - 15.0, "sidechain CUSTOM: il taglio segue la curva disegnata (meta' battito)");

    // cambiare preset non spegne il sidechain
    {
        BassXAudioProcessor p;
        setParam (p, ParamID::scOn, 1.0f);
        p.presetManager.loadPreset (4);
        check (getParam (p, ParamID::scOn) > 0.5f, "cambio preset: il sidechain resta acceso");
    }
}

//==============================================================================
/** Beat repeat: dopo il primo GRID l'uscita e' quel GRID ripetuto. Glitch:
    reverse = passo precedente al contrario; stesso seed, stesso risultato. */
static void testGlitchRepeat()
{
    using namespace bassx;
    const double sr = 48000.0; const int bs = 64;
    const double bpm = 120.0, spb = 60.0 / bpm * sr;

    auto noise = [] (int n, uint32_t seed)
    {
        std::vector<float> v ((size_t) n);
        for (auto& x : v) { seed = seed * 1664525u + 1013904223u; x = (float) (int32_t) seed / 2147483648.0f * 0.5f; }
        return v;
    };

    // --- beat repeat: interval 1 battito, grid 1/16, gate 1/4, decay 0, mix 1
    {
        BeatRepeat br; br.prepare (sr);
        br.set (true, 1.0f, 0.25f, 1.0f, 1.0f, 0.0f, 1.0f);
        const int total = (int) (2.0 * spb);
        auto in = noise (total, 7u), out = in;
        for (int off = 0; off < total; off += bs)
        {
            float* chan[1] = { out.data() + off };   // l'AudioBlock tiene il puntatore all'array dei canali
            juce::dsp::AudioBlock<float> blk (chan, 1, (size_t) bs);
            br.process (blk, bpm, (double) off / spb, true);
        }
        // battito 1: il secondo e il terzo sedicesimo (lontano dai bordi) ripetono il primo
        const int g = (int) (0.25 * spb), b1 = (int) spb;
        double err = 0.0, ref = 0.0;
        for (int i = g / 4; i < g * 3 / 4; ++i)
        {
            err += std::abs (out[(size_t) (b1 + g + i)] - in[(size_t) (b1 + i)]) + std::abs (out[(size_t) (b1 + 2 * g + i)] - in[(size_t) (b1 + i)]);
            ref += std::abs (in[(size_t) (b1 + i)]) * 2.0;
        }
        check (err / ref < 0.01, "beat repeat: il grid catturato si ripete (errore " + juce::String (err / ref, 4) + ")");
    }

    // --- glitch reverse, amount 1: il passo k suona il passo k-1 al contrario
    auto runGlitch = [&] (int mode, int seed)
    {
        GlitchFx gl; gl.prepare (sr);
        gl.set (true, 0.25f, 1.0f, 1.0f, mode, seed);
        const int total = (int) (2.0 * spb);
        auto v = noise (total, 11u);
        for (int off = 0; off < total; off += bs)
        {
            float* chan[1] = { v.data() + off };   // l'AudioBlock tiene il puntatore all'array dei canali
            juce::dsp::AudioBlock<float> blk (chan, 1, (size_t) bs);
            gl.process (blk, bpm, (double) off / spb, true);
        }
        return v;
    };
    {
        const auto in = noise ((int) (2.0 * spb), 11u);
        const auto out = runGlitch ((int) GlitchFx::Mode::Reverse, 0);
        const int g = (int) (0.25 * spb), k = 4 * g;   // passo 4
        double err = 0.0, ref = 0.0;
        for (int i = g / 4; i < g * 3 / 4; ++i)
        {
            err += std::abs (out[(size_t) (k + i)] - in[(size_t) (k - 1 - i)]);
            ref += std::abs (in[(size_t) (k - 1 - i)]);
        }
        check (err / ref < 0.02, "glitch reverse: il passo precedente al contrario (errore " + juce::String (err / ref, 4) + ")");
    }
    {
        const auto a = runGlitch (0, 3), b = runGlitch (0, 3), c = runGlitch (0, 4);
        check (a == b && a != c, "glitch random: stesso seed stesso risultato, seed diverso risultato diverso");
    }
}

//==============================================================================
/** Export del loop: lunghezza esatta in battute, non muto, arpeggio coi tasti. */
static void testLoopExport()
{
    using namespace bassx;
    auto run = [] (bool arp, int bars, juce::String& error, juce::File& f)
    {
        BassXAudioProcessor proc;
        FakePlayHead head; head.bpm = 128.0; head.playing = true;
        proc.setPlayHead (&head);
        proc.presetManager.loadPresetByName ("Init");
        setParam (proc, arp ? ParamID::arpEnable : ParamID::seqEnable, 1.0f);
        if (arp) setParam (proc, ParamID::arpLatch, 1.0f);
        proc.prepareToPlay (48000.0, 256);
        juce::AudioBuffer<float> buf (2, 256);
        for (int b = 0; b < 20; ++b)
        {
            juce::MidiBuffer midi;
            if (arp && b == 1) { midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0); midi.addEvent (juce::MidiMessage::noteOn (1, 55, (juce::uint8) 100), 0); }
            if (arp && b == 3) { midi.addEvent (juce::MidiMessage::noteOff (1, 48), 0); midi.addEvent (juce::MidiMessage::noteOff (1, 55), 0); }
            buf.clear(); proc.processBlock (buf, midi); head.ppq += 256.0 / (60.0 / 128.0 * 48000.0);
        }
        proc.setPlayHead (nullptr);
        f = juce::File::createTempFile (".wav");
        const auto job = proc.makeLoopExportJob();
        return BassXAudioProcessor::exportLoop (job, f, bars, error);
    };

    for (bool arp : { false, true })
    {
        juce::String error; juce::File f;
        const bool ok = run (arp, 2, error, f);
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (ok ? fm.createReaderFor (f) : nullptr);
        const long long expected = (long long) std::lround (2 * 4 * 60.0 / 128.0 * 48000.0);
        float peak = 0.0f;
        if (r != nullptr)
        {
            juce::AudioBuffer<float> b (2, (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
            peak = b.getMagnitude (0, b.getNumSamples());
        }
        check (r != nullptr && r->lengthInSamples == expected && r->bitsPerSample == 24 && peak > 0.01f,
               juce::String ("export WAV ") + (arp ? "arp" : "seq") + ": 2 battute a 128 BPM = " + juce::String (expected)
                 + " campioni, 24 bit, non muto" + (ok ? "" : " (" + error + ")"));
        f.deleteFile();
    }
}

//==============================================================================
/** Campione trascinato -> wavetable: periodo trovato, forma conservata,
    intonata sulla tastiera, salvata nella sessione. */
static void testSampleWavetable()
{
    using namespace bassx;
    const double sr = 44100.0;

    // campione: saw a 98 Hz con armoniche che si spengono nel tempo
    auto f = juce::File::createTempFile (".wav");
    {
        juce::AudioBuffer<float> a (1, (int) (sr * 1.5));
        double ph = 0.0;
        for (int i = 0; i < a.getNumSamples(); ++i)
        {
            const float t = (float) i / (float) a.getNumSamples();
            float v = 0.0f;
            for (int h = 1; h < 30; ++h) v += (float) std::sin (2.0 * juce::MathConstants<double>::pi * h * ph) / (float) h * std::pow (1.0f - t, (float) h * 0.15f);
            a.setSample (0, i, 0.4f * v);
            ph += 98.0 / sr; if (ph >= 1.0) ph -= 1.0;
        }
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), sr, 1, 24, {}, 0));
        w->writeFromAudioSampleBuffer (a, 0, a.getNumSamples());
    }

    // periodo
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        juce::AudioBuffer<float> a (1, (int) r->lengthInSamples);
        r->read (&a, 0, a.getNumSamples(), 0, true, false);
        std::vector<float> x (a.getReadPointer (0), a.getReadPointer (0) + a.getNumSamples());
        const double period = SampleWavetable::detectPeriod (x, sr);
        check (std::abs (period - sr / 98.0) < 1.0, "campione > wavetable: periodo trovato (" + juce::String (period, 2) + " vs " + juce::String (sr / 98.0, 2) + ")");
    }

    BassXAudioProcessor proc;
    proc.setPlayHead (nullptr);
    juce::String error;
    const bool ok = proc.loadSampleWavetable (f, error);
    check (ok && proc.getSampleWavetable() != nullptr && proc.getSampleWavetable()->getNumFrames() == kFramesPerMorphTable,
           "campione > wavetable: 16 frame caricati" + (ok ? juce::String() : " (" + error + ")"));

    // suona intonata: nota 45 (110 Hz) con la forma Sample, filtro aperto
    auto renderPitch = [] (BassXAudioProcessor& p)
    {
        p.presetManager.loadPresetByName ("Init");
        setParam (p, ParamID::osc (0, "wave"), (float) kUserWaveIndex);
        setParam (p, ParamID::f1Model, (float) (int) MultiFilter::Model::Bypass);
        setParam (p, ParamID::env (0, "sustain"), 1.0f);
        p.prepareToPlay (48000.0, 256);
        std::vector<float> mono;
        juce::AudioBuffer<float> buf (2, 256);
        for (int b = 0; b < 200; ++b)
        {
            juce::MidiBuffer midi;
            if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 45, (juce::uint8) 90), 0);
            buf.clear(); p.processBlock (buf, midi);
            for (int i = 0; i < 256; ++i) mono.push_back (buf.getSample (0, i));
        }
        // frequenza dai passaggi per zero in salita sulla parte stabile
        int crossings = 0, first = -1, last = -1;
        for (size_t i = 24000; i + 1 < mono.size(); ++i)
            if (mono[i] <= 0.0f && mono[i + 1] > 0.0f) { if (first < 0) first = (int) i; last = (int) i; ++crossings; }
        return crossings > 1 ? 48000.0 * (crossings - 1) / (double) (last - first) : 0.0;
    };
    const double hz = renderPitch (proc);
    check (std::abs (hz - 110.0) < 1.5, "campione > wavetable: la nota A2 suona a " + juce::String (hz, 2) + " Hz (attesi 110)");

    // la sessione porta con se' la wavetable (anche se il file sparisce)
    juce::MemoryBlock st;
    proc.getStateInformation (st);
    f.deleteFile();
    BassXAudioProcessor reopened;
    reopened.setStateInformation (st.getData(), (int) st.getSize());
    check (reopened.getSampleWavetable() != nullptr && reopened.getSampleWavetableName() == proc.getSampleWavetableName(),
           "campione > wavetable: salvata nella sessione");
}

//==============================================================================
/** Layer = un basso solo: le fondamentali di oscillatori, sub e campioni
    sommano in fase, nota dopo nota, con e senza retrigger. */
static void testLayerCoherence()
{
    using namespace bassx;
    using P = juce::String;

    // 1. ogni frame del banco ha la fondamentale a +sin
    {
        SharedWavetableBank bank;
        juce::dsp::FFT fft (11);
        std::vector<float> d ((size_t) kTableSize * 2);
        float worst = 0.0f; juce::String where;
        for (int t = 0; t < bank->getNumTables(); ++t)
            for (int f = 0; f < bank->getTable (t).getNumFrames(); ++f)
            {
                const auto& m = bank->getTable (t).frames[(size_t) f].mip[0];
                std::fill (d.begin(), d.end(), 0.0f);
                std::copy (m.begin(), m.begin() + kTableSize, d.begin());
                fft.performRealOnlyForwardTransform (d.data(), true);
                if (std::hypot (d[2], d[3]) < 1.0f) continue;               // senza fondamentale
                float e = std::abs (std::remainder (std::atan2 (d[3], d[2]) - sineFundamentalAngle(), juce::MathConstants<float>::twoPi));
                if (e > worst) { worst = e; where = bank->getTable (t).name + " frame " + juce::String (f); }
            }
        check (worst < 0.01f, "layer: fondamentale in fase su tutte le tabelle (peggiore " + juce::String (juce::radiansToDegrees (worst), 2) + " gradi, " + where + ")");
    }

    // 2. render: ampiezza complessa della fondamentale (110 Hz) sulla parte stabile
    struct Cx { double re = 0, im = 0; double mag() const { return std::hypot (re, im); } };
    auto render = [] (std::function<void (BassXAudioProcessor&)> setup, bool secondNote)
    {
        BassXAudioProcessor p;
        p.presetManager.loadPresetByName ("Init");
        for (int o = 0; o < kNumOscillators; ++o) { setParam (p, ParamID::osc (o, "enable"), 0.0f); setParam (p, ParamID::osc (o, "level"), 1.0f); setParam (p, ParamID::osc (o, "octave"), 0.0f); }
        setParam (p, ParamID::subEnable, 0.0f);
        setParam (p, ParamID::noiseEnable, 0.0f);
        setParam (p, ParamID::f1Model, (float) (int) MultiFilter::Model::Bypass);
        setParam (p, ParamID::env (0, "sustain"), 1.0f);
        setup (p);
        p.prepareToPlay (48000.0, 256);
        std::vector<float> mono;
        juce::AudioBuffer<float> buf (2, 256);
        const int start = secondNote ? 300 : 0;
        for (int b = 0; b < start + 200; ++b)
        {
            juce::MidiBuffer midi;
            if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 45, (juce::uint8) 90), 0);
            if (secondNote && b == 150) midi.addEvent (juce::MidiMessage::noteOff (1, 45), 0);
            if (secondNote && b == start) midi.addEvent (juce::MidiMessage::noteOn (1, 45, (juce::uint8) 90), 37);
            buf.clear(); p.processBlock (buf, midi);
            if (b >= start) for (int i = 0; i < 256; ++i) mono.push_back (buf.getSample (0, i));
        }
        Cx c;
        for (int i = 12000; i < 36000; ++i)            // 0.5 s = 55 periodi esatti
        {
            const double w = 2.0 * juce::MathConstants<double>::pi * 110.0 * (i - 12000) / 48000.0;
            c.re += mono[(size_t) i] * std::cos (w); c.im -= mono[(size_t) i] * std::sin (w);
        }
        return c;
    };
    auto deg = [] (Cx a, Cx b) { return std::abs (juce::radiansToDegrees (std::remainder (std::atan2 (a.im, a.re) - std::atan2 (b.im, b.re), juce::MathConstants<double>::twoPi))); };
    auto oscOn = [] (int o, int wave) { return [o, wave] (BassXAudioProcessor& p) { setParam (p, ParamID::osc (o, "enable"), 1.0f); setParam (p, ParamID::osc (o, "wave"), (float) wave); }; };
    auto both = [] (std::function<void (BassXAudioProcessor&)> a, std::function<void (BassXAudioProcessor&)> b) { return [a, b] (BassXAudioProcessor& p) { a (p); b (p); }; };

    const std::pair<int, int> pairs[] = { { 3, 4 }, { 0, 1 }, { 0, 2 }, { 3, 7 }, { 10, 13 } };
    double worstDeg = 0.0, worstSum = 1.0;
    for (auto [a, b] : pairs)
    {
        const auto ca = render (oscOn (0, a), false), cb = render (oscOn (1, b), false);
        const auto cm = render (both (oscOn (0, a), oscOn (1, b)), false);
        worstDeg = std::max (worstDeg, deg (ca, cb));
        worstSum = std::min (worstSum, cm.mag() / (0.5 * (ca.mag() + cb.mag())));
    }
    check (worstDeg < 5.0, "layer: osc 1 e osc 2 con forme diverse partono in fase (scarto max " + juce::String (worstDeg, 1) + " gradi)");
    check (worstSum > 0.99, "layer: le fondamentali si sommano, non si cancellano (" + juce::String (100.0 * worstSum, 1) + "% della somma piena)");

    // 3. sub in fase con l'oscillatore (sub alla stessa ottava, tutte e tre le forme)
    double subDeg = 0.0;
    for (int shape = 0; shape < 3; ++shape)
    {
        const auto co = render (oscOn (0, 3), false);
        const auto cs = render ([shape] (BassXAudioProcessor& p) { setParam (p, ParamID::subEnable, 1.0f); setParam (p, ParamID::subOctave, 0.0f); setParam (p, ParamID::subLevel, 1.0f); setParam (p, ParamID::subShape, (float) shape); }, false);
        subDeg = std::max (subDeg, cs.mag() > 1.0 ? deg (co, cs) : 180.0);
    }
    check (subDeg < 5.0, "layer: sub (sine/tri/square) in fase con l'oscillatore (scarto max " + juce::String (subDeg, 1) + " gradi)");

    // 4. senza retrigger i layer girano liberi ma restano allineati anche alla nota dopo
    auto free = [] (std::function<void (BassXAudioProcessor&)> f) { return [f] (BassXAudioProcessor& p) { f (p); for (int o = 0; o < kNumOscillators; ++o) setParam (p, ParamID::osc (o, "retrig"), 0.0f); }; };
    const auto fa = render (free (oscOn (0, 3)), true), fb = render (free (oscOn (1, 4)), true);
    check (deg (fa, fb) < 5.0, "layer: retrigger OFF, seconda nota ancora in fase (" + juce::String (deg (fa, fb), 1) + " gradi)");

    // 5. PHASE su voce singola sposta davvero il layer (scelta, non caso)
    const auto ph = render (both (oscOn (0, 3), [] (BassXAudioProcessor& p) { setParam (p, ParamID::osc (0, "phase_rand"), 0.25f); }), false);
    const auto p0 = render (oscOn (0, 3), false);
    check (std::abs (deg (ph, p0) - 90.0) < 3.0, "layer: PHASE 25% = fondamentale spostata di 90 gradi (" + juce::String (deg (ph, p0), 1) + ")");
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;

    BassXAudioProcessor proc;

    const double sr = 48000.0;
    const int blockSize = 256;
    proc.prepareToPlay (sr, blockSize);
    proc.setPlayHead (nullptr);

    const int numPresets = proc.presetManager.getNumPresets();
    int failures = 0;

    for (int p = 0; p < numPresets; ++p)
    {
        proc.presetManager.loadPreset (p);
        proc.prepareToPlay (sr, blockSize);

        juce::AudioBuffer<float> buffer (2, blockSize);

        double peak = 0.0, rms = 0.0;
        long long total = 0;
        bool bad = false;

        // 3 secondi: note-on con accento, slide dopo 0.5 s, note-off a 2 s.
        const int totalBlocks = (int) (sr * 3.0 / blockSize);

        for (int b = 0; b < totalBlocks; ++b)
        {
            juce::MidiBuffer midi;
            const double t = (double) b * blockSize / sr;

            if (b == 0)                       midi.addEvent (juce::MidiMessage::noteOn (1, 45, (juce::uint8) 110), 0);
            else if (t >= 0.5 && t < 0.5 + blockSize / sr)
                                              midi.addEvent (juce::MidiMessage::noteOn (1, 52, (juce::uint8) 100), 0);
            else if (t >= 2.0 && t < 2.0 + blockSize / sr)
            {
                midi.addEvent (juce::MidiMessage::noteOff (1, 52), 0);
                midi.addEvent (juce::MidiMessage::noteOff (1, 45), 0);
            }

            buffer.clear();
            proc.processBlock (buffer, midi);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                {
                    const float v = buffer.getSample (ch, i);
                    if (! std::isfinite (v)) { bad = true; }
                    peak = juce::jmax (peak, (double) std::abs (v));
                    rms += (double) v * v;
                    ++total;
                }
        }

        rms = std::sqrt (rms / (double) juce::jmax (1LL, total));

        const auto name = proc.presetManager.getPresetName (p);
        const bool silent = rms < 1.0e-5;
        const bool tooLoud = peak > 1.001;

        if (bad || silent || tooLoud)
        {
            ++failures;
            std::printf ("FAIL  %-24s peak=%.4f rms=%.5f%s%s%s\n", name.toRawUTF8(), peak, rms,
                         bad ? "  [NaN/inf]" : "", silent ? "  [silenzio]" : "",
                         tooLoud ? "  [oltre il tetto]" : "");
        }
        else
        {
            std::printf ("ok    %-24s peak=%.4f rms=%.5f  (%.1f dBFS peak)\n",
                         name.toRawUTF8(), peak, rms, 20.0 * std::log10 (peak));
        }
    }

    // --- Misura dell'aliasing ------------------------------------------------
    // Nota alta con filtro spalancato e risonanza a zero: qualunque energia
    // NON su un multiplo intero della fondamentale è aliasing.
    {
        proc.presetManager.loadPresetByName ("Init");

        auto set = [&proc] (const juce::String& id, float value)
        {
            if (auto* p = proc.apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (value));
        };

        set (bassx::ParamID::f1Model, (float) (int) bassx::MultiFilter::Model::Bypass);
        set (bassx::ParamID::f1EnvAmount, 0.0f);
        set (bassx::ParamID::env (0, "sustain"), 1.0f);
        set (bassx::ParamID::env (0, "attack"), 0.001f);
        set (bassx::ParamID::outputGain, -6.0f);
        set (bassx::ParamID::limiterOn, 0.0f);

        proc.prepareToPlay (sr, blockSize);

        const int captureLength = 1 << 16;
        std::vector<float> capture;
        capture.reserve ((size_t) captureLength + (size_t) blockSize);

        juce::AudioBuffer<float> buffer (2, blockSize);
        bool started = false;

        while ((int) capture.size() < captureLength)
        {
            juce::MidiBuffer midi;

            if (! started)
            {
                midi.addEvent (juce::MidiMessage::noteOn (1, 93, (juce::uint8) 90), 0);  // A6
                started = true;
            }

            buffer.clear();
            proc.processBlock (buffer, midi);

            for (int i = 0; i < blockSize; ++i)
                capture.push_back (buffer.getSample (0, i));
        }

        juce::File out (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("bassx_alias.raw"));
        out.deleteFile();
        juce::FileOutputStream fs (out);
        fs.write (capture.data(), (size_t) captureLength * sizeof (float));
        std::printf ("\nCattura per l'analisi di aliasing: %s (%d campioni, %.1f Hz)\n",
                     out.getFullPathName().toRawUTF8(), captureLength,
                     440.0 * std::pow (2.0, (93.0 - 69.0) / 12.0));
    }

    // --- Catena effetti riordinata + tutte le sezioni nuove attive -------------
    std::printf ("\n--- Routing FX / sezioni nuove ---\n");
    {
        FakePlayHead head;
        proc.setPlayHead (&head);

        juce::Random rnd (42);
        int fxFailures = 0;

        for (int trial = 0; trial < 12; ++trial)
        {
            proc.presetManager.loadPreset (trial % numPresets);

            auto set = [&proc] (const juce::String& id, float value)
            {
                if (auto* p = proc.apvts.getParameter (id))
                    p->setValueNotifyingHost (p->convertTo0to1 (value));
            };

            for (int slot = 0; slot < bassx::kNumFxSlots; ++slot)
                set (bassx::ParamID::fxSlot (slot), (float) rnd.nextInt (bassx::kNumFxSlots));

            set (bassx::ParamID::distOn, 1.0f);
            set (bassx::ParamID::delayOn, 1.0f);
            set (bassx::ParamID::reverbOn, 1.0f);
            set (bassx::ParamID::chorusOn, 1.0f);
            set (bassx::ParamID::spaceWidth, 1.6f);
            set (bassx::ParamID::spaceMonoFreq, 120.0f);
            set (bassx::ParamID::spaceAutopan, 0.5f);
            set (bassx::ParamID::spaceHaas, 0.012f);
            set (bassx::ParamID::dfNeuralDrive, 0.6f);
            set (bassx::ParamID::dfFilterFm, 0.4f);
            set (bassx::ParamID::dfMuffler, 0.5f);
            set (bassx::ParamID::dfVibDepth, 0.3f);
            set (bassx::ParamID::dfSweepSpeed, 0.5f);
            set (bassx::ParamID::dfEnvAttack, 0.01f);
            set (bassx::ParamID::seqEnable, (trial & 1) ? 1.0f : 0.0f);
            set (bassx::ParamID::arpEnable, (trial & 2) ? 1.0f : 0.0f);
            set (bassx::ParamID::playCombine, (float) (trial % 3));

            proc.prepareToPlay (sr, blockSize);
            head.ppq = 0.0;

            juce::AudioBuffer<float> buffer (2, blockSize);
            double peak = 0.0, rms = 0.0; long long total = 0; bool bad = false;

            for (int b = 0; b < (int) (sr * 2.0 / blockSize); ++b)
            {
                juce::MidiBuffer midi;
                if (b == 0) { midi.addEvent (juce::MidiMessage::noteOn (1, 45, (juce::uint8) 110), 0);
                              midi.addEvent (juce::MidiMessage::noteOn (1, 52, (juce::uint8) 90), 0); }
                buffer.clear();
                proc.processBlock (buffer, midi);
                head.ppq += blockSize / (60.0 / head.bpm * sr);

                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const float v = buffer.getSample (ch, i);
                        if (! std::isfinite (v)) bad = true;
                        peak = juce::jmax (peak, (double) std::abs (v));
                        rms += (double) v * v; ++total;
                    }
            }

            rms = std::sqrt (rms / (double) total);
            if (bad || rms < 1.0e-5 || peak > 1.001) ++fxFailures;
        }

        check (fxFailures == 0, "12 catene FX casuali con seq/arp/fish/space: niente NaN, silenzio o clipping (" + juce::String (fxFailures) + " problemi)");

        // --- Nessuna allocazione nel processBlock ------------------------------
        {
            proc.presetManager.loadPresetByName ("Init");
            if (auto* p = proc.apvts.getParameter (bassx::ParamID::seqEnable)) p->setValueNotifyingHost (1.0f);
            if (auto* p = proc.apvts.getParameter (bassx::ParamID::arpEnable)) p->setValueNotifyingHost (1.0f);
            if (auto* p = proc.apvts.getParameter (bassx::ParamID::dfNeuralDrive)) p->setValueNotifyingHost (0.5f);
            proc.prepareToPlay (sr, blockSize);

            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            midi.ensureSize (256);

            // riscaldamento: la prima chiamata riempie le cache
            buffer.clear();
            proc.processBlock (buffer, midi);

            gAllocations = 0;
            gTrackAllocations = true;

            for (int b = 0; b < 200; ++b)
            {
                midi.clear();
                if (b == 3) midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0);
                if (b == 150) midi.addEvent (juce::MidiMessage::noteOff (1, 48), 0);
                buffer.clear();
                proc.processBlock (buffer, midi);
                head.ppq += blockSize / (60.0 / head.bpm * sr);
            }

            gTrackAllocations = false;
            check (gAllocations.load() == 0, "processBlock senza allocazioni (" + juce::String ((int) gAllocations.load()) + ")");
        }

        // --- Ext Mode ------------------------------------------------------------
        {
            proc.presetManager.loadPresetByName ("Init");
            auto layout = proc.getBusesLayout();
            layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
            const bool layoutOk = proc.setBusesLayout (layout);
            if (auto* p = proc.apvts.getParameter (bassx::ParamID::dfExtMode)) p->setValueNotifyingHost (1.0f);
            proc.prepareToPlay (sr, blockSize);

            juce::AudioBuffer<float> buffer (2, blockSize);
            double rmsWithNote = 0.0, rmsNoNote = 0.0;

            for (int b = 0; b < 100; ++b)
            {
                juce::MidiBuffer midi;
                if (b == 50) midi.addEvent (juce::MidiMessage::noteOn (1, 45, (juce::uint8) 100), 0);

                for (int i = 0; i < blockSize; ++i)
                {
                    const float v = 0.4f * std::sin (bassx::kTwoPi * 110.0f * (float) (b * blockSize + i) / (float) sr);
                    buffer.setSample (0, i, v);
                    buffer.setSample (1, i, v);
                }

                proc.processBlock (buffer, midi);

                double r = 0.0;
                for (int i = 0; i < blockSize; ++i) r += (double) buffer.getSample (0, i) * buffer.getSample (0, i);
                if (b >= 20 && b < 50) rmsNoNote += r;
                if (b >= 70) rmsWithNote += r;
            }

            check (layoutOk && rmsWithNote > 1.0e-3 && rmsNoNote < rmsWithNote * 1.0e-3,
                   "ext mode: l'ingresso passa da filtro e VCA solo con la nota (rms " + juce::String (rmsWithNote, 4) + " vs " + juce::String (rmsNoNote, 7) + ")");

            layout.inputBuses.getReference (0) = juce::AudioChannelSet::disabled();
            proc.setBusesLayout (layout);
        }

        proc.setPlayHead (nullptr);
    }

    testSequencer();
    testDistortionLatency();
    testTube();
    testSpace();
    testNeuralInference (argc > 1 ? argv[1] : "tools/neural/reference_io.txt");
    testStopWithLatch();
    testDelayNoStaleLoop();
    testPresets();
    testZipper();
    testFilterModels();
    testFilterGraph();
    testSidechain();
    testGlitchRepeat();
    testLoopExport();
    testSampleWavetable();
    testLayerCoherence();
    testEveryParameterImpact();

    std::printf ("\n%d preset, %d problemi | %d verifiche, %d fallite\n", numPresets, failures, gChecks, gCheckFailures);
    return (failures == 0 && gCheckFailures == 0) ? 0 : 1;
}
