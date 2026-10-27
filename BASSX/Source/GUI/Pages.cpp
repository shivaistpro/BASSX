/*
  ==============================================================================

    BASSX — Pages.cpp

  ==============================================================================
*/

#include "Pages.h"
#include "../PluginProcessor.h"
#include <complex>

namespace bassx
{

using namespace juce;

namespace
{
    constexpr int kHeaderH = 18;
    constexpr int kRowGap  = 6;
    constexpr int kPad     = 8;

    /** Altezza minima sotto la quale un knob con etichetta non è più
        leggibile: sotto i 46 px il testo scende sotto i 9 pt. */
    constexpr int kKnobMinH = 46;

    /** Altezza di un modulo con una sola fila di knob: tutte le pagine
        "leggere" usano questa misura, così le sezioni restano allineate. */
    constexpr int kModH = 6 + kHeaderH + 4 + PageBase::kKnobRowH + 6;

    /** Fondo da display (vetro scuro, griglia, scanline) comune agli strumenti. */
    void paintDisplayGlass (Graphics& g, Rectangle<float> b)
    {
        auto shape = Shapes::chamfer (b, 8.0f);
        g.setColour (Palette::bg0);
        g.fillPath (shape);

        g.setColour (Palette::line.withAlpha (0.6f));
        for (int i = 1; i < 8; ++i)
            g.fillRect (b.getX() + b.getWidth() * (float) i / 8.0f, b.getY(), 1.0f, b.getHeight());
        for (int i = 1; i < 4; ++i)
            g.fillRect (b.getX(), b.getY() + b.getHeight() * (float) i / 4.0f, b.getWidth(), 1.0f);

        g.setColour (juce::Colours::black.withAlpha (0.25f));
        for (float y = b.getY() + 1.0f; y < b.getBottom(); y += 3.0f)
            g.fillRect (b.getX(), y, b.getWidth(), 1.0f);

        g.setColour (Palette::line.brighter (0.2f));
        g.strokePath (shape, PathStrokeType (1.0f));
    }

    void strokeGlow (Graphics& g, const Path& p, Colour c, float w = 1.6f)
    {
        g.setColour (c.withAlpha (0.18f));
        g.strokePath (p, PathStrokeType (w * 4.0f, PathStrokeType::curved, PathStrokeType::rounded));
        g.setColour (c);
        g.strokePath (p, PathStrokeType (w, PathStrokeType::curved, PathStrokeType::rounded));
    }

    /** Dispone un modulo: imposta il pannello e restituisce l'area interna. */
    Rectangle<int> placeModule (TexturedPanel* p, Rectangle<int> r)
    {
        p->setBounds (r);
        return r.reduced (kPad, 6);
    }
}

//==============================================================================
// OutputScope / FilterResponse
//==============================================================================

void OutputScope::update (const BassXAudioProcessor& proc)
{
    constexpr int n = BassXAudioProcessor::kScopeSize;
    const int w = proc.scopeWrite.load (std::memory_order_relaxed);

    std::vector<float> ring ((size_t) n);
    for (int i = 0; i < n; ++i)
        ring[(size_t) i] = 0.5f * (proc.scopeL[(size_t) ((w + i) % n)].load (std::memory_order_relaxed)
                                 + proc.scopeR[(size_t) ((w + i) % n)].load (std::memory_order_relaxed));

    // Trigger: primo passaggio per lo zero in salita nella prima metà.
    int start = 0;
    for (int i = 1; i < n / 2; ++i)
        if (ring[(size_t) i - 1] <= 0.0f && ring[(size_t) i] > 0.0f) { start = i; break; }

    samples.assign (ring.begin() + start, ring.begin() + start + n / 2);
    repaint();
}

void OutputScope::paint (Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (0.5f);
    paintDisplayGlass (g, b);

    if (samples.size() < 2)
        return;

    float peak = 0.05f;
    for (auto v : samples) peak = jmax (peak, std::abs (v));

    Path p;
    const float mid = b.getCentreY(), half = b.getHeight() * 0.42f;
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const float x = b.getX() + b.getWidth() * (float) i / (float) (samples.size() - 1);
        const float y = mid - samples[i] / peak * half;
        if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
    }
    strokeGlow (g, p, Palette::lcdGreen);
}

void FilterResponse::setSettings (const Settings& s)
{
    if (valid && s == settings)
        return;

    settings = s;

    if (freqs.empty())
        for (int i = 0; i <= 240; ++i)
            freqs.push_back (20.0f * std::pow (1000.0f, (float) i / 240.0f));

    measure (settings, freqs, total, f1, f2);
    valid = true;
    repaint();
}

void FilterResponse::measure (const Settings& s, const std::vector<float>& hz,
                              std::vector<float>& totalDb, std::vector<float>& f1Db, std::vector<float>& f2Db)
{
    // Impulso piccolo (i modelli con saturazione restano lineari) attraverso
    // gli stessi filtri e lo stesso routing di Voice::renderNextBlock.
    constexpr int order = 14, n = 1 << order;
    constexpr double fs = 48000.0;
    constexpr float amp = 1.0e-3f;

    using Model = MultiFilter::Model;
    const int last = (int) Model::NumModels - 1;
    MultiFilter a, b;
    a.prepare (fs); b.prepare (fs);
    a.setModel ((Model) jlimit (0, last, s.m1)); b.setModel ((Model) jlimit (0, last, s.m2));
    a.setCutoff (s.c1); a.setResonance (s.r1); a.setDrive (s.d1); a.setBassCompensation (s.bass);
    b.setCutoff (s.c2); b.setResonance (s.r2); b.setDrive (s.d2);

    // tre risposte: catena completa, F1 da solo, F2 da solo
    std::vector<float> irTotal ((size_t) n * 2, 0.0f), irA ((size_t) n * 2, 0.0f), irB ((size_t) n * 2, 0.0f);
    {
        MultiFilter a1 = a, b1 = b;   // copie per le misure da soli
        for (int i = 0; i < n; ++i)
        {
            const float x = (i == 0) ? amp : 0.0f;
            float y;
            switch (s.routing)
            {
                case 1:  { const float ya = a.process (x), yb = b.process (x); y = ya + (yb - ya) * s.blend; break; }
                case 2:  { const float ya = a.process (x); y = ya + b.process (x - ya) * s.blend; break; }
                default: y = b.process (a.process (x)); break;
            }
            irTotal[(size_t) i] = y / amp;
            irA[(size_t) i] = a1.process (x) / amp;
            irB[(size_t) i] = b1.process (x) / amp;
        }
    }

    juce::dsp::FFT fft (order);
    auto toDb = [&] (std::vector<float>& ir, std::vector<float>& out)
    {
        fft.performFrequencyOnlyForwardTransform (ir.data());
        out.resize (hz.size());
        for (size_t i = 0; i < hz.size(); ++i)
        {
            const float bin = hz[i] / (float) fs * (float) n;
            const int k = jlimit (0, n / 2 - 1, (int) bin);
            const float t = bin - (float) k;
            const float m = ir[(size_t) k] + (ir[(size_t) k + 1] - ir[(size_t) k]) * t;
            out[i] = 20.0f * std::log10 (jmax (1.0e-6f, m));
        }
    };
    toDb (irTotal, totalDb);
    toDb (irA, f1Db);
    toDb (irB, f2Db);
}

