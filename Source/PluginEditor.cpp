#include "PluginEditor.h"

#include <cmath>

void RandomRadioFXAudioProcessorEditor::StationMeter::setLevel (float newLevel)
{
    const auto clipped = juce::jlimit (0.0f, 1.0f, newLevel);
    if (std::abs (clipped - level) < 0.001f)
        return;

    level = clipped;
    repaint();
}

void RandomRadioFXAudioProcessorEditor::StationMeter::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff1b1a15));
    g.fillRoundedRectangle (area, 3.0f);
    g.setColour (juce::Colour (0xff8f8a6a).withAlpha (0.6f));
    g.drawRoundedRectangle (area.reduced (0.5f), 3.0f, 1.0f);

    auto fill = area.reduced (3.0f);
    fill.setWidth (fill.getWidth() * juce::jlimit (0.0f, 1.0f, std::pow (level, 0.6f)));
    const auto amber = juce::Colour::fromRGB (242, 167, 59);
    const auto hot = juce::Colour::fromRGB (255, 230, 146);
    juce::ColourGradient glow (amber, fill.getX(), fill.getCentreY(), hot, fill.getRight(), fill.getCentreY(), false);
    g.setGradientFill (glow);
    g.fillRoundedRectangle (fill, 2.0f);

    g.setColour (juce::Colours::black.withAlpha (0.35f));
    for (int x = static_cast<int> (area.getX()) + 8; x < static_cast<int> (area.getRight()); x += 14)
        g.drawVerticalLine (x, area.getY() + 2.0f, area.getBottom() - 2.0f);
}

RandomRadioFXAudioProcessorEditor::RandomRadioFXAudioProcessorEditor (RandomRadioFXAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    const std::array<std::pair<const char*, const char*>, 13> controls
    {{
        { "switchSec", "Switch (s)" },
        { "crossfadeMs", "Crossfade (ms)" },
        { "wet", "FX Wet" },
        { "liveInMix", "Live In" },
        { "stutterMix", "Stutter Mix" },
        { "stutterRate", "Stutter Rate" },
        { "grainMix", "Granular Mix" },
        { "grainSizeMs", "Grain Size" },
        { "grainDensity", "Grain Density" },
        { "microMix", "Micro Mix" },
        { "microMs", "Micro Length" },
        { "softCrush", "Soft Crush" },
        { "captureOnRecord", "Capture Rec" }
    }};

    sliders.ensureStorageAllocated (static_cast<int> (controls.size()));
    labels.ensureStorageAllocated (static_cast<int> (controls.size()));
    attachments.reserve (controls.size());

    for (const auto& [paramId, title] : controls)
    {
        auto* s = sliders.add (new juce::Slider());
        s->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 20);
        s->setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xffc49639));
        s->setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff302923));
        s->setColour (juce::Slider::thumbColourId, juce::Colour (0xfff0dd8f));
        s->setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff4b4236));
        s->setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffece7d5));
        s->setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff252018));
        addAndMakeVisible (s);

        auto* l = labels.add (new juce::Label());
        l->setText (title, juce::dontSendNotification);
        l->setJustificationType (juce::Justification::centred);
        l->setColour (juce::Label::textColourId, juce::Colour (0xffece1bf));
        l->setFont (juce::Font ("American Typewriter", 14.0f, juce::Font::bold));
        addAndMakeVisible (l);

        attachments.push_back (std::make_unique<Attachment> (audioProcessor.getValueTreeState(), paramId, *s));
    }

    stationLabels.ensureStorageAllocated (RandomRadioFXAudioProcessor::meterStationCount);
    stationMeters.ensureStorageAllocated (RandomRadioFXAudioProcessor::meterStationCount);

    for (int i = 0; i < RandomRadioFXAudioProcessor::meterStationCount; ++i)
    {
        auto* label = stationLabels.add (new juce::Label());
        label->setText (audioProcessor.getStationMeterName (i), juce::dontSendNotification);
        label->setJustificationType (juce::Justification::centredLeft);
        label->setColour (juce::Label::textColourId, juce::Colour (0xffd5c7a0));
        label->setFont (juce::Font ("Courier New", 13.0f, juce::Font::plain));
        addAndMakeVisible (label);

        auto* meter = stationMeters.add (new StationMeter());
        addAndMakeVisible (meter);
    }

    nextButton.onClick = [this] { audioProcessor.nextStationNow(); };
    nextButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff9b6a28));
    nextButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffb8873c));
    nextButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff4eacb));
    nextButton.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff21180f));
    nextButton.setClickingTogglesState (false);
    addAndMakeVisible (nextButton);

    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xfff0d992));
    statusLabel.setFont (juce::Font ("Courier New", 14.0f, juce::Font::bold));
    addAndMakeVisible (statusLabel);

    startTimerHz (20);
    setSize (1080, 680);
}

