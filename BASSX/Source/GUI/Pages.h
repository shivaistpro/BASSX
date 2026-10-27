/*
  ==============================================================================

    BASSX — Pages.h

    Le pagine del pannello inferiore. Ciascuna è un modulo indipendente che
    conosce solo l'APVTS: nessuna dipende dalle altre, quindi aggiungerne una
    non richiede di toccare le esistenti.

  ==============================================================================
*/

#pragma once

#include "../DSP/TubeStage.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include "Widgets.h"
#include "../Parameters.h"
#include "../DSP/ModMatrix.h"
#include "../DSP/Sequencer.h"

class BassXAudioProcessor;

namespace bassx
{

//==============================================================================
/**
    Base comune: fabbrica di controlli già agganciati all'APVTS.

    I controlli sono posseduti dalla pagina tramite OwnedArray, e gli
    attachment vivono in array paralleli. Questo è ciò che permette di
    dichiarare una pagina intera in una manciata di righe invece di ripetere
    per ogni parametro la coppia membro + attachment.
*/
class PageBase  : public juce::Component
{
public:
    explicit PageBase (juce::AudioProcessorValueTreeState& state) : apvts (state) {}
    ~PageBase() override = default;

    /** Passo fisso della griglia dei knob, uguale in tutte le pagine. */
    static constexpr int kKnobPitch = 66;
    static constexpr int kKnobRowH  = 70;

    /** Dispone una riga di componenti dentro `area`.
        Righe di soli knob: passo fisso, gruppo centrato nell'area in entrambe
        le direzioni (stessa griglia in tutte le pagine, knob mai giganti). Righe miste o fixedPitch = false:
        larghezze uguali su tutta l'area (usata dalla fila principale). */
    static void layoutRow (juce::Array<juce::Component*> items, juce::Rectangle<int> area, int gap = 4,
                           bool fixedPitch = true)
    {
        if (items.isEmpty() || area.isEmpty())
            return;

        bool allKnobs = true;
        for (auto* c : items)
            allKnobs = allKnobs && dynamic_cast<LabelledKnob*> (c) != nullptr;

        const int total = area.getWidth() - gap * (items.size() - 1);
        int each = juce::jmax (10, total / items.size());

        if (fixedPitch && allKnobs)
        {
            each = juce::jmin (each, kKnobPitch - gap);
            const int groupW = each * items.size() + gap * (items.size() - 1);
            area = area.withSizeKeepingCentre (groupW, juce::jmin (area.getHeight(), kKnobRowH));
        }

        for (int i = 0; i < items.size(); ++i)
        {
            items[i]->setBounds (area.removeFromLeft (each));

            if (i < items.size() - 1)
                area.removeFromLeft (gap);
        }
    }

protected:
    using SliderAttachment  = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment   = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment  = juce::AudioProcessorValueTreeState::ButtonAttachment;

    //==========================================================================
    LabelledKnob* addKnob (const juce::String& paramID, const juce::String& caption,
                           BassXLookAndFeel::KnobStyle style = BassXLookAndFeel::KnobStyle::Chrome,
                           bool onMetal = false)
    {
        auto* k = knobs.add (new LabelledKnob (caption, style, onMetal));
        addAndMakeVisible (k);
        sliderAttachments.add (new SliderAttachment (apvts, paramID, k->getSlider()));
        return k;
    }

    juce::ComboBox* addCombo (const juce::String& paramID)
    {
        auto* c = combos.add (new juce::ComboBox());
        addAndMakeVisible (c);

        // Le voci vengono prese dal parametro stesso: duplicarle qui
        // significherebbe che un giorno le due liste divergono.
        if (auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (paramID)))
            c->addItemList (choiceParam->choices, 1);

        comboAttachments.add (new ComboAttachment (apvts, paramID, *c));
        return c;
    }

    juce::TextButton* addToggle (const juce::String& paramID, const juce::String& text)
    {
        auto* b = buttons.add (new juce::TextButton (text));
        b->setClickingTogglesState (true);
        addAndMakeVisible (b);
        buttonAttachments.add (new ButtonAttachment (apvts, paramID, *b));
        return b;
    }

    juce::Label* addCaption (const juce::String& text,
                             juce::Justification just = juce::Justification::centredLeft)
    {
        auto* l = labels.add (new juce::Label ({}, text.toUpperCase()));
        l->setJustificationType (just);
        l->setFont (BassXLookAndFeel::getCondensed (11.0f, false));
        l->setColour (juce::Label::textColourId, Palette::dimText);
        l->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (l);
        return l;
    }