void FilterResponse::paint (Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (0.5f);
    paintDisplayGlass (g, b);

    if (! valid || freqs.empty())
        return;

    constexpr float top = 24.0f, bottom = -48.0f;
    auto curve = [&] (const std::vector<float>& db)
    {
        Path p;
        for (size_t i = 0; i < freqs.size(); ++i)
        {
            const float x = b.getX() + std::log10 (freqs[i] / 20.0f) / 3.0f * b.getWidth();
            const float y = jmap (jlimit (bottom, top, db[i]), top, bottom, b.getY() + 4.0f, b.getBottom() - 4.0f);
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        return p;
    };

    // linea a 0 dB
    const float y0 = jmap (0.0f, top, bottom, b.getY() + 4.0f, b.getBottom() - 4.0f);
    g.setColour (Palette::dimText.withAlpha (0.25f));
    g.drawHorizontalLine ((int) y0, b.getX(), b.getRight());

    using Model = MultiFilter::Model;
    const bool f2Active = settings.m2 != (int) Model::Bypass;

    // i singoli filtri tratteggiati, la catena vera piena
    if (f2Active)
        strokeGlow (g, curve (f2), Palette::dimText, 1.0f);
    if (f2Active || settings.routing != 0)
        strokeGlow (g, curve (f1), Palette::orange.withAlpha (0.6f), 1.0f);
    strokeGlow (g, curve (total), Palette::lcdGreen);

    // marcatori del cutoff
    auto marker = [&] (float hz, Colour c)
    {
        const float x = b.getX() + std::log10 (jlimit (20.0f, 20000.0f, hz) / 20.0f) / 3.0f * b.getWidth();
        g.setColour (c.withAlpha (0.35f));
        g.drawVerticalLine ((int) x, b.getY() + 4.0f, b.getBottom() - 4.0f);
    };
    if (settings.m1 != (int) Model::Bypass) marker (settings.c1, Palette::orange);
    if (f2Active) marker (settings.c2, Palette::dimText);

    g.setFont (BassXLookAndFeel::getLcdFont (10.0f));
    g.setColour (Palette::dimText);
    const auto names = MultiFilter::getModelNames();
    const auto routings = StringArray { "SERIAL", "PARALLEL", "SPLIT" };
    g.drawText ("F1 " + names[settings.m1].toUpperCase()
                  + (f2Active ? "   F2 " + names[settings.m2].toUpperCase() : String())
                  + "   " + routings[jlimit (0, 2, settings.routing)],
                b.reduced (6.0f, 4.0f), Justification::topLeft, false);

    for (auto f : { 100.0f, 1000.0f, 10000.0f })
    {
        const float x = b.getX() + std::log10 (f / 20.0f) / 3.0f * b.getWidth();
        g.drawText (f >= 1000.0f ? String ((int) (f / 1000.0f)) + "k" : String ((int) f),
                    Rectangle<float> (x + 3.0f, b.getBottom() - 15.0f, 40.0f, 12.0f), Justification::left, false);
    }
}

//==============================================================================
// OscPage
//==============================================================================

OscPage::OscPage (AudioProcessorValueTreeState& state, BassXAudioProcessor& proc)
    : PageBase (state), wavetableBank (proc.getWavetableBank()), processor (proc)
{
    for (int o = 0; o < kNumOscillators; ++o)
    {
        auto& s = strips[(size_t) o];

        s.panel  = addModulePanel();
        s.header = addHeader ("Oscillator " + String (o + 1));

        s.enable = addToggle (ParamID::osc (o, "enable"), "ON");
        s.wave   = addCombo  (ParamID::osc (o, "wave"));

        s.display = displays.add (new WavetableDisplay());
        addAndMakeVisible (s.display);

        s.position  = addKnob (ParamID::osc (o, "position"),   "Pos");
        s.level     = addKnob (ParamID::osc (o, "level"),      "Level");
        s.pan       = addKnob (ParamID::osc (o, "pan"),        "Pan",    BassXLookAndFeel::KnobStyle::Dark);
        s.octave    = addKnob (ParamID::osc (o, "octave"),     "Oct",    BassXLookAndFeel::KnobStyle::Dark);
        s.semi      = addKnob (ParamID::osc (o, "semi"),       "Semi",   BassXLookAndFeel::KnobStyle::Dark);
        s.fine      = addKnob (ParamID::osc (o, "fine"),       "Fine",   BassXLookAndFeel::KnobStyle::Dark);
        s.fold      = addKnob (ParamID::osc (o, "fold"),       "Fold",   BassXLookAndFeel::KnobStyle::Orange);
        s.uniVoices = addKnob (ParamID::osc (o, "uni_voices"), "Unison", BassXLookAndFeel::KnobStyle::Dark);
        s.uniDetune = addKnob (ParamID::osc (o, "uni_detune"), "Detune");
        s.uniBlend  = addKnob (ParamID::osc (o, "uni_blend"),  "Blend",  BassXLookAndFeel::KnobStyle::Dark);
        s.uniSpread = addKnob (ParamID::osc (o, "uni_spread"), "Spread", BassXLookAndFeel::KnobStyle::Dark);
        s.phaseRand = addKnob (ParamID::osc (o, "phase_rand"), "Phase",  BassXLookAndFeel::KnobStyle::Dark);
    }

    extraPanel  = addModulePanel();
    extraHeader = addHeader ("Sub / Noise / Cross Modulation");

    addToggle (ParamID::subEnable, "SUB");
    addCombo  (ParamID::subShape);
    addKnob   (ParamID::subOctave, "Sub Oct", BassXLookAndFeel::KnobStyle::Dark);
    addKnob   (ParamID::subLevel,  "Sub Lvl");

    addToggle (ParamID::noiseEnable, "NOISE");
    addCombo  (ParamID::noiseColour);
    addKnob   (ParamID::noiseLevel, "Noise");

    addKnob   (ParamID::fmAmount,   "FM",   BassXLookAndFeel::KnobStyle::Orange);
    addCombo  (ParamID::fmSource);
    addCombo  (ParamID::fmTarget);
    addKnob   (ParamID::ringAmount, "Ring", BassXLookAndFeel::KnobStyle::Orange);
    addToggle (ParamID::syncEnable, "SYNC");

    // Primo aggiornamento immediato: senza, i display restano neri fino al
    // primo scatto del timer, cioè per un frame visibile all'apertura.
    timerCallback();

    startTimerHz (16);
}

void OscPage::timerCallback()
{
    for (int o = 0; o < kNumOscillators; ++o)
    {
        auto& s = strips[(size_t) o];

        if (s.display == nullptr)
            continue;

        const int table = (int) std::lround (
            apvts.getRawParameterValue (ParamID::osc (o, "wave"))->load());
        const float pos = apvts.getRawParameterValue (ParamID::osc (o, "position"))->load();

        if (table == kUserWaveIndex)
        {
            const auto* user = processor.getSampleWavetable();
            s.display->setSource (user != nullptr ? user : &wavetableBank.getTable (0), pos,
                                  processor.getSampleWavetableVersion(),
                                  user != nullptr ? processor.getSampleWavetableName() : String ("trascina un campione qui"));
        }
        else
            s.display->setSource (&wavetableBank.getTable (table), pos);
    }
}

int OscPage::stripAt (int x, int y) const
{
    for (int o = 0; o < kNumOscillators; ++o)
        if (strips[(size_t) o].panel != nullptr && strips[(size_t) o].panel->getBounds().contains (x, y))
            return o;
    return -1;
}

bool OscPage::isInterestedInFileDrag (const StringArray& files)
{
    for (auto& f : files)
        if (File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3"))
            return true;
    return false;
}

void OscPage::filesDropped (const StringArray& files, int x, int y)
{
    const int o = stripAt (x, y);
    dropTarget = -1;
    repaint();
    if (o < 0 || files.isEmpty())
        return;

    String error;
    if (! processor.loadSampleWavetable (File (files[0]), error))
    {
        NativeMessageBox::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "BASSX", error);
        return;
    }

    // l'oscillatore su cui e' caduto passa alla forma "Sample", acceso
    if (auto* w = apvts.getParameter (ParamID::osc (o, "wave")))
        w->setValueNotifyingHost (w->convertTo0to1 ((float) kUserWaveIndex));
    if (auto* e = apvts.getParameter (ParamID::osc (o, "enable")))
        e->setValueNotifyingHost (1.0f);
    if (o > 0)
        if (auto* l = apvts.getParameter (ParamID::osc (o, "level")); l != nullptr && l->getValue() < 0.05f)
            l->setValueNotifyingHost (l->convertTo0to1 (0.8f));
}

void OscPage::paintOverChildren (Graphics& g)
{
    if (dropTarget < 0)
        return;
    auto r = strips[(size_t) dropTarget].panel->getBounds().toFloat().reduced (2.0f);
    g.setColour (Palette::lcdGreen.withAlpha (0.15f));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (Palette::lcdGreen);
    g.drawRoundedRectangle (r, 4.0f, 2.0f);
    g.setFont (BassXLookAndFeel::getLcdFont (12.0f));
    g.drawText ("RILASCIA: CAMPIONE > WAVETABLE", r, Justification::centred, false);
}

void OscPage::resized()
{
    auto area = getLocalBounds().reduced (kPad);

    // Le tre strisce degli oscillatori occupano il 68% dell'altezza; il resto
    // va alla sezione sub/rumore/cross-mod.
    auto oscArea = area.removeFromTop (juce::roundToInt (area.getHeight() * 0.70f));
    area.removeFromTop (kRowGap);

    const int stripW = (oscArea.getWidth() - kRowGap * 2) / 3;

    for (int o = 0; o < kNumOscillators; ++o)
    {
        auto& s = strips[(size_t) o];

        auto strip = oscArea.removeFromLeft (stripW);

        if (o < kNumOscillators - 1)
            oscArea.removeFromLeft (kRowGap);

        s.panel->setBounds (strip);

        auto inner = strip.reduced (kPad, 6);

        auto headerRow = inner.removeFromTop (kHeaderH);
        s.enable->setBounds (headerRow.removeFromRight (40).reduced (0, 1));
        headerRow.removeFromRight (4);
        s.header->setBounds (headerRow);

        inner.removeFromTop (4);

        s.wave->setBounds (inner.removeFromTop (22));
        inner.removeFromTop (4);

        s.display->setBounds (inner.removeFromTop (juce::jmax (44, inner.getHeight() / 5)));
        inner.removeFromTop (4);

        const int rows = 3;
        const int rowH = juce::jmax (kKnobMinH, (inner.getHeight() - kRowGap * (rows - 1)) / rows);

        layoutRow ({ s.position, s.level, s.pan, s.fold },
                   inner.removeFromTop (rowH));
        inner.removeFromTop (kRowGap);

        layoutRow ({ s.octave, s.semi, s.fine, s.phaseRand },
                   inner.removeFromTop (rowH));
        inner.removeFromTop (kRowGap);

        layoutRow ({ s.uniVoices, s.uniDetune, s.uniBlend, s.uniSpread },
                   inner.removeFromTop (rowH));
    }

    // --- Sezione extra --------------------------------------------------------
    extraPanel->setBounds (area);

    auto inner = area.reduced (kPad, 6);
    extraHeader->setBounds (inner.removeFromTop (kHeaderH));
    inner.removeFromTop (4);

    // I controlli extra sono stati aggiunti in ordine noto: si recuperano dagli
    // array nell'ordine di creazione. È fragile se si riordina il costruttore,
    // ma evita quindici membri per una sezione di layout fisso.
    const int firstKnob = kNumOscillators * 12;
    const int firstButton = kNumOscillators;       // 3 toggle "ON"
    const int firstCombo = kNumOscillators;        // 3 combo "wave"

    auto* subOct   = knobs[firstKnob + 0];
    auto* subLvl   = knobs[firstKnob + 1];
    auto* noiseLvl = knobs[firstKnob + 2];
    auto* fmAmt    = knobs[firstKnob + 3];
    auto* ringAmt  = knobs[firstKnob + 4];

    auto* subOn    = buttons[firstButton + 0];
    auto* noiseOn  = buttons[firstButton + 1];
    auto* syncOn   = buttons[firstButton + 2];

    auto* subShape   = combos[firstCombo + 0];
    auto* noiseCol   = combos[firstCombo + 1];
    auto* fmSrc      = combos[firstCombo + 2];
    auto* fmTgt      = combos[firstCombo + 3];

    const int colW = inner.getWidth() / 3;

    // Colonna 1: sub
    {
        auto col = inner.removeFromLeft (colW).reduced (2, 0);
        auto top = col.removeFromTop (22);
        subOn->setBounds (top.removeFromLeft (52));
        top.removeFromLeft (4);
        subShape->setBounds (top);
        col.removeFromTop (4);
        layoutRow ({ subOct, subLvl }, col);
    }

    // Colonna 2: rumore
    {
        auto col = inner.removeFromLeft (colW).reduced (2, 0);
        auto top = col.removeFromTop (22);
        noiseOn->setBounds (top.removeFromLeft (62));
        top.removeFromLeft (4);
        noiseCol->setBounds (top);
        col.removeFromTop (4);
        layoutRow ({ noiseLvl }, col.removeFromLeft (col.getWidth() / 2));
    }

    // Colonna 3: FM / ring / sync
    {
        auto col = inner.reduced (2, 0);
        auto top = col.removeFromTop (22);
        fmSrc->setBounds (top.removeFromLeft (top.getWidth() / 3));
        top.removeFromLeft (4);
        fmTgt->setBounds (top.removeFromLeft (top.getWidth() / 2));
        top.removeFromLeft (4);
        syncOn->setBounds (top);
        col.removeFromTop (4);
        layoutRow ({ fmAmt, ringAmt }, col);
    }
}

//==============================================================================
// FilterPage
//==============================================================================

FilterPage::FilterPage (AudioProcessorValueTreeState& state)
    : PageBase (state)
{
    panel1 = addModulePanel();
    addHeader ("Filter 1");
    model1 = addCombo (ParamID::f1Model);
    addKnob (ParamID::f1Cutoff,    "Cutoff");
    addKnob (ParamID::f1Reso,      "Reso");
    addKnob (ParamID::f1Drive,     "Drive",   BassXLookAndFeel::KnobStyle::Orange);
    addKnob (ParamID::f1EnvAmount, "Env Amt");
    addKnob (ParamID::f1KeyTrack,  "Key Trk", BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::f1BassComp,  "Bass",    BassXLookAndFeel::KnobStyle::Dark);
    envSrc1 = addCombo (ParamID::f1EnvSource);

    panel2 = addModulePanel();
    addHeader ("Filter 2");
    model2 = addCombo (ParamID::f2Model);
    addKnob (ParamID::f2Cutoff,    "Cutoff");
    addKnob (ParamID::f2Reso,      "Reso");
    addKnob (ParamID::f2Drive,     "Drive",   BassXLookAndFeel::KnobStyle::Orange);
    addKnob (ParamID::f2EnvAmount, "Env Amt");
    addKnob (ParamID::f2KeyTrack,  "Key Trk", BassXLookAndFeel::KnobStyle::Dark);
    envSrc2 = addCombo (ParamID::f2EnvSource);

    panelRouting = addModulePanel();
    addHeader ("Routing");
    routing = addCombo (ParamID::filterRouting);
    addKnob (ParamID::filterBlend, "Blend");
    addKnob (ParamID::oversampling, "Quality", BassXLookAndFeel::KnobStyle::Dark);

    addCaption ("Diode = modello Bass Line. Serial: F1 > F2. "
                "Parallel: blend fra i due. Split: F2 riceve cio' che F1 scarta.");

    panelResponse = addModulePanel();
    addHeader ("Response", Palette::lcdGreen);
    addAndMakeVisible (response);
    startTimerHz (20);
}

void FilterPage::timerCallback()
{
    if (isShowing())
        updateResponse();
}

void FilterPage::updateResponse()
{
    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    FilterResponse::Settings st;
    st.m1 = (int) get (ParamID::f1Model);      st.m2 = (int) get (ParamID::f2Model);
    st.routing = (int) get (ParamID::filterRouting);
    st.c1 = get (ParamID::f1Cutoff);  st.r1 = get (ParamID::f1Reso);  st.d1 = get (ParamID::f1Drive);
    st.bass = get (ParamID::f1BassComp);
    st.c2 = get (ParamID::f2Cutoff);  st.r2 = get (ParamID::f2Reso);  st.d2 = get (ParamID::f2Drive);
    st.blend = get (ParamID::filterBlend);
    response.setSettings (st);
}

void FilterPage::resized()
{
    auto area = getLocalBounds().reduced (kPad);

    // Fila 1: i due filtri affiancati, un modulo standard ciascuno.
    auto row = area.removeFromTop (kModH);
    area.removeFromTop (kRowGap);

    auto filterModule = [this] (TexturedPanel* p, Rectangle<int> r, SectionHeader* h, ComboBox* model,
                                ComboBox* env, Array<Component*> row)
    {
        auto inner = placeModule (p, r);
        auto head = inner.removeFromTop (kHeaderH);
        h->setBounds (head.removeFromLeft (90));
        env->setBounds (head.removeFromRight (90).reduced (0, 1));
        head.removeFromRight (6);
        model->setBounds (head.removeFromRight (juce::jmin (150, head.getWidth())).reduced (0, 1));
        inner.removeFromTop (4);
        layoutRow (row, inner);
    };

    const int halfW = (row.getWidth() - kRowGap) / 2;
    filterModule (panel1, row.removeFromLeft (halfW), headers[0], model1, envSrc1,
                  { knobs[0], knobs[1], knobs[2], knobs[3], knobs[4], knobs[5] });
    row.removeFromLeft (kRowGap);
    filterModule (panel2, row, headers[1], model2, envSrc2,
                  { knobs[6], knobs[7], knobs[8], knobs[9], knobs[10] });

    // Fila 2: routing (colonna) + risposta in frequenza.
    {
        auto inner = placeModule (panelRouting, area.removeFromLeft (halfW / 2));
        headers[2]->setBounds (inner.removeFromTop (kHeaderH));
        inner.removeFromTop (4);
        routing->setBounds (inner.removeFromTop (22));
        inner.removeFromTop (6);
        layoutRow ({ knobs[11], knobs[12] }, inner.removeFromTop (kKnobRowH));
        inner.removeFromTop (6);
        labels[0]->setBounds (inner);
        labels[0]->setJustificationType (juce::Justification::topLeft);
        labels[0]->setFont (BassXLookAndFeel::getCondensed (10.5f, false));
    }

    area.removeFromLeft (kRowGap);
    {
        auto inner = placeModule (panelResponse, area);
        headers[3]->setBounds (inner.removeFromTop (kHeaderH));
        inner.removeFromTop (4);
        response.setBounds (inner);
    }

    updateResponse();
}

//==============================================================================
// EnvelopeGraph
//==============================================================================

void EnvelopeGraph::setEnvelope (float delayT, float attack, float hold, float decay,
                                 float sustain, float release,
                                 float curveA, float curveD, float curveR)
{
    if (std::abs (d - delayT) < 1e-5f && std::abs (a - attack) < 1e-5f
        && std::abs (h - hold) < 1e-5f && std::abs (dec - decay) < 1e-5f
        && std::abs (s - sustain) < 1e-5f && std::abs (r - release) < 1e-5f
        && std::abs (ca - curveA) < 1e-4f && std::abs (cd - curveD) < 1e-4f
        && std::abs (cr - curveR) < 1e-4f)
        return;

    d = delayT; a = attack; h = hold; dec = decay; s = sustain; r = release;
    ca = curveA; cd = curveD; cr = curveR;
    repaint();
}

void EnvelopeGraph::setLiveLevel (float level)
{
    if (std::abs (level - live) > 0.02f)
    {
        live = level;
        repaint();
    }
}

void EnvelopeGraph::paint (Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced (2.0f);

    paintDisplayGlass (g, area);

    auto plot = area.reduced (5.0f, 6.0f);

    // Il tempo totale mostrato è la somma dei segmenti più un margine di
    // sustain. Scalare ogni segmento sul totale, invece che su un asse
    // temporale fisso, è ciò che rende il grafico leggibile sia con un decay
    // di 5 ms sia con uno di 10 secondi.
    const float sustainShown = 0.18f;
    const float total = juce::jmax (0.001f, d + a + h + dec + r) / (1.0f - sustainShown);

    auto xFor = [&] (float t) { return plot.getX() + (t / total) * plot.getWidth(); };
    auto yFor = [&] (float v) { return plot.getBottom() - v * plot.getHeight(); };

    /** Curva esponenziale con la stessa forma usata dal DSP: si punta a un
        target oltre l'estremo e si taglia. Riprodurla qui invece di disegnare
        una retta è l'unico modo perché il grafico corrisponda a ciò che si
        sente. */
    auto curvedPoint = [] (float t, float curve, bool rising)
    {
        const float k = juce::jlimit (-0.98f, 0.98f, curve);

        if (std::abs (k) < 1e-3f)
            return rising ? t : 1.0f - t;

        const float shaped = (1.0f - std::exp (-t * (2.0f + k * 5.0f)))
                           / (1.0f - std::exp (-(2.0f + k * 5.0f)));

        return rising ? shaped : 1.0f - shaped;
    };

    Path path;
    float t = 0.0f;

    path.startNewSubPath (xFor (0.0f), yFor (0.0f));

    // Delay
    t += d;
    path.lineTo (xFor (t), yFor (0.0f));

    // Attack
    for (int i = 1; i <= 24; ++i)
    {
        const float u = (float) i / 24.0f;
        path.lineTo (xFor (t + a * u), yFor (curvedPoint (u, ca, true)));
    }
    t += a;

    // Hold
    t += h;
    path.lineTo (xFor (t), yFor (1.0f));

    // Decay
    for (int i = 1; i <= 24; ++i)
    {
        const float u = (float) i / 24.0f;
        const float v = s + (1.0f - s) * curvedPoint (u, cd, false);
        path.lineTo (xFor (t + dec * u), yFor (v));
    }
    t += dec;

    // Sustain
    const float sustainWidth = total * sustainShown;
    path.lineTo (xFor (t + sustainWidth), yFor (s));
    t += sustainWidth;

    // Release
    for (int i = 1; i <= 24; ++i)
    {
        const float u = (float) i / 24.0f;
        path.lineTo (xFor (t + r * u), yFor (s * curvedPoint (u, cr, false)));
    }

    // Riempimento sotto la curva
    Path filled (path);
    filled.lineTo (xFor (t + r), yFor (0.0f));
    filled.lineTo (xFor (0.0f), yFor (0.0f));
    filled.closeSubPath();

    g.setColour (Palette::orange.withAlpha (0.16f));
    g.fillPath (filled);

    g.setColour (Palette::orangeBright);
    g.strokePath (path, PathStrokeType (1.8f, PathStrokeType::curved, PathStrokeType::rounded));

    // Livello in tempo reale
    if (live > 0.005f)
    {
        g.setColour (Palette::lcdGreen.withAlpha (0.85f));
        const float y = yFor (live);
        g.drawLine (plot.getX(), y, plot.getRight(), y, 1.0f);
    }
}

//==============================================================================
// EnvPage
//==============================================================================

EnvPage::EnvPage (AudioProcessorValueTreeState& state, BassXAudioProcessor& proc)
    : PageBase (state), processor (proc)
{
    static const char* names[kNumEnvelopes] =
    {
        "Env 1  >  Amplitude", "Env 2  >  Filter", "Env 3  >  Free", "Env 4  >  Free"
    };

    for (int e = 0; e < kNumEnvelopes; ++e)
    {
        auto& s = strips[(size_t) e];

        s.panel  = addModulePanel();
        s.header = addHeader (names[e]);

        s.graph = graphs.add (new EnvelopeGraph());
        addAndMakeVisible (s.graph);

        s.controls[0] = addKnob (ParamID::env (e, "delay"),   "Delay",  BassXLookAndFeel::KnobStyle::Dark);
        s.controls[1] = addKnob (ParamID::env (e, "attack"),  "Attack");
        s.controls[2] = addKnob (ParamID::env (e, "hold"),    "Hold",   BassXLookAndFeel::KnobStyle::Dark);
        s.controls[3] = addKnob (ParamID::env (e, "decay"),   "Decay");
        s.controls[4] = addKnob (ParamID::env (e, "sustain"), "Sustain");
        s.controls[5] = addKnob (ParamID::env (e, "release"), "Release");
        s.controls[6] = addKnob (ParamID::env (e, "curve_a"), "Crv A",  BassXLookAndFeel::KnobStyle::Orange);
        s.controls[7] = addKnob (ParamID::env (e, "curve_d"), "Crv D",  BassXLookAndFeel::KnobStyle::Orange);
        s.controls[8] = addKnob (ParamID::env (e, "curve_r"), "Crv R",  BassXLookAndFeel::KnobStyle::Orange);
        s.controls[9] = addKnob (ParamID::env (e, "vel"),     "Vel",    BassXLookAndFeel::KnobStyle::Dark);
    }

    startTimerHz (24);
}

void EnvPage::timerCallback()
{
    refreshGraphs();

    // Solo i primi due inviluppi hanno un livello pubblicato dal motore: sono
    // quelli che contano visivamente (ampiezza e filtro).
    if (strips[0].graph != nullptr)
        strips[0].graph->setLiveLevel (processor.meters.ampEnvLevel.load());

    if (strips[1].graph != nullptr)
        strips[1].graph->setLiveLevel (processor.meters.filterEnvLevel.load());
}

void EnvPage::refreshGraphs()
{
    for (int e = 0; e < kNumEnvelopes; ++e)
    {
        auto get = [this, e] (const char* suffix)
        {
            return apvts.getRawParameterValue (ParamID::env (e, suffix))->load();
        };

        if (strips[(size_t) e].graph != nullptr)
            strips[(size_t) e].graph->setEnvelope (get ("delay"), get ("attack"), get ("hold"),
                                                   get ("decay"), get ("sustain"), get ("release"),
                                                   get ("curve_a"), get ("curve_d"), get ("curve_r"));
    }
}

void EnvPage::resized()
{
    auto area = getLocalBounds().reduced (kPad);

    const int stripH = (area.getHeight() - kRowGap * (kNumEnvelopes - 1)) / kNumEnvelopes;

    for (int e = 0; e < kNumEnvelopes; ++e)
    {
        auto& s = strips[(size_t) e];

        auto strip = area.removeFromTop (stripH);

        if (e < kNumEnvelopes - 1)
            area.removeFromTop (kRowGap);

        s.panel->setBounds (strip);

        auto inner = strip.reduced (kPad, 5);

        auto head = inner.removeFromTop (kHeaderH);
        s.header->setBounds (head.removeFromLeft (160));

        inner.removeFromTop (2);

        const int graphW = juce::jlimit (120, 260, inner.getWidth() / 5);
        s.graph->setBounds (inner.removeFromLeft (graphW));
        inner.removeFromLeft (kRowGap);

        layoutRow ({ s.controls[0], s.controls[1], s.controls[2], s.controls[3], s.controls[4],
                     s.controls[5], s.controls[6], s.controls[7], s.controls[8], s.controls[9] },
                   inner);
    }
}

//==============================================================================
// LfoScope
//==============================================================================

void LfoScope::setShape (int shapeIndex, float phaseOffset, bool unipolar)
{
    if (shape == shapeIndex && std::abs (phase - phaseOffset) < 0.004f && uni == unipolar)
        return;

    shape = shapeIndex;
    phase = phaseOffset;
    uni = unipolar;
    repaint();
}

void LfoScope::paint (Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced (2.0f);

    paintDisplayGlass (g, area);

    auto plot = area.reduced (4.0f, 5.0f);

    g.setColour (Colour (0x14ffffff));
    g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());

    // Le forme replicano quelle del DSP. Le due casuali sono disegnate con un
    // seme fisso: mostrare rumore diverso a ogni repaint renderebbe il grafico
    // illeggibile e non aggiungerebbe informazione.
    Xorshift rng { 0x51ED270Bu };
    float shValue = rng.nextBipolar();
    float randPrev = shValue, randNext = rng.nextBipolar();
    float chaos = 0.31f;

    Path path;
    const int points = juce::jmax (32, (int) plot.getWidth());

    for (int i = 0; i < points; ++i)
    {
        const float t = (float) i / (float) (points - 1);
        const float p = wrap01 (t * 2.0f + phase);       // due cicli
        float v = 0.0f;

        switch ((LFO::Shape) shape)
        {
            case LFO::Shape::Sine:        v = std::sin (kTwoPi * p); break;
            case LFO::Shape::Triangle:    v = 4.0f * std::abs (p - 0.5f) - 1.0f; break;
            case LFO::Shape::SawUp:       v = 2.0f * p - 1.0f; break;
            case LFO::Shape::SawDown:     v = 1.0f - 2.0f * p; break;
            case LFO::Shape::Square:      v = p < 0.5f ? 1.0f : -1.0f; break;
            case LFO::Shape::Pulse25:     v = p < 0.25f ? 1.0f : -1.0f; break;

            case LFO::Shape::SampleHold:
                if (i > 0 && p < wrap01 ((float) (i - 1) / (float) (points - 1) * 2.0f + phase))
                    shValue = rng.nextBipolar();
                v = shValue;
                break;

            case LFO::Shape::SmoothRandom:
            {
                if (i > 0 && p < wrap01 ((float) (i - 1) / (float) (points - 1) * 2.0f + phase))
                {
                    randPrev = randNext;
                    randNext = rng.nextBipolar();
                }
                const float u = 0.5f - 0.5f * std::cos (kPi * p);
                v = lerp (randPrev, randNext, u);
                break;
            }

            case LFO::Shape::ExpRamp:     v = 2.0f * std::exp (-4.0f * p) - 1.0f; break;

            case LFO::Shape::Chaos:
                if (i > 0 && p < wrap01 ((float) (i - 1) / (float) (points - 1) * 2.0f + phase))
                    chaos = 3.95f * chaos * (1.0f - chaos);
                v = chaos * 2.0f - 1.0f;
                break;

            case LFO::Shape::NumShapes:
                break;
        }

        if (uni)
            v = v * 0.5f + 0.5f;

        const float x = plot.getX() + t * plot.getWidth();
        const float y = plot.getCentreY() - v * plot.getHeight() * (uni ? 0.44f : 0.44f)
                      + (uni ? plot.getHeight() * 0.22f : 0.0f);

        if (i == 0) path.startNewSubPath (x, y);
        else        path.lineTo (x, y);
    }

    g.setColour (Palette::lcdGreen);
    g.strokePath (path, PathStrokeType (1.6f));
}

