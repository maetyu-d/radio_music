#pragma once

#include <array>
#include <atomic>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

extern "C"
{
#include <vlc/vlc.h>
}

class RandomRadioFXAudioProcessor : public juce::AudioProcessor,
                                    private juce::Thread
{
public:
    static constexpr int meterStationCount = 10;

    RandomRadioFXAudioProcessor();
    ~RandomRadioFXAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getValueTreeState() { return parameters; }
    float getStationMeterLevel (int stationIndex) const;
    juce::String getStationMeterName (int stationIndex) const;
    juce::String getConnectionStatus() const;
    void nextStationNow();

private:
    struct Station
    {
        const char* name;
        const char* url;
    };
    struct FxPreset
    {
        const char* name;
        float wet;
        float stutterMix;
        float stutterRate;
        float grainMix;
        float grainSizeMs;
        float grainDensity;
        float microMix;
        float microMs;
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void run() override;
    void queueStationLoad (int stationIndex);
    void startStationPlayback (int stationIndex);
    int chooseRandomStationExcluding (int exclude);
    int chooseRandomPresetExcluding (int exclude);
    void applyPreset (int presetIndex);
    juce::String getActivePresetName() const;

    void processEffects (juce::AudioBuffer<float>& buffer);

    static void vlcPlayCallback (void* data, const void* samples, unsigned int count, int64_t pts);
    static void vlcPauseCallback (void* data, int64_t pts);
    static void vlcResumeCallback (void* data, int64_t pts);
    static void vlcFlushCallback (void* data, int64_t pts);
    static void vlcDrainCallback (void* data);
    void handleVlcSamples (const void* samples, int frameCount);
    bool ensureVlcInitialized();
    void shutdownVlc();

    juce::AudioProcessorValueTreeState parameters;

    libvlc_instance_t* vlcInstance = nullptr;
    libvlc_media_player_t* vlcPlayer = nullptr;
    juce::CriticalSection vlcLock;

    juce::CriticalSection requestLock;
    int requestedStation = -1;
    bool hasPendingRequest = false;

    juce::WaitableEvent requestWake;

    juce::AbstractFifo audioFifo { 48000 * 20 };
    std::vector<float> audioRing;

    mutable juce::CriticalSection statusLock;
    juce::String connectionStatus { "Initializing libVLC..." };

    int activeStationIndex = -1;
    int activePresetIndex = -1;
    int64_t samplesSinceConnectRequest = 0;
    bool waitingForAudio = false;
    bool liveInputMode = false;

    int64_t switchIntervalSamples = 0;
    int64_t samplesUntilSwitch = 0;
    double currentSampleRate = 44100.0;
    bool playbackPrepared = false;

    juce::AudioBuffer<float> workingBuffer;
    juce::AudioBuffer<float> dryBuffer;

    juce::AudioBuffer<float> historyBuffer;
    int historyWritePos = 0;
    int historySize = 0;

    bool stutterActive = false;
    int stutterRemaining = 0;
    int stutterLength = 0;
    int stutterReadPos = 0;

    bool grainActive = false;
    int grainStartPos = 0;
    int grainLength = 0;
    int grainPos = 0;

    int microStartPos = 0;
    int microLength = 0;
    int microReadPos = 0;
    int microRefreshCountdown = 0;

    bool stretchActive = false;
    int stretchRemaining = 0;
    float stretchReadPos = 0.0f;
    float stretchRate = 1.0f;
    float stretchMix = 0.0f;

    std::array<std::atomic<float>, meterStationCount> stationMeterLevels {};

    juce::Random random;

    const std::array<Station, meterStationCount> stations
    {{
        { "Groove Salad", "https://ice1.somafm.com/groovesalad-128-mp3" },
        { "Drone Zone", "https://ice4.somafm.com/dronezone-128-mp3" },
        { "Secret Agent", "https://ice2.somafm.com/secretagent-128-mp3" },
        { "DEF CON Radio", "https://ice5.somafm.com/defcon-128-mp3" },
        { "Beat Blender", "https://ice2.somafm.com/beatblender-128-mp3" },
        { "Lush", "https://ice2.somafm.com/lush-128-mp3" },
        { "Underground 80s", "https://ice4.somafm.com/u80s-128-mp3" },
        { "Mission Control", "https://ice1.somafm.com/missioncontrol-128-mp3" },
        { "The Trip", "https://ice2.somafm.com/thetrip-128-mp3" },
        { "Radio Paradise", "https://stream.radioparadise.com/mp3-192" }
    }};

    static constexpr int presetCount = 20;
    const std::array<FxPreset, presetCount> presets
    {{
        { "Aeolian Lattice", 0.62f, 0.05f, 220.0f, 0.16f, 28.0f, 240.0f, 0.12f, 28.0f },
        { "Glass Arp Drift", 0.58f, 0.04f, 180.0f, 0.20f, 22.0f, 320.0f, 0.14f, 24.0f },
        { "Minor Orbit", 0.64f, 0.06f, 260.0f, 0.14f, 34.0f, 180.0f, 0.18f, 36.0f },
        { "Harmonic Relay", 0.60f, 0.03f, 140.0f, 0.24f, 18.0f, 420.0f, 0.10f, 22.0f },
        { "Dorian Bloom", 0.66f, 0.07f, 300.0f, 0.18f, 26.0f, 300.0f, 0.16f, 30.0f },
        { "Soft Chopper", 0.56f, 0.08f, 340.0f, 0.12f, 40.0f, 150.0f, 0.09f, 42.0f },
        { "Velvet Partials", 0.63f, 0.04f, 160.0f, 0.22f, 20.0f, 360.0f, 0.13f, 26.0f },
        { "Tape Constellation", 0.61f, 0.05f, 210.0f, 0.19f, 24.0f, 280.0f, 0.15f, 32.0f },
        { "Panned Harmonics", 0.59f, 0.06f, 250.0f, 0.17f, 30.0f, 230.0f, 0.11f, 27.0f },
        { "Nocturne Pulses", 0.65f, 0.05f, 230.0f, 0.21f, 21.0f, 390.0f, 0.20f, 34.0f },
        { "Resin Echo", 0.57f, 0.03f, 130.0f, 0.23f, 19.0f, 440.0f, 0.08f, 25.0f },
        { "Interval Garden", 0.62f, 0.06f, 270.0f, 0.15f, 33.0f, 200.0f, 0.17f, 38.0f },
        { "Warm Spectra", 0.60f, 0.04f, 170.0f, 0.20f, 23.0f, 330.0f, 0.12f, 29.0f },
        { "Clocked Petals", 0.55f, 0.07f, 320.0f, 0.13f, 36.0f, 170.0f, 0.10f, 40.0f },
        { "Cloud Counterpoint", 0.67f, 0.05f, 240.0f, 0.25f, 16.0f, 460.0f, 0.19f, 23.0f },
        { "Prismatic Choir", 0.63f, 0.06f, 280.0f, 0.18f, 27.0f, 260.0f, 0.14f, 31.0f },
        { "Fifth Spiral", 0.58f, 0.05f, 200.0f, 0.21f, 22.0f, 350.0f, 0.09f, 26.0f },
        { "Tuned Fragments", 0.61f, 0.07f, 310.0f, 0.16f, 29.0f, 210.0f, 0.15f, 35.0f },
        { "Silver Motif", 0.59f, 0.04f, 150.0f, 0.24f, 18.0f, 430.0f, 0.11f, 21.0f },
        { "Midnight Cadence", 0.66f, 0.06f, 290.0f, 0.19f, 25.0f, 300.0f, 0.18f, 33.0f }
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RandomRadioFXAudioProcessor)
};