    SectionHeader* addHeader (const juce::String& title, juce::Colour accent = Palette::orange)
    {
        auto* h = headers.add (new SectionHeader (title, accent));
        addAndMakeVisible (h);
        return h;
    }

    /** Riquadro di modulo con sfondo a texture, usato per raggruppare
        visivamente una sezione. */
    TexturedPanel* addModulePanel()
    {
        auto* p = modulePanels.add (new TexturedPanel (TexturedPanel::Kind::Module));
        p->setShowBorder (true);
        addAndMakeVisible (p);
        p->toBack();
        return p;
    }

    juce::AudioProcessorValueTreeState& apvts;

    juce::OwnedArray<LabelledKnob>   knobs;
    juce::OwnedArray<juce::ComboBox> combos;
    juce::OwnedArray<juce::TextButton> buttons;
    juce::OwnedArray<juce::Label>    labels;
    juce::OwnedArray<SectionHeader>  headers;
    juce::OwnedArray<TexturedPanel>  modulePanels;

    juce::OwnedArray<SliderAttachment> sliderAttachments;
    juce::OwnedArray<ComboAttachment>  comboAttachments;
    juce::OwnedArray<ButtonAttachment> buttonAttachments;
};

//==============================================================================
/** Pagina oscillatori: tre oscillatori wavetable, sub, rumore, cross-mod. */
class OscPage  : public PageBase,
                 public juce::FileDragAndDropTarget,
                 private juce::Timer
{
public:
    OscPage (juce::AudioProcessorValueTreeState& state, BassXAudioProcessor& proc);

    // trascina un campione audio su un oscillatore: diventa la sua wavetable
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray&, int x, int y) override { dropTarget = stripAt (x, y); repaint(); }
    void fileDragMove (const juce::StringArray&, int x, int y) override  { dropTarget = stripAt (x, y); repaint(); }
    void fileDragExit (const juce::StringArray&) override                { dropTarget = -1; repaint(); }
    void paintOverChildren (juce::Graphics& g) override;

    void resized() override;

private:
    void timerCallback() override;

    struct OscStrip
    {
        TexturedPanel* panel = nullptr;
        SectionHeader* header = nullptr;
        juce::TextButton* enable = nullptr;
        juce::ComboBox* wave = nullptr;
        WavetableDisplay* display = nullptr;
        LabelledKnob* position = nullptr;
        LabelledKnob* level = nullptr;
        LabelledKnob* pan = nullptr;
        LabelledKnob* octave = nullptr;
        LabelledKnob* semi = nullptr;
        LabelledKnob* fine = nullptr;
        LabelledKnob* fold = nullptr;
        LabelledKnob* uniVoices = nullptr;
        LabelledKnob* uniDetune = nullptr;
        LabelledKnob* uniBlend = nullptr;
        LabelledKnob* uniSpread = nullptr;
        LabelledKnob* phaseRand = nullptr;
    };

    std::array<OscStrip, kNumOscillators> strips;

    TexturedPanel* extraPanel = nullptr;
    SectionHeader* extraHeader = nullptr;
    juce::OwnedArray<WavetableDisplay> displays;

    const WavetableBank& wavetableBank;
    BassXAudioProcessor& processor;
    int dropTarget = -1;
    int stripAt (int x, int y) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OscPage)
};

//==============================================================================
/** Oscilloscopio dell'uscita (buffer circolare del processore), con trigger
    sul passaggio per lo zero in salita per un'immagine ferma. */
class OutputScope  : public juce::Component
{
public:
    void update (const BassXAudioProcessor& proc);
    void paint (juce::Graphics& g) override;

private:
    std::vector<float> samples;
};

/** Risposta in ampiezza VERA della sezione filtri: i due MultiFilter del
    motore (stesso codice delle voci) vengono misurati con un impulso e una
    FFT, con il modello, il routing e il blend correnti. Prima era un
    passa-basso a 4 poli fisso: cambiare modello non cambiava il grafico. */
class FilterResponse  : public juce::Component
{
public:
    struct Settings
    {
        int   m1 = 0, m2 = (int) MultiFilter::Model::Bypass, routing = 0;
        float c1 = 700.0f, r1 = 0.5f, d1 = 1.8f, bass = 0.3f;
        float c2 = 3000.0f, r2 = 0.2f, d2 = 1.0f, blend = 0.0f;