//==============================================================================
// LfoPage
//==============================================================================

LfoPage::LfoPage (AudioProcessorValueTreeState& state)
    : PageBase (state)
{
    for (int l = 0; l < kNumLFOs; ++l)
    {
        auto& s = strips[(size_t) l];

        s.panel  = addModulePanel();
        s.header = addHeader ("LFO " + String (l + 1));

        s.scope = scopes.add (new LfoScope());
        addAndMakeVisible (s.scope);

        s.shape    = addCombo  (ParamID::lfo (l, "shape"));
        s.division = addCombo  (ParamID::lfo (l, "division"));
        s.trigger  = addCombo  (ParamID::lfo (l, "trigger"));
        s.sync     = addToggle (ParamID::lfo (l, "sync"), "SYNC");
        s.unipolar = addToggle (ParamID::lfo (l, "unipolar"), "UNI");

        s.controls[0] = addKnob (ParamID::lfo (l, "rate"),   "Rate");
        s.controls[1] = addKnob (ParamID::lfo (l, "depth"),  "Depth");
        s.controls[2] = addKnob (ParamID::lfo (l, "phase"),  "Phase",  BassXLookAndFeel::KnobStyle::Dark);
        s.controls[3] = addKnob (ParamID::lfo (l, "fade"),   "Fade",   BassXLookAndFeel::KnobStyle::Dark);
        s.controls[4] = addKnob (ParamID::lfo (l, "smooth"), "Smooth", BassXLookAndFeel::KnobStyle::Dark);
    }

    startTimerHz (12);
}

void LfoPage::timerCallback()
{
    for (int l = 0; l < kNumLFOs; ++l)
    {
        auto& s = strips[(size_t) l];

        if (s.scope == nullptr)
            continue;

        s.scope->setShape ((int) std::lround (apvts.getRawParameterValue (ParamID::lfo (l, "shape"))->load()),
                           apvts.getRawParameterValue (ParamID::lfo (l, "phase"))->load(),
                           apvts.getRawParameterValue (ParamID::lfo (l, "unipolar"))->load() > 0.5f);
    }
}

