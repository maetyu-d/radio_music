#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "PluginProcessor.h"

class RandomRadioFXAudioProcessorEditor : public juce::AudioProcessorEditor,
                                          private juce::Timer
{
public:
    explicit RandomRadioFXAudioProcessorEditor (RandomRadioFXAudioProcessor&);
    ~RandomRadioFXAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class StationMeter : public juce::Component
    {
    public:
        void setLevel (float newLevel);
        void paint (juce::Graphics&) override;

    private:
        float level = 0.0f;
    };

    void timerCallback() override;

    using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    RandomRadioFXAudioProcessor& audioProcessor;

    juce::OwnedArray<juce::Slider> sliders;
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::Label> stationLabels;
    juce::OwnedArray<StationMeter> stationMeters;
    std::vector<std::unique_ptr<Attachment>> attachments;

    juce::TextButton nextButton { "Next Station" };
    juce::Label statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RandomRadioFXAudioProcessorEditor)
};