        bool operator== (const Settings& o) const noexcept
        {
            return m1 == o.m1 && m2 == o.m2 && routing == o.routing && c1 == o.c1 && r1 == o.r1
                && d1 == o.d1 && bass == o.bass && c2 == o.c2 && r2 == o.r2 && d2 == o.d2 && blend == o.blend;
        }
    };

    void setSettings (const Settings& s);
    void paint (juce::Graphics& g) override;

    /** Risposta in dB alle frequenze `hz` (per i test). */
    static void measure (const Settings& s, const std::vector<float>& hz,
                         std::vector<float>& totalDb, std::vector<float>& f1Db, std::vector<float>& f2Db);

private:
    Settings settings;
    bool valid = false;
    std::vector<float> freqs, total, f1, f2;
};

//==============================================================================
/** Pagina filtri: due filtri con routing. */
class FilterPage  : public PageBase,
                    private juce::Timer
{
public:
    explicit FilterPage (juce::AudioProcessorValueTreeState& state);
    void resized() override;

private:
    void timerCallback() override;
    void updateResponse();

    TexturedPanel *panelResponse = nullptr;
    FilterResponse response;
    TexturedPanel *panel1 = nullptr, *panel2 = nullptr, *panelRouting = nullptr;
    juce::ComboBox *model1 = nullptr, *model2 = nullptr;
    juce::ComboBox *envSrc1 = nullptr, *envSrc2 = nullptr, *routing = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilterPage)
};

//==============================================================================
/** Pagina inviluppi: quattro DAHDSR completi con grafico. */
class EnvelopeGraph  : public juce::Component
{
public:
    void setEnvelope (float delay, float attack, float hold, float decay,
                      float sustain, float release,
                      float curveA, float curveD, float curveR);

    void setLiveLevel (float level);

    void paint (juce::Graphics& g) override;

private:
    float d = 0, a = 0.003f, h = 0, dec = 0.25f, s = 0.0f, r = 0.08f;
    float ca = 0, cd = 0, cr = 0;
    float live = 0.0f;
};

class EnvPage  : public PageBase,
                 private juce::Timer
{
public:
    EnvPage (juce::AudioProcessorValueTreeState& state, BassXAudioProcessor& proc);
    void resized() override;

private:
    void timerCallback() override;
    void refreshGraphs();

    struct EnvStrip
    {
        TexturedPanel* panel = nullptr;
        SectionHeader* header = nullptr;
        EnvelopeGraph* graph = nullptr;
        std::array<LabelledKnob*, 10> controls {};
    };

    std::array<EnvStrip, kNumEnvelopes> strips;
    juce::OwnedArray<EnvelopeGraph> graphs;

    BassXAudioProcessor& processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvPage)
};

//==============================================================================
/** Pagina LFO. */
class LfoScope  : public juce::Component
{
public:
    void setShape (int shapeIndex, float phaseOffset, bool unipolar);
    void paint (juce::Graphics& g) override;

private:
    int shape = 0;
    float phase = 0.0f;
    bool uni = false;
};

class LfoPage  : public PageBase,
                 private juce::Timer
{
public:
    explicit LfoPage (juce::AudioProcessorValueTreeState& state);
    void resized() override;

private:
    void timerCallback() override;

    struct LfoStrip
    {
        TexturedPanel* panel = nullptr;
        SectionHeader* header = nullptr;
        LfoScope* scope = nullptr;
        juce::ComboBox* shape = nullptr;
        juce::ComboBox* division = nullptr;
        juce::ComboBox* trigger = nullptr;
        juce::TextButton* sync = nullptr;
        juce::TextButton* unipolar = nullptr;
        std::array<LabelledKnob*, 5> controls {};
    };

    std::array<LfoStrip, kNumLFOs> strips;
    juce::OwnedArray<LfoScope> scopes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LfoPage)
};

//==============================================================================
/** Editor della matrice di modulazione. */
class ModMatrixPage  : public PageBase,
                       private juce::Timer
{
public:
    ModMatrixPage (juce::AudioProcessorValueTreeState& state, ModMatrix& matrix);
    void resized() override;

private:
    void timerCallback() override;
    void refreshFromMatrix();
    void pushRowToMatrix (int row);

    struct Row
    {
        juce::ComboBox source, via, dest;
        juce::Slider depth;
        juce::Label index;
    };

    std::array<Row, kNumModSlots> rows;
    ModMatrix& modMatrix;
    bool updating = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModMatrixPage)
};