void LfoPage::resized()
{
    auto area = getLocalBounds().reduced (kPad);

    const int cols = 2;
    const int rows = 2;
    const int cellW = (area.getWidth() - kRowGap) / cols;
    const int cellH = (area.getHeight() - kRowGap) / rows;

    for (int l = 0; l < kNumLFOs; ++l)
    {
        auto& s = strips[(size_t) l];

        const int col = l % cols;
        const int row = l / cols;

        juce::Rectangle<int> cell (area.getX() + col * (cellW + kRowGap),
                                   area.getY() + row * (cellH + kRowGap),
                                   cellW, cellH);

        s.panel->setBounds (cell);

        auto inner = cell.reduced (kPad, 6);

        auto head = inner.removeFromTop (kHeaderH);
        s.header->setBounds (head.removeFromLeft (60));
        head.removeFromLeft (4);
        s.unipolar->setBounds (head.removeFromRight (44).reduced (0, 1));
        head.removeFromRight (4);
        s.sync->setBounds (head.removeFromRight (50).reduced (0, 1));
        head.removeFromRight (4);
        s.trigger->setBounds (head.reduced (0, 1));

        inner.removeFromTop (4);

        auto comboRow = inner.removeFromTop (22);
        s.shape->setBounds (comboRow.removeFromLeft (comboRow.getWidth() * 3 / 5));
        comboRow.removeFromLeft (4);
        s.division->setBounds (comboRow);

        inner.removeFromTop (4);

        s.scope->setBounds (inner.removeFromTop (juce::jmax (40, inner.getHeight() * 2 / 5)));
        inner.removeFromTop (4);

        layoutRow ({ s.controls[0], s.controls[1], s.controls[2], s.controls[3], s.controls[4] }, inner);
    }
}

//==============================================================================
// ModMatrixPage
//==============================================================================

ModMatrixPage::ModMatrixPage (AudioProcessorValueTreeState& state, ModMatrix& matrix)
    : PageBase (state), modMatrix (matrix)
{
    addHeader ("Modulation Matrix");
    addCaption ("Sorgente  >  Destinazione, con profondita' e sorgente ausiliaria (VIA) "
                "che scala la profondita'.");

    const auto sourceNames = getModSourceNames();
    const auto destNames   = getModDestNames();

    for (int i = 0; i < kNumModSlots; ++i)
    {
        auto& row = rows[(size_t) i];

        row.index.setText (String (i + 1), dontSendNotification);
        row.index.setJustificationType (Justification::centredRight);
        row.index.setFont (BassXLookAndFeel::getCondensed (11.0f, false));
        row.index.setColour (Label::textColourId, Palette::dimText);
        addAndMakeVisible (row.index);

        row.source.addItemList (sourceNames, 1);
        row.via.addItemList (sourceNames, 1);
        row.dest.addItemList (destNames, 1);

        addAndMakeVisible (row.source);
        addAndMakeVisible (row.via);
        addAndMakeVisible (row.dest);

        row.depth.setSliderStyle (Slider::LinearHorizontal);
        row.depth.setRange (-1.0, 1.0, 0.001);
        row.depth.setTextBoxStyle (Slider::TextBoxRight, false, 52, 18);
        row.depth.setDoubleClickReturnValue (true, 0.0);
        addAndMakeVisible (row.depth);

        auto push = [this, i] { pushRowToMatrix (i); };

        row.source.onChange = push;
        row.via.onChange = push;
        row.dest.onChange = push;
        row.depth.onValueChange = push;
    }

    refreshFromMatrix();
    startTimerHz (4);
}

void ModMatrixPage::timerCallback()
{
    // La matrice può cambiare sotto i piedi della GUI quando si carica un
    // preset. Un polling a 4 Hz è più semplice e più robusto di un sistema di
    // notifiche, e a questa frequenza il costo è nullo.
    refreshFromMatrix();
}

void ModMatrixPage::refreshFromMatrix()
{
    if (updating)
        return;

    const ScopedValueSetter<bool> guard (updating, true);

    for (int i = 0; i < kNumModSlots; ++i)
    {
        const auto& slot = modMatrix.getSlot (i);
        auto& row = rows[(size_t) i];

        const int src  = (int) slot.source;
        const int via  = (int) slot.via;
        const int dest = (int) slot.dest;

        if (row.source.getSelectedItemIndex() != src)
            row.source.setSelectedItemIndex (src, dontSendNotification);

        if (row.via.getSelectedItemIndex() != via)
            row.via.setSelectedItemIndex (via, dontSendNotification);

        if (row.dest.getSelectedItemIndex() != dest)
            row.dest.setSelectedItemIndex (dest, dontSendNotification);

        if (std::abs ((float) row.depth.getValue() - slot.depth) > 1.0e-4f)
            row.depth.setValue (slot.depth, dontSendNotification);
    }
}

void ModMatrixPage::pushRowToMatrix (int i)
{
    if (updating)
        return;

    auto& row = rows[(size_t) i];

    modMatrix.setSlot (i,
                       (ModSource) jlimit (0, (int) ModSource::NumSources - 1, row.source.getSelectedItemIndex()),
                       (ModDest)   jlimit (0, (int) ModDest::NumDests - 1,   row.dest.getSelectedItemIndex()),
                       (float) row.depth.getValue(),
                       (ModSource) jlimit (0, (int) ModSource::NumSources - 1, row.via.getSelectedItemIndex()));
}

void ModMatrixPage::resized()
{
    auto area = getLocalBounds().reduced (kPad);

    headers[0]->setBounds (area.removeFromTop (kHeaderH));
    labels[0]->setBounds (area.removeFromTop (16));
    area.removeFromTop (4);

    const int rowH = juce::jmax (20, area.getHeight() / kNumModSlots);

    for (int i = 0; i < kNumModSlots; ++i)
    {
        auto row = area.removeFromTop (rowH).reduced (0, 1);
        auto& r = rows[(size_t) i];

        r.index.setBounds (row.removeFromLeft (22));
        row.removeFromLeft (4);
        r.source.setBounds (row.removeFromLeft (juce::jlimit (90, 150, row.getWidth() / 5)));
        row.removeFromLeft (4);
        r.via.setBounds (row.removeFromLeft (juce::jlimit (80, 130, row.getWidth() / 5)));
        row.removeFromLeft (4);
        r.dest.setBounds (row.removeFromLeft (juce::jlimit (100, 170, row.getWidth() / 3)));
        row.removeFromLeft (6);
        r.depth.setBounds (row);
    }
}

//==============================================================================
// SeqPage
//==============================================================================

//==============================================================================
// Export del loop
//==============================================================================

void DragFileTile::paint (Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (0.5f);
    paintDisplayGlass (g, b);
    g.setFont (BassXLookAndFeel::getLcdFont (10.0f));
    g.setColour (file.existsAsFile() ? Palette::lcdGreen : Palette::dimText);
    g.drawFittedText ((file.existsAsFile() ? String ("TRASCINA NELLA DAW >  ") + file.getFileName() : status).toUpperCase(),
                      getLocalBounds().reduced (6, 2), Justification::centredLeft, 2);
}

void DragFileTile::mouseDrag (const MouseEvent&)
{
    if (! dragging && file.existsAsFile())
    {
        dragging = true;
        DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                              [this] { dragging = false; });
    }
}

void DragFileTile::mouseUp (const MouseEvent& e)
{
    if (! e.mouseWasDraggedSinceMouseDown() && file.existsAsFile())
        file.revealToUser();
}

void SeqPage::startExport()
{
    const int bars = jmax (1, exportBars.getText().getIntValue());
    const auto dir = File::getSpecialLocation (File::userMusicDirectory).getChildFile ("BASSX Loops");
    dir.createDirectory();
    const auto file = dir.getNonexistentChildFile ("BASSX_" + String (roundToInt (processor.getLastBpm())) + "bpm_"
                                                     + String (bars) + "bar_" + Time::getCurrentTime().formatted ("%H%M%S"), ".wav");
    exportButton.setEnabled (false);
    exportTile.setFile ({});
    exportTile.setStatus ("RENDER IN CORSO...");

    auto job = std::make_shared<BassXAudioProcessor::LoopExportJob> (processor.makeLoopExportJob());
    SafePointer<SeqPage> safe (this);
    Thread::launch ([job, file, bars, safe]
    {
        String error;
        const bool ok = BassXAudioProcessor::exportLoop (*job, file, bars, error);
        MessageManager::callAsync ([safe, ok, error, file]
        {
            if (safe == nullptr) return;
            safe->exportButton.setEnabled (true);
            if (ok) safe->exportTile.setFile (file);
            else  { safe->exportTile.setStatus ("ERRORE"); NativeMessageBox::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "BASSX", error); }
        });
    });
}

SeqPage::SeqPage (AudioProcessorValueTreeState& state, BassXAudioProcessor& proc)
    : PageBase (state), processor (proc)
{
    controlPanel = addModulePanel();
    addHeader ("Step Sequencer");

    addToggle (ParamID::seqEnable, "RUN");
    division = addCombo (ParamID::seqDivision);
    direction = addCombo (ParamID::seqDirection);
    addToggle (ParamID::seqKeyFollow, "KEY");

    addKnob (ParamID::seqPattern,    "Pattern",  BassXLookAndFeel::KnobStyle::Orange);
    addKnob (ParamID::seqLength,     "Length",   BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::seqSwing,      "Swing");
    addKnob (ParamID::seqOctave,     "Octave",   BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::seqGateLength, "Gate");

    addCaption ("KEY: il tasto traspone. xN ratchet, barra verde = probabilita'.");

    exportBars.addItemList ({ "1 bar", "2 bars", "4 bars", "8 bars", "16 bars" }, 1);
    exportBars.setSelectedId (3, dontSendNotification);
    exportBars.setTooltip ("Lunghezza del loop esportato");
    addAndMakeVisible (exportBars);
    exportButton.setTooltip ("Registra in WAV il loop che SEQ/ARP stanno suonando, al BPM della DAW. "
                             "Il file finisce in Musica/BASSX Loops e si trascina nella DAW dal riquadro accanto.");
    exportButton.onClick = [this] { startExport(); };
    addAndMakeVisible (exportButton);
    addAndMakeVisible (exportTile);

    for (int i = 0; i < kNumSeqSteps; ++i)
    {
        auto& col = steps[(size_t) i];

        col.number.setText (String (i + 1), dontSendNotification);
        col.number.setJustificationType (Justification::centred);
        col.number.setFont (BassXLookAndFeel::getCondensed (10.0f, true));
        col.number.setColour (Label::textColourId, Palette::dimText);
        addAndMakeVisible (col.number);

        addAndMakeVisible (col.led);

        col.note.setSliderStyle (Slider::LinearBarVertical);
        col.note.setRange (-24.0, 24.0, 1.0);
        col.note.setTextBoxStyle (Slider::TextBoxBelow, false, 34, 15);
        col.note.setDoubleClickReturnValue (true, 0.0);
        col.note.setColour (Slider::trackColourId, Palette::orange.withAlpha (0.55f));
        col.note.setColour (Slider::backgroundColourId, Palette::bg0);
        col.note.setColour (Slider::textBoxTextColourId, Palette::lcdGreen);
        col.note.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
        col.note.setColour (Slider::textBoxBackgroundColourId, Palette::bg0);
        addAndMakeVisible (col.note);

        for (auto* b : { &col.gate, &col.accent, &col.slide, &col.tie })
        {
            b->setClickingTogglesState (true);
            addAndMakeVisible (*b);
        }

        addAndMakeVisible (col.ratchet);

        col.probability.setSliderStyle (Slider::LinearBar);
        col.probability.setRange (0.0, 100.0, 1.0);
        col.probability.setTextValueSuffix ("%");
        col.probability.setDoubleClickReturnValue (true, 100.0);
        col.probability.setColour (Slider::trackColourId, Palette::lcdGreen.withAlpha (0.45f));
        col.probability.setColour (Slider::backgroundColourId, Palette::bg0);
        col.probability.setColour (Slider::textBoxTextColourId, Palette::dimText);
        col.probability.setTooltip ("Probabilita' del passo");
        addAndMakeVisible (col.probability);

        auto push = [this, i]
        {
            if (updating)
                return;

            const int pattern = jlimit (0, kNumSeqPatterns - 1,
                (int) std::lround (apvts.getRawParameterValue (ParamID::seqPattern)->load()) - 1);

            auto& step = processor.sequencer.getStep (pattern, i);
            auto& c = steps[(size_t) i];

            step.note     = (int) c.note.getValue();
            step.gate     = c.gate.getToggleState();
            step.accent   = c.accent.getToggleState();
            step.slide    = c.slide.getToggleState();
            step.tie      = c.tie.getToggleState();
            step.velocity = step.accent ? 1.0f : 0.75f;
            step.ratchet  = c.ratchetValue;
            step.probability = (float) c.probability.getValue() / 100.0f;
        };

        col.note.onValueChange = push;
        col.gate.onClick = push;
        col.accent.onClick = push;
        col.slide.onClick = push;
        col.tie.onClick = push;
        col.probability.onValueChange = push;
        col.ratchet.onClick = [this, i, push]
        {
            auto& c = steps[(size_t) i];
            c.ratchetValue = c.ratchetValue % 4 + 1;
            c.ratchet.setButtonText ("x" + String (c.ratchetValue));
            c.ratchet.setToggleState (c.ratchetValue > 1, dontSendNotification);
            push();
        };
    }

    rebuildFromPattern();
    startTimerHz (24);
}

SeqPage::~SeqPage() = default;