RandomRadioFXAudioProcessorEditor::~RandomRadioFXAudioProcessorEditor() = default;

void RandomRadioFXAudioProcessorEditor::paint (juce::Graphics& g)
{
    juce::ColourGradient bg (juce::Colour (0xff262018), 0.0f, 0.0f,
                             juce::Colour (0xff0f0d0a), 0.0f, static_cast<float> (getHeight()), false);
    g.setGradientFill (bg);
    g.fillAll();

    auto panel = getLocalBounds().toFloat().reduced (10.0f);
    juce::ColourGradient brass (juce::Colour (0xff6c5a37), panel.getX(), panel.getY(),
                                juce::Colour (0xff4c3f27), panel.getRight(), panel.getBottom(), false);
    g.setGradientFill (brass);
    g.fillRoundedRectangle (panel, 8.0f);
    g.setColour (juce::Colour (0xff2a2218));
    g.drawRoundedRectangle (panel, 8.0f, 2.0f);

    for (int i = 0; i < 12; ++i)
    {
        const float x = panel.getX() + 14.0f + static_cast<float> (i % 6) * ((panel.getWidth() - 28.0f) / 5.0f);
        const float y = panel.getY() + 14.0f + static_cast<float> (i / 6) * (panel.getHeight() - 28.0f);
        g.setColour (juce::Colour (0xff231c12));
        g.fillEllipse (x, y, 8.0f, 8.0f);
        g.setColour (juce::Colour (0xff9f9278).withAlpha (0.6f));
        g.drawEllipse (x, y, 8.0f, 8.0f, 1.0f);
    }

    g.setColour (juce::Colour (0xfff0dfae));
    g.setFont (juce::Font ("American Typewriter", 18.0f, juce::Font::bold));
    g.drawText ("matd.space", 24, 14, 220, 28, juce::Justification::topLeft, false);
}

void RandomRadioFXAudioProcessorEditor::resized()
{
    const int margin = 24;
    const int gap = 12;

    auto area = getLocalBounds().reduced (margin);

    auto top = area.removeFromTop (44);
    auto buttonArea = top.removeFromRight (180).reduced (6, 4);
    nextButton.setBounds (buttonArea);
    statusLabel.setBounds (top.reduced (16, 0));

    area.removeFromTop (10);

    auto controls = area.removeFromTop (290);
    const int cols = 6;
    const int rows = (sliders.size() + cols - 1) / cols;
    const int cellW = (controls.getWidth() - gap * (cols - 1)) / cols;
    const int cellH = (controls.getHeight() - gap * (rows - 1)) / rows;

    for (int i = 0; i < sliders.size(); ++i)
    {
        const int x = i % cols;
        const int y = i / cols;
        auto cell = juce::Rectangle<int> (controls.getX() + x * (cellW + gap), controls.getY() + y * (cellH + gap), cellW, cellH);
        labels[i]->setBounds (cell.removeFromTop (24));
        sliders[i]->setBounds (cell.reduced (6));
    }

    area.removeFromTop (14);

    const int meterRows = 10;
    const int meterCellH = (area.getHeight() - gap * (meterRows - 1)) / meterRows;
    for (int i = 0; i < RandomRadioFXAudioProcessor::meterStationCount; ++i)
    {
        auto row = area.removeFromTop (meterCellH);
        stationLabels[i]->setBounds (row.removeFromLeft (220));
        stationMeters[i]->setBounds (row.reduced (2, 2));
        area.removeFromTop (gap);
    }
}

void RandomRadioFXAudioProcessorEditor::timerCallback()
{
    statusLabel.setText ("Status: " + audioProcessor.getConnectionStatus(), juce::dontSendNotification);

    for (int i = 0; i < RandomRadioFXAudioProcessor::meterStationCount; ++i)
        stationMeters[i]->setLevel (audioProcessor.getStationMeterLevel (i));
}