//==============================================================================
/** Il file appena esportato: si trascina nella DAW, click = apre la cartella. */
class DragFileTile  : public juce::Component
{
public:
    void setFile (const juce::File& f) { file = f; repaint(); }
    void setStatus (const juce::String& s) { status = s; repaint(); }
    void paint (juce::Graphics& g) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    juce::File file;
    juce::String status { "EXPORT: -" };
    bool dragging = false;
};

/** Editor del sequencer a 16 passi. */
class StepButton;

class SeqPage  : public PageBase,
                 private juce::Timer
{
public:
    SeqPage (juce::AudioProcessorValueTreeState& state, BassXAudioProcessor& proc);
    ~SeqPage() override;

    void resized() override;

private:
    void timerCallback() override;
    void rebuildFromPattern();

    struct StepColumn
    {
        juce::Slider note;
        juce::TextButton gate { "GATE" };
        juce::TextButton accent { "ACC" };
        juce::TextButton slide { "SLD" };
        juce::TextButton tie { "TIE" };
        juce::TextButton ratchet { "x1" };
        juce::Slider probability;
        LedComponent led { LedComponent::Colour::Amber };
        juce::Label number;
        int ratchetValue = 1;
    };

    std::array<StepColumn, kNumSeqSteps> steps;

    TexturedPanel* controlPanel = nullptr;
    juce::ComboBox* division = nullptr;
    juce::ComboBox* direction = nullptr;

    // export del loop in WAV
    juce::ComboBox exportBars;
    juce::TextButton exportButton { "EXPORT WAV" };
    DragFileTile exportTile;
    void startExport();

    BassXAudioProcessor& processor;
    int lastPattern = -1;
    bool updating = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeqPage)
};

//==============================================================================
/** Pagina effetti, con la catena riordinabile in testa. */
class FxPage  : public PageBase
{
public:
    explicit FxPage (juce::AudioProcessorValueTreeState& state);
    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    TexturedPanel *routePanel = nullptr;
    TexturedPanel *distPanel = nullptr, *eqPanel = nullptr, *modPanel = nullptr;
    TexturedPanel *delayPanel = nullptr, *reverbPanel = nullptr, *dynPanel = nullptr;
    TexturedPanel *glitchPanel = nullptr, *repeatPanel = nullptr;

    juce::ComboBox *distAlgo = nullptr, *delayDivL = nullptr, *delayDivR = nullptr;
    juce::ComboBox *glitchRate = nullptr, *glitchMode = nullptr;
    juce::ComboBox *repInterval = nullptr, *repGrid = nullptr, *repGate = nullptr;
    std::array<juce::ComboBox*, kNumFxSlots> slotBoxes {};
    juce::TextButton resetChain { "DEFAULT" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxPage)
};

//==============================================================================
/** Pagina "Fish": la fila di controlli stile Bass Line modificata (Devil Fish)
    più lo stadio neurale e la Ext Mode. */
class FishPage  : public PageBase,
                  private juce::Timer
{
public:
    FishPage (juce::AudioProcessorValueTreeState& state, BassXAudioProcessor& proc);
    void resized() override;

private:
    void timerCallback() override;

    TexturedPanel *linePanel = nullptr, *fishPanel = nullptr, *neuralPanel = nullptr, *scopePanel = nullptr;
    OutputScope scope;
    juce::TextButton* extMode = nullptr;
    LedComponent extLed { LedComponent::Colour::Red };
    LedComponent resoLed { LedComponent::Colour::Red }, accentLed { LedComponent::Colour::Green };
    LedComponent sweepLed { LedComponent::Colour::Red }, accVolLed { LedComponent::Colour::Green };
    BassXAudioProcessor& processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FishPage)
};

//==============================================================================
/** Pagina arpeggiatore e combinazione con il sequencer. */
class ArpPage  : public PageBase,
                 private juce::Timer
{
public:
    ArpPage (juce::AudioProcessorValueTreeState& state, BassXAudioProcessor& proc);
    void resized() override;

private:
    void timerCallback() override;

    TexturedPanel *arpPanel = nullptr, *comboPanel = nullptr, *scopePanel = nullptr;
    OutputScope scope;
    juce::ComboBox *mode = nullptr, *division = nullptr, *accent = nullptr, *combine = nullptr;
    LcdDisplay noteDisplay;
    LedComponent arpLed { LedComponent::Colour::Amber };
    BassXAudioProcessor& processor;
    int lastNote = -2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArpPage)
};