void SeqPage::timerCallback()
{
    const int pattern = jlimit (0, kNumSeqPatterns - 1,
        (int) std::lround (apvts.getRawParameterValue (ParamID::seqPattern)->load()) - 1);

    if (pattern != lastPattern)
        rebuildFromPattern();

    const int current = processor.meters.currentStep.load();
    const bool running = apvts.getRawParameterValue (ParamID::seqEnable)->load() > 0.5f;

    for (int i = 0; i < kNumSeqSteps; ++i)
        steps[(size_t) i].led.setLevel ((running && i == current) ? 1.0f : 0.0f);
}

void SeqPage::rebuildFromPattern()
{
    const int pattern = jlimit (0, kNumSeqPatterns - 1,
        (int) std::lround (apvts.getRawParameterValue (ParamID::seqPattern)->load()) - 1);

    lastPattern = pattern;

    const ScopedValueSetter<bool> guard (updating, true);

    for (int i = 0; i < kNumSeqSteps; ++i)
    {
        const auto& step = processor.sequencer.getStep (pattern, i);
        auto& c = steps[(size_t) i];

        c.note.setValue (step.note, dontSendNotification);
        c.gate.setToggleState (step.gate, dontSendNotification);
        c.accent.setToggleState (step.accent, dontSendNotification);
        c.slide.setToggleState (step.slide, dontSendNotification);
        c.tie.setToggleState (step.tie, dontSendNotification);
        c.ratchetValue = jlimit (1, 4, step.ratchet);
        c.ratchet.setButtonText ("x" + String (c.ratchetValue));
        c.ratchet.setToggleState (c.ratchetValue > 1, dontSendNotification);
        c.probability.setValue (std::round (step.probability * 100.0f), dontSendNotification);
    }
}

void SeqPage::resized()
{
    auto area = getLocalBounds().reduced (kPad);

    // --- Barra dei controlli --------------------------------------------------
    auto control = area.removeFromTop (86);
    controlPanel->setBounds (control);

    auto inner = control.reduced (kPad, 6);
    auto head = inner.removeFromTop (kHeaderH);
    headers[0]->setBounds (head.removeFromLeft (140));
    head.removeFromLeft (6);
    buttons[0]->setBounds (head.removeFromLeft (54).reduced (0, 1));
    head.removeFromLeft (6);
    division->setBounds (head.removeFromLeft (juce::jmin (90, head.getWidth())).reduced (0, 1));
    head.removeFromLeft (6);
    direction->setBounds (head.removeFromLeft (juce::jmin (96, head.getWidth())).reduced (0, 1));
    head.removeFromLeft (6);
    buttons[1]->setBounds (head.removeFromLeft (juce::jmin (44, head.getWidth())).reduced (0, 1));
    head.removeFromLeft (8);
    labels[0]->setBounds (head);
    labels[0]->setFont (BassXLookAndFeel::getCondensed (10.0f, false));

    inner.removeFromTop (2);
    layoutRow ({ knobs[0], knobs[1], knobs[2], knobs[3], knobs[4] },
               inner.removeFromLeft (juce::jmin (inner.getWidth(), 420)));

    // export: a destra della fila di knob
    {
        auto ex = inner.reduced (8, 10);
        auto top = ex.removeFromTop (24);
        exportBars.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (6);
        exportButton.setBounds (top.removeFromLeft (110));
        ex.removeFromTop (4);
        exportTile.setBounds (ex.removeFromTop (22).withWidth (jmin (ex.getWidth(), 420)));
    }

    area.removeFromTop (kRowGap);

    // --- Griglia dei passi ----------------------------------------------------
    const int colW = area.getWidth() / kNumSeqSteps;

    for (int i = 0; i < kNumSeqSteps; ++i)
    {
        auto col = juce::Rectangle<int> (area.getX() + i * colW, area.getY(),
                                         colW, area.getHeight()).reduced (2, 0);
        auto& c = steps[(size_t) i];

        auto top = col.removeFromTop (16);
        c.number.setBounds (top.removeFromLeft (top.getWidth() - 14));
        c.led.setBounds (top.withSizeKeepingCentre (12, 12));

        const int buttonH = juce::jlimit (15, 21, col.getHeight() / 9);

        auto buttonArea = col.removeFromBottom (buttonH * 6 + 6);

        c.note.setBounds (col.reduced (1, 2));

        c.gate.setBounds (buttonArea.removeFromTop (buttonH).reduced (1, 1));
        c.accent.setBounds (buttonArea.removeFromTop (buttonH).reduced (1, 1));
        c.slide.setBounds (buttonArea.removeFromTop (buttonH).reduced (1, 1));
        c.tie.setBounds (buttonArea.removeFromTop (buttonH).reduced (1, 1));
        c.ratchet.setBounds (buttonArea.removeFromTop (buttonH).reduced (1, 1));
        c.probability.setBounds (buttonArea.removeFromTop (buttonH).reduced (1, 1));
    }
}

//==============================================================================
// FxPage
//==============================================================================

FxPage::FxPage (AudioProcessorValueTreeState& state)
    : PageBase (state)
{
    // --- Catena: un effetto per slot, nell'ordine di elaborazione ----------------
    routePanel = addModulePanel();

    for (int slot = 0; slot < kNumFxSlots; ++slot)
        slotBoxes[(size_t) slot] = addCombo (ParamID::fxSlot (slot));

    resetChain.onClick = [this]
    {
        for (int slot = 0; slot < kNumFxSlots; ++slot)
            if (auto* p = apvts.getParameter (ParamID::fxSlot (slot)))
                p->setValueNotifyingHost (p->convertTo0to1 ((float) slot));
    };
    resetChain.setTooltip ("Ripristina l'ordine di default della catena");
    addAndMakeVisible (resetChain);

    // --- Distorsione ------------------------------------------------------------
    distPanel = addModulePanel();
    addHeader ("Distortion");
    addToggle (ParamID::distOn, "ON");
    distAlgo = addCombo (ParamID::distAlgo);
    addKnob (ParamID::distDrive,      "Drive", BassXLookAndFeel::KnobStyle::Orange);
    addKnob (ParamID::distTone,       "Tone");
    addKnob (ParamID::distMix,        "Mix");
    addKnob (ParamID::distBits,       "Bits",  BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::distDownsample, "S.Rate", BassXLookAndFeel::KnobStyle::Dark);

    // --- EQ ---------------------------------------------------------------------
    eqPanel = addModulePanel();
    addHeader ("Equaliser");
    addKnob (ParamID::eqLowFreq,  "Lo Frq", BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::eqLowGain,  "Lo Gain");
    addKnob (ParamID::eqMidFreq,  "Mid Frq", BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::eqMidGain,  "Mid Gain");
    addKnob (ParamID::eqMidQ,     "Mid Q",   BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::eqHighFreq, "Hi Frq",  BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::eqHighGain, "Hi Gain");

    // --- Chorus / Phaser ---------------------------------------------------------
    modPanel = addModulePanel();
    addHeader ("Chorus / Phaser");
    addToggle (ParamID::chorusOn, "CHO");
    addKnob (ParamID::chorusRate,   "Rate");
    addKnob (ParamID::chorusDepth,  "Depth");
    addKnob (ParamID::chorusSpread, "Spread", BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::chorusMix,    "Mix");
    addToggle (ParamID::phaserOn, "PHA");
    addKnob (ParamID::phaserRate,     "Rate");
    addKnob (ParamID::phaserDepth,    "Depth");
    addKnob (ParamID::phaserFreq,     "Centre",   BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::phaserFeedback, "Feedback", BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::phaserMix,      "Mix");

    // --- Delay --------------------------------------------------------------------
    delayPanel = addModulePanel();
    addHeader ("Delay");
    addToggle (ParamID::delayOn, "ON");
    addToggle (ParamID::delaySync, "SYNC");
    addToggle (ParamID::delayPingPong, "P-P");
    delayDivL = addCombo (ParamID::delayDivL);
    delayDivR = addCombo (ParamID::delayDivR);
    addKnob (ParamID::delayFeedback, "Feedbk");
    addKnob (ParamID::delayMix,      "Mix");
    addKnob (ParamID::delayLowpass,  "LPF", BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::delayHighpass, "HPF", BassXLookAndFeel::KnobStyle::Dark);

    // --- Riverbero -----------------------------------------------------------------
    reverbPanel = addModulePanel();
    addHeader ("Reverb");
    addToggle (ParamID::reverbOn, "ON");
    addKnob (ParamID::reverbSize,     "Size");
    addKnob (ParamID::reverbDecay,    "Decay");
    addKnob (ParamID::reverbDamping,  "Damp",   BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::reverbLowCut,   "Lo Cut", BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::reverbPreDelay, "Pre",    BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::reverbWidth,    "Width",  BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::reverbMix,      "Mix");

    // --- Dinamica --------------------------------------------------------------------
    dynPanel = addModulePanel();
    addHeader ("Comp / Limit");
    addToggle (ParamID::compOn, "COMP");
    addToggle (ParamID::compRms, "RMS");
    addToggle (ParamID::limiterOn, "LIM");
    addKnob (ParamID::compThreshold, "Thresh");
    addKnob (ParamID::compRatio,     "Ratio");
    addKnob (ParamID::compAttack,    "Attack",  BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::compRelease,   "Release", BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::compKnee,      "Knee",    BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::compMakeup,    "Makeup");
    addKnob (ParamID::compMix,       "Mix",     BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::limiterCeiling, "Ceiling", BassXLookAndFeel::KnobStyle::Orange);

    // --- Glitch / Beat Repeat ---------------------------------------------------------
    glitchPanel = addModulePanel();
    addHeader ("Glitch");
    addToggle (ParamID::glitchOn, "ON");
    glitchRate = addCombo (ParamID::glitchRate);
    glitchMode = addCombo (ParamID::glitchMode);
    addKnob (ParamID::glitchAmount, "Amount", BassXLookAndFeel::KnobStyle::Orange);
    addKnob (ParamID::glitchSeed,   "Seed",   BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::glitchMix,    "Mix");

    repeatPanel = addModulePanel();
    addHeader ("Beat Repeat");
    addToggle (ParamID::brepOn, "ON");
    repInterval = addCombo (ParamID::brepInterval);
    repGrid     = addCombo (ParamID::brepGrid);
    repGate     = addCombo (ParamID::brepGate);
    addKnob (ParamID::brepChance, "Chance", BassXLookAndFeel::KnobStyle::Orange);
    addKnob (ParamID::brepDecay,  "Decay",  BassXLookAndFeel::KnobStyle::Dark);
    addKnob (ParamID::brepMix,    "Mix");
    for (auto* c : { repInterval, repGrid, repGate })
        c->setTooltip ("Interval: ogni quanto cattura / Grid: lunghezza ripetuta / Gate: quanto dura il rullo");
}

void FxPage::paint (Graphics& g)
{
    // Frecce fra gli slot della catena: rendono leggibile il verso del segnale.
    g.setColour (Palette::orange.withAlpha (0.85f));

    for (int slot = 0; slot < kNumFxSlots - 1; ++slot)
    {
        const auto a = slotBoxes[(size_t) slot]->getBounds();
        const auto b = slotBoxes[(size_t) slot + 1]->getBounds();
        const float x0 = (float) a.getRight() + 1.0f, x1 = (float) b.getX() - 1.0f;
        const float y = (float) a.getCentreY();

        if (x1 - x0 < 4.0f)
            continue;

        Path arrow;
        arrow.addArrow ({ x0, y, x1, y }, 1.5f, 7.0f, 5.0f);
        g.fillPath (arrow);
    }
}

void FxPage::resized()
{
    auto area = getLocalBounds().reduced (kPad);

    // --- Catena -------------------------------------------------------------------
    {
        auto strip = area.removeFromTop (40);
        routePanel->setBounds (strip);

        auto inner = strip.reduced (kPad, 9);
        resetChain.setBounds (inner.removeFromRight (72).reduced (0, 0));
        inner.removeFromRight (10);

        const int gap = 14;
        const int w = (inner.getWidth() - gap * (kNumFxSlots - 1)) / kNumFxSlots;

        for (int slot = 0; slot < kNumFxSlots; ++slot)
        {
            slotBoxes[(size_t) slot]->setBounds (inner.removeFromLeft (w));
            inner.removeFromLeft (gap);
        }

        area.removeFromTop (kRowGap);
    }

    const int rowH = (area.getHeight() - kRowGap * 3) / 4;

    auto layoutSection = [] (TexturedPanel* panel, SectionHeader* header,
                             juce::Array<juce::Component*> headerItems,
                             juce::Array<juce::Component*> knobItems,
                             juce::Rectangle<int> bounds)
    {
        panel->setBounds (bounds);

        auto inner = bounds.reduced (kPad, 5);
        auto head = inner.removeFromTop (kHeaderH);

        header->setBounds (head.removeFromLeft (juce::jmin (140, head.getWidth() / 2)));

        for (auto* item : headerItems)
        {
            head.removeFromLeft (4);
            const int w = (dynamic_cast<juce::ComboBox*> (item) != nullptr) ? 96 : 46;
            item->setBounds (head.removeFromLeft (juce::jmin (w, juce::jmax (0, head.getWidth()))).reduced (0, 1));
        }

        inner.removeFromTop (2);
        layoutRow (knobItems, inner);
    };

    // Riga 1: distorsione + EQ
    {
        auto row = area.removeFromTop (rowH);
        auto left = row.removeFromLeft (row.getWidth() * 2 / 5);
        row.removeFromLeft (kRowGap);

        layoutSection (distPanel, headers[0], { buttons[0], distAlgo },
                       { knobs[0], knobs[1], knobs[2], knobs[3], knobs[4] }, left);

        layoutSection (eqPanel, headers[1], {},
                       { knobs[5], knobs[6], knobs[7], knobs[8], knobs[9], knobs[10], knobs[11] }, row);
    }

    area.removeFromTop (kRowGap);

    // Riga 2: chorus/phaser + delay
    {
        auto row = area.removeFromTop (rowH);
        auto left = row.removeFromLeft (row.getWidth() * 5 / 9);
        row.removeFromLeft (kRowGap);

        layoutSection (modPanel, headers[2], { buttons[1], buttons[2] },
                       { knobs[12], knobs[13], knobs[14], knobs[15],
                         knobs[16], knobs[17], knobs[18], knobs[19], knobs[20] }, left);

        layoutSection (delayPanel, headers[3], { buttons[3], buttons[4], buttons[5], delayDivL, delayDivR },
                       { knobs[21], knobs[22], knobs[23], knobs[24] }, row);
    }

    area.removeFromTop (kRowGap);

    // Riga 3: riverbero + dinamica
    {
        auto row = area.removeFromTop (rowH);
        auto left = row.removeFromLeft (row.getWidth() * 4 / 9);
        row.removeFromLeft (kRowGap);

        layoutSection (reverbPanel, headers[4], { buttons[6] },
                       { knobs[25], knobs[26], knobs[27], knobs[28], knobs[29], knobs[30], knobs[31] }, left);

        layoutSection (dynPanel, headers[5], { buttons[7], buttons[8], buttons[9] },
                       { knobs[32], knobs[33], knobs[34], knobs[35],
                         knobs[36], knobs[37], knobs[38], knobs[39] }, row);
    }

    area.removeFromTop (kRowGap);

    // Riga 4: glitch + beat repeat
    {
        auto row = area;
        auto left = row.removeFromLeft (row.getWidth() / 2);
        row.removeFromLeft (kRowGap);

        layoutSection (glitchPanel, headers[6], { buttons[10], glitchRate, glitchMode },
                       { knobs[40], knobs[41], knobs[42] }, left);
        layoutSection (repeatPanel, headers[7], { buttons[11], repInterval, repGrid, repGate },
                       { knobs[43], knobs[44], knobs[45] }, row);
    }
}

//==============================================================================
// FishPage
//==============================================================================

FishPage::FishPage (AudioProcessorValueTreeState& state, BassXAudioProcessor& proc)
    : PageBase (state), processor (proc)
{
    using KS = BassXLookAndFeel::KnobStyle;

    // --- Fila Bass Line ----------------------------------------------------------
    linePanel = addModulePanel();
    addHeader ("Bass Line");
    extMode = addToggle (ParamID::dfExtMode, "EXT MODE");
    extMode->setTooltip ("Ext Mode: l'audio in ingresso (sidechain) passa da filtro e VCA, comandato dalle note");
    addAndMakeVisible (extLed);

    addKnob (ParamID::masterTune,         "Tuning");
    addKnob (ParamID::f1Cutoff,           "Cut Off");
    addKnob (ParamID::f1Reso,             "Resonance");
    addKnob (ParamID::f1EnvAmount,        "Envmod");
    addKnob (ParamID::env (1, "decay"),   "Decay");
    addKnob (ParamID::accentAmount,       "Accent");

    // --- Fila Devil Fish -----------------------------------------------------------
    fishPanel = addModulePanel();
    addHeader ("Devil Fish");
    addKnob (ParamID::glideTime,    "Slide Time");
    addKnob (ParamID::accentFilter, "Sweep Amt.");
    addKnob (ParamID::dfEnvAttack,  "Env. Attack");
    addKnob (ParamID::accentDecay,  "Acc. Decay");
    addKnob (ParamID::accentVca,    "Acc. Vol.");
    addKnob (ParamID::dfVibSpeed,   "Vib. Speed");
    addKnob (ParamID::dfVibDepth,   "Vib. Depth");

    for (auto* led : { &resoLed, &accentLed, &sweepLed, &accVolLed })
        addAndMakeVisible (*led);

    // --- Neural / mod ---------------------------------------------------------------
    neuralPanel = addModulePanel();
    addHeader ("Neural Engine", Palette::lcdGreen);
    addKnob (ParamID::dfNeuralDrive, "Neural Drive", KS::Orange);
    addKnob (ParamID::dfFilterFm,    "Filter FM",    KS::Orange);
    addKnob (ParamID::dfMuffler,     "Muffler",      KS::Dark);
    addKnob (ParamID::dfSweepSpeed,  "Sweep Speed",  KS::Dark);
    addKnob (ParamID::dfExtGain,     "Ext Gain",     KS::Dark);

    addCaption ("Neural Drive: preamp a diodi modellato da una rete GRU (12 stati) addestrata sul "
                "circuito. Filter FM: l'uscita del VCA modula il cutoff. Muffler: soft clip sul VCA. "
                "Sweep Speed: carica lenta dell'accento. Env Attack: attacco morbido delle note non accentate.");

    scopePanel = addModulePanel();
    addHeader ("Output Scope", Palette::lcdGreen);
    addAndMakeVisible (scope);

    startTimerHz (30);
}

void FishPage::timerCallback()
{
    if (isShowing())
        scope.update (processor);

    const float accent = processor.meters.accentCharge.load();
    const float amp = processor.meters.ampEnvLevel.load();
    const float reso = apvts.getRawParameterValue (ParamID::f1Reso)->load();

    resoLed.setLevel (reso * amp);
    sweepLed.setLevel (jlimit (0.0f, 1.0f, accent));
    accentLed.setLevel (jlimit (0.0f, 1.0f, accent));
    accVolLed.setLevel (jlimit (0.0f, 1.0f, accent * amp));

    const bool ext = apvts.getRawParameterValue (ParamID::dfExtMode)->load() > 0.5f;
    extLed.setLevel (ext ? (processor.meters.extInputConnected.load() ? 1.0f : 0.35f) : 0.0f);
}

void FishPage::resized()
{
    auto area = getLocalBounds().reduced (kPad);

    // LED piccolo a destra del knob, sotto la didascalia: non copre testo.
    auto ledOver = [] (LedComponent& led, const Component* knob)
    {
        const auto b = knob->getBounds();
        led.setBounds (b.getRight() - 10, b.getY() + 18, 9, 9);
    };

    // Fila 1: Bass Line + Devil Fish.
    auto row = area.removeFromTop (kModH);
    area.removeFromTop (kRowGap);
    const int lineW = (row.getWidth() - kRowGap) * 46 / 100;

    {
        auto inner = placeModule (linePanel, row.removeFromLeft (lineW));
        auto head = inner.removeFromTop (kHeaderH);
        headers[0]->setBounds (head.removeFromLeft (110));
        extMode->setBounds (head.removeFromRight (84).reduced (0, 1));
        head.removeFromRight (4);
        extLed.setBounds (head.removeFromRight (16).withSizeKeepingCentre (10, 10));
        inner.removeFromTop (4);
        layoutRow ({ knobs[0], knobs[1], knobs[2], knobs[3], knobs[4], knobs[5] }, inner);
        ledOver (resoLed, knobs[2]);
        ledOver (accentLed, knobs[5]);
    }

    row.removeFromLeft (kRowGap);
    {
        auto inner = placeModule (fishPanel, row);
        headers[1]->setBounds (inner.removeFromTop (kHeaderH));
        inner.removeFromTop (4);
        layoutRow ({ knobs[6], knobs[7], knobs[8], knobs[9], knobs[10], knobs[11], knobs[12] }, inner);
        ledOver (sweepLed, knobs[7]);
        ledOver (accVolLed, knobs[10]);
    }

    // Fila 2: Neural Engine (knob + note) + oscilloscopio.
    {
        auto inner = placeModule (neuralPanel, area.removeFromLeft (lineW));
        headers[2]->setBounds (inner.removeFromTop (kHeaderH));
        inner.removeFromTop (4);
        layoutRow ({ knobs[13], knobs[14], knobs[15], knobs[16], knobs[17] }, inner.removeFromTop (kKnobRowH));
        inner.removeFromTop (8);
        labels[0]->setBounds (inner);
        labels[0]->setJustificationType (Justification::topLeft);
        labels[0]->setFont (BassXLookAndFeel::getCondensed (10.5f, false));
    }

    area.removeFromLeft (kRowGap);
    {
        auto inner = placeModule (scopePanel, area);
        headers[3]->setBounds (inner.removeFromTop (kHeaderH));
        inner.removeFromTop (4);
        scope.setBounds (inner);
    }
}

//==============================================================================
// ArpPage
//==============================================================================

ArpPage::ArpPage (AudioProcessorValueTreeState& state, BassXAudioProcessor& proc)
    : PageBase (state), processor (proc)
{
    using KS = BassXLookAndFeel::KnobStyle;

    arpPanel = addModulePanel();
    addHeader ("Arpeggiator");
    addToggle (ParamID::arpEnable, "ON");
    addToggle (ParamID::arpLatch,  "LATCH");
    addToggle (ParamID::arpSlide,  "SLIDE");
    mode     = addCombo (ParamID::arpMode);
    division = addCombo (ParamID::arpDivision);
    accent   = addCombo (ParamID::arpAccent);
    addKnob (ParamID::arpOctaves, "Octaves", KS::Orange);
    addKnob (ParamID::arpGate,    "Gate");
    addKnob (ParamID::arpSwing,   "Swing");
    addAndMakeVisible (arpLed);

    comboPanel = addModulePanel();
    addHeader ("Seq + Arp");
    addToggle (ParamID::seqEnable, "SEQ RUN");
    combine = addCombo (ParamID::playCombine);
    addAndMakeVisible (noteDisplay);
    noteDisplay.setLines ("ARP NOTE", "--");

    addCaption ("Layer: sequencer e arpeggiatore suonano insieme, ognuno col suo ritmo. "
                "Seq Rhythm > Arp Notes: ritmo, accenti e slide dal sequencer, note dall'accordo tenuto. "
                "Seq Transposes Arp: l'arpeggio segue la trasposizione del passo corrente. "
                "Orologio agganciato alla DAW; a trasporto fermo suona finche' tieni i tasti (o col LATCH).");

    scopePanel = addModulePanel();
    addHeader ("Output Scope", Palette::lcdGreen);
    addAndMakeVisible (scope);

    startTimerHz (24);
}

void ArpPage::timerCallback()
{
    if (isShowing())
        scope.update (processor);

    const int note = processor.meters.arpNote.load();
    const bool on = apvts.getRawParameterValue (ParamID::arpEnable)->load() > 0.5f;

    arpLed.setLevel (on && note >= 0 ? 1.0f : 0.0f);

    if (note != lastNote)
    {
        lastNote = note;
        noteDisplay.setLines ("ARP NOTE",
                              note >= 0 ? MidiMessage::getMidiNoteName (note, true, true, 3) : String ("--"));
    }
}

void ArpPage::resized()
{
    auto area = getLocalBounds().reduced (kPad);

    // Fila 1: arpeggiatore (modulo standard a tutta larghezza).
    {
        auto inner = placeModule (arpPanel, area.removeFromTop (kModH));
        auto head = inner.removeFromTop (kHeaderH);
        headers[0]->setBounds (head.removeFromLeft (120));
        arpLed.setBounds (head.removeFromLeft (16).withSizeKeepingCentre (10, 10));

        for (auto* c : { accent, division, mode })
        {
            c->setBounds (head.removeFromRight (120).reduced (0, 1));
            head.removeFromRight (6);
        }
        for (auto* b : { buttons[2], buttons[1], buttons[0] })
        {
            b->setBounds (head.removeFromRight (64).reduced (0, 1));
            head.removeFromRight (6);
        }

        inner.removeFromTop (4);
        layoutRow ({ knobs[0], knobs[1], knobs[2] }, inner);
    }

    area.removeFromTop (kRowGap);
    const int halfW = (area.getWidth() - kRowGap) / 2;

    // Fila 2: combinazione Seq + Arp + oscilloscopio.
    {
        auto inner = placeModule (comboPanel, area.removeFromLeft (halfW));
        auto head = inner.removeFromTop (kHeaderH);
        headers[1]->setBounds (head.removeFromLeft (110));
        combine->setBounds (head.removeFromRight (juce::jmin (200, head.getWidth() / 2)).reduced (0, 1));
        head.removeFromRight (6);
        buttons[3]->setBounds (head.removeFromRight (84).reduced (0, 1));

        inner.removeFromTop (8);
        noteDisplay.setBounds (inner.removeFromTop (56));
        inner.removeFromTop (8);
        labels[0]->setBounds (inner);
        labels[0]->setJustificationType (Justification::topLeft);
        labels[0]->setFont (BassXLookAndFeel::getCondensed (10.5f, false));
    }

    area.removeFromLeft (kRowGap);
    {
        auto inner = placeModule (scopePanel, area);
        headers[2]->setBounds (inner.removeFromTop (kHeaderH));
        inner.removeFromTop (4);
        scope.setBounds (inner);
    }
}

//==============================================================================
// Goniometer
//==============================================================================

void Goniometer::setPoints (const std::vector<Point<float>>& pts, float corr)
{
    points = pts;
    correlation = corr;
    repaint();
}