//==============================================================================
/** Goniometro (vettorscopio) e correlazione. */
class Goniometer  : public juce::Component
{
public:
    void setPoints (const std::vector<juce::Point<float>>& pts, float corr);
    void paint (juce::Graphics& g) override;

private:
    std::vector<juce::Point<float>> points;
    float correlation = 1.0f;
};

/** Editor della curva del sidechain: punti liberi, tensione per segmento,
    testina di fase. Toccare una curva di fabbrica la copia in CUSTOM. */
class SidechainCurveEditor  : public juce::Component
{
public:
    explicit SidechainCurveEditor (BassXAudioProcessor& p, juce::AudioProcessorValueTreeState& s)
        : processor (p), apvts (s) {}

    void refresh();            // timer: curva, fase, guadagno
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;

private:
    juce::Point<float> toScreen (float x, float y) const;
    juce::Point<float> fromScreen (juce::Point<float> p) const;
    void beginEdit();          // forma di fabbrica -> CUSTOM
    void push();               // curva modificata -> processore

    BassXAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    SidechainDucker::Curve curve;
    float phase = 0.0f, gain = 1.0f, offset = 0.0f;
    int dragPoint = -1, dragSegment = -1;
    float dragStartTension = 0.0f, dragStartY = 0.0f;
};

/** Spettro prima/dopo il ducking, con la banda che il sidechain taglia e la
    riduzione in tempo reale. */
class SidechainSpectrum  : public juce::Component
{
public:
    explicit SidechainSpectrum (BassXAudioProcessor& p) : processor (p) {}
    void refresh (float bandLowHz, float bandHighHz, bool on);
    void paint (juce::Graphics& g) override;

private:
    BassXAudioProcessor& processor;
    static constexpr int kOrder = 12, kSize = 1 << kOrder;
    juce::dsp::FFT fft { kOrder };
    std::vector<float> work = std::vector<float> ((size_t) kSize * 2);
    std::vector<float> pre = std::vector<float> ((size_t) kSize / 2, -100.0f), post = pre;
    float bandLo = 20.0f, bandHi = 20000.0f, gain = 1.0f;
    bool active = false;
};

class SidechainPage  : public PageBase,
                       private juce::Timer
{
public:
    SidechainPage (juce::AudioProcessorValueTreeState& state, BassXAudioProcessor& proc);
    void resized() override;

private:
    void timerCallback() override;

    TexturedPanel *ctrlPanel = nullptr, *curvePanel = nullptr, *specPanel = nullptr;
    juce::ComboBox *trigger = nullptr, *rate = nullptr, *shape = nullptr;
    SidechainCurveEditor curveEditor;
    SidechainSpectrum spectrum;
    BassXAudioProcessor& processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SidechainPage)
};

//==============================================================================
/** Pagina spazio: panorama, immagine stereo, autopan. */
class SpacePage  : public PageBase,
                   private juce::Timer
{
public:
    SpacePage (juce::AudioProcessorValueTreeState& state, BassXAudioProcessor& proc);
    void resized() override;

private:
    void timerCallback() override;

    TexturedPanel *imagePanel = nullptr, *motionPanel = nullptr, *scopePanel = nullptr;
    juce::ComboBox* autopanDiv = nullptr;
    Goniometer scope;
    std::vector<juce::Point<float>> scratch;
    BassXAudioProcessor& processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpacePage)
};

//==============================================================================
/** Curva di trasferimento della valvola (statica, senza filtri/sag). */
class TubeCurve  : public juce::Component
{
public:
    void setParams (const eta::EngineParams& p, bool on);
    void paint (juce::Graphics& g) override;

private:
    eta::EngineParams params;
    bool active = false;
};

/** Pagina valvola: preamp ETA-1 in testa alla catena effetti. */
class TubePage  : public PageBase,
                  private juce::Timer
{
public:
    TubePage (juce::AudioProcessorValueTreeState& state);
    void resized() override;

private:
    void timerCallback() override;
    void refreshCurve();

    TexturedPanel *valvePanel = nullptr, *tonePanel = nullptr, *curvePanel = nullptr;
    juce::ComboBox* model = nullptr;
    TubeCurve curve;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TubePage)
};

} // namespace bassx