void Goniometer::paint (Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    auto meter = b.removeFromBottom (16.0f);

    paintDisplayGlass (g, b);

    const auto c = b.getCentre();
    const float r = juce::jmin (b.getWidth(), b.getHeight()) * 0.46f;

    // Assi: verticale = mono (M), diagonali = L e R.
    g.setColour (Palette::lcdGreen.withAlpha (0.18f));
    g.drawLine (c.x, c.y - r, c.x, c.y + r);
    g.drawLine (c.x - r, c.y, c.x + r, c.y);
    g.drawLine (c.x - r * 0.707f, c.y - r * 0.707f, c.x + r * 0.707f, c.y + r * 0.707f);
    g.drawLine (c.x + r * 0.707f, c.y - r * 0.707f, c.x - r * 0.707f, c.y + r * 0.707f);

    g.setColour (Palette::lcdGreen.withAlpha (0.8f));

    for (const auto& p : points)
    {
        // Rotazione di 45°: L+R in verticale, L-R in orizzontale.
        const float m = (p.x + p.y) * 0.7071f, sd = (p.x - p.y) * 0.7071f;
        const float x = c.x + jlimit (-1.0f, 1.0f, sd) * r;
        const float y = c.y - jlimit (-1.0f, 1.0f, m) * r;
        g.fillRect (x, y, 1.6f, 1.6f);
    }

    // Correlazione: -1 (fuori fase) .. +1 (mono).
    meter.reduce (2.0f, 3.0f);
    g.setColour (Palette::bg0);
    g.fillRect (meter);
    g.setColour (Palette::line);
    g.drawRect (meter, 1.0f);

    const float pos = meter.getX() + (correlation * 0.5f + 0.5f) * meter.getWidth();
    g.setColour (correlation < 0.0f ? Palette::danger : Palette::lcdGreen);
    g.fillRect (Rectangle<float> (juce::jmin (pos, meter.getCentreX()), meter.getY(),
                                  std::abs (pos - meter.getCentreX()), meter.getHeight()));
    g.setColour (Palette::dimText);
    g.setFont (BassXLookAndFeel::getCondensed (10.0f, false));
    g.drawText ("CORRELATION " + String (correlation, 2), meter.toNearestInt(), Justification::centred);
}

//==============================================================================
// SpacePage
//==============================================================================

//==============================================================================
// Sidechain
//==============================================================================

Point<float> SidechainCurveEditor::toScreen (float x, float y) const
{
    auto b = getLocalBounds().toFloat().reduced (10.0f, 12.0f);
    return { b.getX() + x * b.getWidth(), b.getBottom() - y * b.getHeight() };
}

Point<float> SidechainCurveEditor::fromScreen (Point<float> p) const
{
    auto b = getLocalBounds().toFloat().reduced (10.0f, 12.0f);
    return { jlimit (0.0f, 1.0f, (p.x - b.getX()) / b.getWidth()),
             jlimit (0.0f, 1.0f, (b.getBottom() - p.y) / b.getHeight()) };
}

void SidechainCurveEditor::refresh()
{
    if (dragPoint < 0 && dragSegment < 0)
        curve = processor.getSidechainDisplayCurve();
    phase  = processor.getSidechainPhase();
    gain   = processor.getSidechainGain();
    offset = apvts.getRawParameterValue (ParamID::scPhase)->load();
    repaint();
}

void SidechainCurveEditor::beginEdit()
{
    auto* shapeParam = apvts.getParameter (ParamID::scShape);
    if ((int) apvts.getRawParameterValue (ParamID::scShape)->load() != SidechainDucker::kCustomShape)
    {
        processor.setSidechainCustomCurve (curve);   // la forma di fabbrica diventa il punto di partenza
        shapeParam->setValueNotifyingHost (shapeParam->convertTo0to1 ((float) SidechainDucker::kCustomShape));
    }
}

void SidechainCurveEditor::push() { processor.setSidechainCustomCurve (curve); }

void SidechainCurveEditor::mouseDown (const MouseEvent& e)
{
    dragPoint = dragSegment = -1;
    const auto pos = e.position;

    for (int i = 0; i < curve.numPoints; ++i)
        if (toScreen (curve.points[(size_t) i].x, curve.points[(size_t) i].y).getDistanceFrom (pos) < 9.0f)
        {
            beginEdit();
            dragPoint = i;
            return;
        }

    const auto v = fromScreen (pos);

    // tasto destro / alt: piega il segmento sotto il mouse (tensione)
    if (e.mods.isPopupMenu() || e.mods.isAltDown())
    {
        for (int i = 0; i < curve.numPoints - 1; ++i)
            if (v.x >= curve.points[(size_t) i].x && v.x <= curve.points[(size_t) i + 1].x)
            {
                beginEdit();
                dragSegment = i;
                dragStartTension = curve.points[(size_t) i].tension;
                dragStartY = pos.y;
                return;
            }
        return;
    }

    // click sul vuoto: nuovo punto
    if (curve.numPoints < SidechainDucker::kMaxPoints)
    {
        beginEdit();
        int at = curve.numPoints;
        for (int i = 0; i < curve.numPoints; ++i)
            if (curve.points[(size_t) i].x > v.x) { at = i; break; }
        at = jlimit (1, curve.numPoints, at);
        for (int i = curve.numPoints; i > at; --i) curve.points[(size_t) i] = curve.points[(size_t) i - 1];
        curve.points[(size_t) at] = { v.x, v.y, 0.0f };
        ++curve.numPoints;
        dragPoint = at;
        push();
    }
}

void SidechainCurveEditor::mouseDrag (const MouseEvent& e)
{
    if (dragPoint >= 0)
    {
        auto v = fromScreen (e.position);
        auto& p = curve.points[(size_t) dragPoint];
        const bool first = dragPoint == 0, lastPt = dragPoint == curve.numPoints - 1;
        // gli estremi restano a x = 0 e x = 1, gli altri fra i vicini
        if (first) v.x = 0.0f;
        else if (lastPt) v.x = 1.0f;
        else v.x = jlimit (curve.points[(size_t) dragPoint - 1].x + 0.005f, curve.points[(size_t) dragPoint + 1].x - 0.005f, v.x);
        p.x = v.x; p.y = v.y;
        push();
        repaint();
    }
    else if (dragSegment >= 0)
    {
        auto& p = curve.points[(size_t) dragSegment];
        p.tension = jlimit (-0.95f, 0.95f, dragStartTension + (dragStartY - e.position.y) / 120.0f);
        push();
        repaint();
    }
}

void SidechainCurveEditor::mouseUp (const MouseEvent&) { dragPoint = dragSegment = -1; }

void SidechainCurveEditor::mouseDoubleClick (const MouseEvent& e)
{
    for (int i = 1; i < curve.numPoints - 1; ++i)
        if (toScreen (curve.points[(size_t) i].x, curve.points[(size_t) i].y).getDistanceFrom (e.position) < 9.0f)
        {
            beginEdit();
            for (int k = i; k < curve.numPoints - 1; ++k) curve.points[(size_t) k] = curve.points[(size_t) k + 1];
            --curve.numPoints;
            push();
            repaint();
            return;
        }
}

void SidechainCurveEditor::paint (Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (0.5f);
    paintDisplayGlass (g, b);
    auto area = getLocalBounds().toFloat().reduced (10.0f, 12.0f);

    // griglia: quarti del ciclo
    g.setColour (Palette::dimText.withAlpha (0.18f));
    for (int i = 1; i < 4; ++i)
        g.drawVerticalLine ((int) (area.getX() + area.getWidth() * (float) i / 4.0f), area.getY(), area.getBottom());
    g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());

    // curva (con l'offset di PHASE applicato: e' cio' che si sente)
    Path p, fill;
    for (int i = 0; i <= 300; ++i)
    {
        const float x = (float) i / 300.0f;
        float q = x - offset;
        if (q < 0.0f) q += 1.0f; else if (q > 1.0f) q -= 1.0f;   // x = 1 resta la fine del ciclo, non torna a 0
        const auto pt = toScreen (x, SidechainDucker::evaluate (curve, q));
        if (i == 0) { p.startNewSubPath (pt); fill.startNewSubPath (area.getX(), area.getY()); fill.lineTo (pt); }
        else { p.lineTo (pt); fill.lineTo (pt); }
    }
    fill.lineTo (area.getRight(), area.getY());
    fill.closeSubPath();
    g.setColour (Palette::danger.withAlpha (0.18f));     // la parte tagliata
    g.fillPath (fill);
    strokeGlow (g, p, Palette::lcdGreen);

    // punti modificabili (curva senza offset, cosi' si trascinano dove si vedono a PHASE 0)
    for (int i = 0; i < curve.numPoints; ++i)
    {
        float sx = curve.points[(size_t) i].x + offset; sx -= std::floor (sx);
        if (i == curve.numPoints - 1 && offset == 0.0f) sx = 1.0f;
        const auto pt = toScreen (sx, curve.points[(size_t) i].y);
        g.setColour (Palette::orange);
        g.fillEllipse (Rectangle<float> (8.0f, 8.0f).withCentre (pt));
    }

    // testina di fase + marcatore dell'offset
    const float px = area.getX() + area.getWidth() * jlimit (0.0f, 1.0f, std::fmod (phase + offset + 1.0f, 1.0f));
    g.setColour (Palette::orange.withAlpha (0.9f));
    g.drawVerticalLine ((int) px, area.getY(), area.getBottom());
    const float ox = area.getX() + area.getWidth() * (offset < 0.0f ? offset + 1.0f : offset);
    g.setColour (Palette::lcdGreen.withAlpha (0.5f));
    g.fillRect (Rectangle<float> (ox - 1.0f, area.getY(), 2.0f, 8.0f));

    g.setFont (BassXLookAndFeel::getLcdFont (10.0f));
    g.setColour (Palette::dimText);
    g.drawText ("GAIN " + String (Decibels::gainToDecibels (gain, -60.0f), 1) + " dB   PHASE "
                  + String (roundToInt (offset * 100.0f)) + "%",
                b.reduced (6.0f, 3.0f), Justification::topRight, false);
    g.drawText ("CLICK: PUNTO   DOPPIO CLICK: TOGLI   ALT/DESTRO + TRASCINA: CURVA",
                b.reduced (6.0f, 3.0f), Justification::bottomLeft, false);
}

void SidechainSpectrum::refresh (float lo, float hi, bool on)
{
    bandLo = lo; bandHi = hi; active = on;
    gain = processor.getSidechainGain();

    const int w = processor.analyserWrite.load (std::memory_order_relaxed);
    auto analyse = [&] (const std::array<std::atomic<float>, BassXAudioProcessor::kAnalyserSize>& ring, std::vector<float>& out)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < kSize; ++i)
        {
            const float win = 0.5f - 0.5f * std::cos (MathConstants<float>::twoPi * (float) i / (float) (kSize - 1));
            work[(size_t) i] = ring[(size_t) ((w + i + BassXAudioProcessor::kAnalyserSize - kSize) % BassXAudioProcessor::kAnalyserSize)].load (std::memory_order_relaxed) * win;
        }
        fft.performFrequencyOnlyForwardTransform (work.data());
        for (int k = 0; k < kSize / 2; ++k)
        {
            const float db = Decibels::gainToDecibels (work[(size_t) k] * 4.0f / (float) kSize, -100.0f);
            out[(size_t) k] = db > out[(size_t) k] ? db : out[(size_t) k] * 0.85f + db * 0.15f;   // caduta morbida
        }
    };
    analyse (processor.analyserPre, pre);
    analyse (processor.analyserPost, post);
    repaint();
}

void SidechainSpectrum::paint (Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (0.5f);
    paintDisplayGlass (g, b);
    auto area = b.reduced (6.0f, 8.0f);
    const double sr = jmax (8000.0, processor.getSampleRate() > 0 ? processor.getSampleRate() : 48000.0);

    auto xOf = [&] (float hz) { return area.getX() + std::log10 (jlimit (20.0f, 20000.0f, hz) / 20.0f) / 3.0f * area.getWidth(); };
    auto yOf = [&] (float db) { return jmap (jlimit (-90.0f, 0.0f, db), 0.0f, -90.0f, area.getY(), area.getBottom()); };

    // banda colpita dal sidechain, alta quanto la riduzione corrente
    if (active)
    {
        const float x0 = xOf (bandLo), x1 = xOf (bandHi);
        g.setColour (Palette::danger.withAlpha (0.10f));
        g.fillRect (Rectangle<float> (x0, area.getY(), x1 - x0, area.getHeight()));
        const float duckH = area.getHeight() * jlimit (0.0f, 1.0f, 1.0f - gain);
        g.setColour (Palette::danger.withAlpha (0.35f));
        g.fillRect (Rectangle<float> (x0, area.getY(), x1 - x0, duckH));
        g.setColour (Palette::danger.withAlpha (0.7f));
        g.drawVerticalLine ((int) x0, area.getY(), area.getBottom());
        g.drawVerticalLine ((int) x1, area.getY(), area.getBottom());
    }

    auto curve = [&] (const std::vector<float>& db)
    {
        Path p;
        bool started = false;
        for (int k = 1; k < kSize / 2; ++k)
        {
            const float hz = (float) ((double) k * sr / (double) kSize);
            if (hz < 20.0f || hz > 20000.0f) continue;
            const Point<float> pt (xOf (hz), yOf (db[(size_t) k]));
            if (! started) { p.startNewSubPath (pt); started = true; } else p.lineTo (pt);
        }
        return p;
    };
    strokeGlow (g, curve (pre), Palette::dimText, 1.0f);
    strokeGlow (g, curve (post), Palette::lcdGreen);

    g.setFont (BassXLookAndFeel::getLcdFont (10.0f));
    g.setColour (Palette::dimText);
    for (auto f : { 100.0f, 1000.0f, 10000.0f })
        g.drawText (f >= 1000.0f ? String ((int) (f / 1000.0f)) + "k" : String ((int) f),
                    Rectangle<float> (xOf (f) + 3.0f, b.getBottom() - 15.0f, 40.0f, 12.0f), Justification::left, false);
    g.drawText ("PRIMA (GRIGIO) / DOPO (VERDE)" + (active ? String ("   BANDA ") + String (roundToInt (bandLo)) + "-" + String (roundToInt (bandHi)) + " HZ" : String()),
                b.reduced (6.0f, 3.0f), Justification::topLeft, false);
}

SidechainPage::SidechainPage (AudioProcessorValueTreeState& state, BassXAudioProcessor& proc)
    : PageBase (state), curveEditor (proc, state), spectrum (proc), processor (proc)
{
    using KS = BassXLookAndFeel::KnobStyle;

    ctrlPanel = addModulePanel();
    addHeader ("Sidechain");
    addToggle (ParamID::scOn, "ON");
    trigger = addCombo (ParamID::scTrigger);
    rate    = addCombo (ParamID::scRate);
    shape   = addCombo (ParamID::scShape);
    addKnob (ParamID::scDepth,     "Depth",    KS::Orange);
    addKnob (ParamID::scPhase,     "Phase");
    addKnob (ParamID::scSmooth,    "Smooth",   KS::Dark);
    addKnob (ParamID::scBandLow,   "Band Lo");
    addKnob (ParamID::scBandHigh,  "Band Hi");
    addKnob (ParamID::scThreshold, "Thresh",   KS::Dark);

    curvePanel = addModulePanel();
    addHeader ("Curve", Palette::lcdGreen);
    addAndMakeVisible (curveEditor);

    specPanel = addModulePanel();
    addHeader ("Spectrum", Palette::lcdGreen);
    addAndMakeVisible (spectrum);

    startTimerHz (30);
}

void SidechainPage::timerCallback()
{
    if (! isShowing())
        return;

    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    curveEditor.refresh();
    spectrum.refresh (get (ParamID::scBandLow), jmax (get (ParamID::scBandHigh), get (ParamID::scBandLow) * 1.05f),
                      get (ParamID::scOn) > 0.5f);
}

void SidechainPage::resized()
{
    auto area = getLocalBounds().reduced (kPad);

    auto row = area.removeFromTop (kModH);
    area.removeFromTop (kRowGap);
    {
        auto inner = placeModule (ctrlPanel, row);
        auto head = inner.removeFromTop (kHeaderH);
        headers[0]->setBounds (head.removeFromLeft (110));
        buttons[0]->setBounds (head.removeFromLeft (56).reduced (0, 1));
        head.removeFromLeft (8);
        shape->setBounds (head.removeFromRight (130).reduced (0, 1));
        head.removeFromRight (6);
        rate->setBounds (head.removeFromRight (90).reduced (0, 1));
        head.removeFromRight (6);
        trigger->setBounds (head.removeFromRight (140).reduced (0, 1));
        inner.removeFromTop (4);
        layoutRow ({ knobs[0], knobs[1], knobs[2], knobs[3], knobs[4], knobs[5] }, inner);
    }

    const int half = (area.getWidth() - kRowGap) / 2;
    {
        auto inner = placeModule (curvePanel, area.removeFromLeft (half));
        headers[1]->setBounds (inner.removeFromTop (kHeaderH));
        inner.removeFromTop (4);
        curveEditor.setBounds (inner);
        curveEditor.refresh();
    }
    area.removeFromLeft (kRowGap);
    {
        auto inner = placeModule (specPanel, area);
        headers[2]->setBounds (inner.removeFromTop (kHeaderH));
        inner.removeFromTop (4);
        spectrum.setBounds (inner);
    }
}

//==============================================================================
SpacePage::SpacePage (AudioProcessorValueTreeState& state, BassXAudioProcessor& proc)
    : PageBase (state), processor (proc)
{
    using KS = BassXLookAndFeel::KnobStyle;

    imagePanel = addModulePanel();
    addHeader ("Stereo Image");
    addToggle (ParamID::spaceOn, "ON");
    addKnob (ParamID::spacePan,      "Pan",       KS::Orange);
    addKnob (ParamID::spaceWidth,    "Width");
    addKnob (ParamID::spaceMonoFreq, "Bass Mono", KS::Dark);
    addKnob (ParamID::spaceHaas,     "Haas",      KS::Dark);
    addKnob (ParamID::spaceRotate,   "Rotation",  KS::Dark);

    motionPanel = addModulePanel();
    addHeader ("Autopan");
    autopanDiv = addCombo (ParamID::spaceAutopanDiv);
    addKnob (ParamID::spaceAutopan, "Depth", KS::Orange);

    scopePanel = addModulePanel();
    addHeader ("Goniometer", Palette::lcdGreen);
    addAndMakeVisible (scope);

    addCaption ("Bass Mono: sotto la frequenza scelta il lato stereo viene rimosso (LR 24 dB/oct): "
                "sub sempre in mono sull'impianto. Width agisce sul lato (M/S), Haas ritarda il canale "
                "destro, Autopan e' agganciato al tempo della DAW. Lo slot SPACE si sposta nella catena FX.");

    scratch.reserve ((size_t) BassXAudioProcessor::kScopeSize);
    startTimerHz (30);
}

void SpacePage::timerCallback()
{
    if (! isShowing())
        return;

    scratch.clear();

    for (int i = 0; i < BassXAudioProcessor::kScopeSize; ++i)
        scratch.push_back ({ processor.scopeL[(size_t) i].load (std::memory_order_relaxed),
                             processor.scopeR[(size_t) i].load (std::memory_order_relaxed) });

    scope.setPoints (scratch, processor.meters.correlation.load());
}

void SpacePage::resized()
{
    auto area = getLocalBounds().reduced (kPad);

    // Fila 1: immagine stereo + autopan, moduli standard.
    auto row = area.removeFromTop (kModH);
    area.removeFromTop (kRowGap);
    const int imageW = (row.getWidth() - kRowGap) * 62 / 100;

    {
        auto inner = placeModule (imagePanel, row.removeFromLeft (imageW));
        auto head = inner.removeFromTop (kHeaderH);
        headers[0]->setBounds (head.removeFromLeft (130));
        buttons[0]->setBounds (head.removeFromRight (56).reduced (0, 1));
        inner.removeFromTop (4);
        layoutRow ({ knobs[0], knobs[1], knobs[2], knobs[3], knobs[4] }, inner);
    }

    row.removeFromLeft (kRowGap);
    {
        auto inner = placeModule (motionPanel, row);
        auto head = inner.removeFromTop (kHeaderH);
        headers[1]->setBounds (head.removeFromLeft (100));
        autopanDiv->setBounds (head.removeFromRight (96).reduced (0, 1));
        inner.removeFromTop (4);
        layoutRow ({ knobs[5] }, inner);
    }

    // Fila 2: goniometro con le note a sinistra.
    {
        auto inner = placeModule (scopePanel, area);
        headers[2]->setBounds (inner.removeFromTop (kHeaderH));
        inner.removeFromTop (6);
        auto notes = inner.removeFromLeft (juce::jmin (320, inner.getWidth() / 3));
        labels[0]->setBounds (notes);
        labels[0]->setJustificationType (Justification::topLeft);
        labels[0]->setFont (BassXLookAndFeel::getCondensed (10.5f, false));
        inner.removeFromLeft (10);
        scope.setBounds (inner);
    }
}

//==============================================================================
// TubePage
//==============================================================================

void TubeCurve::setParams (const eta::EngineParams& p, bool on)
{
    const bool changed = on != active || p.profile != params.profile || p.drive != params.drive
                      || p.bias != params.bias || p.character != params.character || p.age != params.age;
    params = p;
    active = on;
    if (changed)
        repaint();
}

void TubeCurve::paint (Graphics& g)
{
    auto plot = getLocalBounds().toFloat();
    paintDisplayGlass (g, plot.reduced (0.5f));
    plot.reduce (6.0f, 6.0f);

    const auto d = eta::TubeEngine::derive (params);
    constexpr int pts = 140;
    float ys[pts], peak = 1.0e-3f;
    for (int i = 0; i < pts; ++i)
    {
        ys[i] = eta::TubeEngine::transfer (d, -1.0f + 2.0f * (float) i / (float) (pts - 1));
        peak = jmax (peak, std::abs (ys[i]));
    }

    Path p;   // auto-scalata: conta la forma, non il livello
    for (int i = 0; i < pts; ++i)
    {
        const float px = plot.getX() + (float) i / (float) (pts - 1) * plot.getWidth();
        const float py = plot.getCentreY() - 0.9f * ys[i] / peak * plot.getHeight() * 0.5f;
        if (i == 0) p.startNewSubPath (px, py); else p.lineTo (px, py);
    }

    const float a = active ? 1.0f : 0.3f;
    g.setColour (Palette::lcdGreen.withAlpha (0.22f * a));
    g.strokePath (p, PathStrokeType (6.0f, PathStrokeType::curved, PathStrokeType::rounded));
    g.setColour (Palette::lcdGreen.withAlpha (a));
    g.strokePath (p, PathStrokeType (1.8f, PathStrokeType::curved, PathStrokeType::rounded));

    g.setFont (BassXLookAndFeel::getCondensed (10.0f, false));
    g.drawText (eta::profiles()[(size_t) jlimit (0, eta::kNumProfiles - 1, params.profile)].name,
                getLocalBounds().reduced (8, 4), Justification::topLeft);
}

TubePage::TubePage (AudioProcessorValueTreeState& state)
    : PageBase (state)
{
    using KS = BassXLookAndFeel::KnobStyle;

    valvePanel = addModulePanel();
    addHeader ("Valve");
    addToggle (ParamID::tubeOn, "ON");
    model = addCombo (ParamID::tubeModel);
    addKnob (ParamID::tubeDrive,     "Drive",     KS::Orange);
    addKnob (ParamID::tubeBias,      "Bias");
    addKnob (ParamID::tubeSag,       "Sag");
    addKnob (ParamID::tubeCharacter, "Character", KS::Dark);
    addKnob (ParamID::tubeAge,       "Age",       KS::Dark);

    tonePanel = addModulePanel();
    addHeader ("Tone / Output");
    addKnob (ParamID::tubeWarmth,   "Warmth",   KS::Dark);
    addKnob (ParamID::tubePresence, "Presence", KS::Dark);
    addKnob (ParamID::tubeTone,     "Tone",     KS::Dark);
    addKnob (ParamID::tubeMix,      "Mix",      KS::Orange);
    addKnob (ParamID::tubeOutput,   "Output");

    curvePanel = addModulePanel();
    addHeader ("Transfer", Palette::lcdGreen);
    addAndMakeVisible (curve);

    addCaption ("Motore ETA-1: modelli Koren triodo/pentodo sulla retta di carico, sag dell'alimentatore, "
                "blocking di griglia, trasformatore d'uscita. Oversampling 4x. Lo stadio sta in testa alla "
                "catena FX, prima della distorsione: Sag comprime gli accenti come un vero preamp.");

    startTimerHz (20);
}

void TubePage::timerCallback()
{
    if (isShowing())
        refreshCurve();
}

void TubePage::refreshCurve()
{
    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    eta::EngineParams p;
    p.profile   = (int) get (ParamID::tubeModel);
    p.drive     = get (ParamID::tubeDrive);
    p.bias      = get (ParamID::tubeBias);
    p.character = get (ParamID::tubeCharacter);
    p.age       = get (ParamID::tubeAge);
    curve.setParams (p, get (ParamID::tubeOn) > 0.5f);
}

void TubePage::resized()
{
    auto area = getLocalBounds().reduced (kPad);

    // Fila 1: valvola + tono/uscita, moduli standard affiancati.
    auto row = area.removeFromTop (kModH);
    area.removeFromTop (kRowGap);
    const int halfW = (row.getWidth() - kRowGap) / 2;

    {
        auto inner = placeModule (valvePanel, row.removeFromLeft (halfW));
        auto head = inner.removeFromTop (kHeaderH);
        headers[0]->setBounds (head.removeFromLeft (80));
        model->setBounds (head.removeFromRight (140).reduced (0, 1));
        head.removeFromRight (6);
        buttons[0]->setBounds (head.removeFromRight (56).reduced (0, 1));
        inner.removeFromTop (4);
        layoutRow ({ knobs[0], knobs[1], knobs[2], knobs[3], knobs[4] }, inner);
    }

    row.removeFromLeft (kRowGap);
    {
        auto inner = placeModule (tonePanel, row);
        headers[1]->setBounds (inner.removeFromTop (kHeaderH));
        inner.removeFromTop (4);
        layoutRow ({ knobs[5], knobs[6], knobs[7], knobs[8], knobs[9] }, inner);
    }

    // Fila 2: curva di trasferimento a tutta larghezza, note a sinistra.
    {
        auto inner = placeModule (curvePanel, area);
        headers[2]->setBounds (inner.removeFromTop (kHeaderH));
        inner.removeFromTop (6);
        auto notes = inner.removeFromLeft (juce::jmin (320, inner.getWidth() / 3));
        labels[0]->setBounds (notes);
        labels[0]->setJustificationType (Justification::topLeft);
        labels[0]->setFont (BassXLookAndFeel::getCondensed (10.5f, false));
        inner.removeFromLeft (10);
        curve.setBounds (inner);
        refreshCurve();
    }
}

} // namespace bassx
