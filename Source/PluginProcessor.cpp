#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>
#include <cstdlib>

RandomRadioFXAudioProcessor::RandomRadioFXAudioProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      Thread ("Radio loader"),
      parameters (static_cast<juce::AudioProcessor&> (*this), nullptr, juce::Identifier ("RandomRadioFX"), createParameterLayout())
{
    audioRing.resize (static_cast<size_t> (audioFifo.getTotalSize() * 2), 0.0f);
    connectionStatus = "Ready (waiting for playback start)";

    captureThread.startThread();
    startThread();
}

RandomRadioFXAudioProcessor::~RandomRadioFXAudioProcessor()
{
    signalThreadShouldExit();
    requestWake.signal();
    stopThread (3000);
    stopCapture();
    captureThread.stopThread (2000);
    shutdownVlc();
}

const juce::String RandomRadioFXAudioProcessor::getName() const { return JucePlugin_Name; }
bool RandomRadioFXAudioProcessor::acceptsMidi() const { return false; }
bool RandomRadioFXAudioProcessor::producesMidi() const { return false; }
bool RandomRadioFXAudioProcessor::isMidiEffect() const { return false; }
double RandomRadioFXAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int RandomRadioFXAudioProcessor::getNumPrograms() { return 1; }
int RandomRadioFXAudioProcessor::getCurrentProgram() { return 0; }
void RandomRadioFXAudioProcessor::setCurrentProgram (int) {}
const juce::String RandomRadioFXAudioProcessor::getProgramName (int) { return {}; }
void RandomRadioFXAudioProcessor::changeProgramName (int, const juce::String&) {}

void RandomRadioFXAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    playbackPrepared = true;
    currentSampleRate = sampleRate;
    switchIntervalSamples = static_cast<int64_t> (sampleRate * parameters.getRawParameterValue ("switchSec")->load());
    samplesUntilSwitch = switchIntervalSamples;

    workingBuffer.setSize (2, samplesPerBlock);
    dryBuffer.setSize (2, samplesPerBlock);

    historySize = static_cast<int> (sampleRate * 3.0);
    historyBuffer.setSize (2, historySize);
    historyBuffer.clear();
    historyWritePos = 0;

    stutterActive = false;
    grainActive = false;
    microReadPos = 0;
    stretchActive = false;
    stretchRemaining = 0;
    stretchReadPos = 0.0f;
    stretchRate = 1.0f;
    stretchMix = 0.0f;
    liveInputMode = false;

    for (auto& m : stationMeterLevels)
        m.store (0.0f, std::memory_order_relaxed);

    audioFifo.reset();

    queueStationLoad (chooseRandomStationExcluding (-1));
}

void RandomRadioFXAudioProcessor::releaseResources()
{
}

bool RandomRadioFXAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo())
        && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
        && in == out;
}

void RandomRadioFXAudioProcessor::queueStationLoad (int stationIndex)
{
    if (! juce::isPositiveAndBelow (stationIndex, meterStationCount))
        return;

    const juce::ScopedLock lock (requestLock);
    requestedStation = stationIndex;
    hasPendingRequest = true;
    requestWake.signal();
}

int RandomRadioFXAudioProcessor::chooseRandomStationExcluding (int exclude)
{
    juce::Array<int> choices;
    for (int i = 0; i < meterStationCount; ++i)
        if (i != exclude)
            choices.add (i);

    if (choices.isEmpty())
        return 0;

    return choices[random.nextInt (choices.size())];
}

int RandomRadioFXAudioProcessor::chooseRandomPresetExcluding (int exclude)
{
    juce::Array<int> choices;
    for (int i = 0; i < presetCount; ++i)
        if (i != exclude)
            choices.add (i);

    if (choices.isEmpty())
        return 0;

    return choices[random.nextInt (choices.size())];
}

void RandomRadioFXAudioProcessor::applyPreset (int presetIndex)
{
    if (! juce::isPositiveAndBelow (presetIndex, presetCount))
        return;

    const auto& p = presets[static_cast<size_t> (presetIndex)];
    auto setParam = [this] (const char* paramId, float value)
    {
        if (auto* param = parameters.getParameter (paramId))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
    };

    setParam ("wet", p.wet);
    setParam ("stutterMix", p.stutterMix);
    setParam ("stutterRate", p.stutterRate);
    setParam ("grainMix", p.grainMix);
    setParam ("grainSizeMs", p.grainSizeMs);
    setParam ("grainDensity", p.grainDensity);
    setParam ("microMix", p.microMix);
    setParam ("microMs", p.microMs);

    activePresetIndex = presetIndex;
}

juce::String RandomRadioFXAudioProcessor::getActivePresetName() const
{
    if (! juce::isPositiveAndBelow (activePresetIndex, presetCount))
        return "No preset";

    return presets[static_cast<size_t> (activePresetIndex)].name;
}

void RandomRadioFXAudioProcessor::startStationPlayback (int stationIndex)
{
    if (! playbackPrepared)
        return;

    if (! ensureVlcInitialized())
        return;

    const juce::ScopedLock vlcGuard (vlcLock);

    const auto& station = stations[static_cast<size_t> (stationIndex)];

    const auto presetIndex = chooseRandomPresetExcluding (activePresetIndex);
    applyPreset (presetIndex);

    {
        const juce::ScopedLock lock (statusLock);
        connectionStatus = "Connecting: " + juce::String (station.name)
                         + " | Preset: " + getActivePresetName();
    }

    libvlc_media_player_stop (vlcPlayer);

    libvlc_media_t* media = libvlc_media_new_location (vlcInstance, station.url);
    if (media == nullptr)
    {
        const juce::ScopedLock lock (statusLock);
        connectionStatus = "Media creation failed: " + juce::String (station.name);
        return;
    }

    libvlc_media_player_set_media (vlcPlayer, media);
    libvlc_media_release (media);

    if (libvlc_media_player_play (vlcPlayer) != 0)
    {
        const juce::ScopedLock lock (statusLock);
        connectionStatus = "Playback start failed: " + juce::String (station.name);
        return;
    }

    activeStationIndex = stationIndex;
    waitingForAudio = true;
    samplesSinceConnectRequest = 0;
    audioFifo.reset();
}

void RandomRadioFXAudioProcessor::run()
{
    while (! threadShouldExit())
    {
        requestWake.wait (300);
        if (threadShouldExit())
            break;

        int stationToLoad = -1;
        {
            const juce::ScopedLock lock (requestLock);
            if (! hasPendingRequest)
                continue;

            stationToLoad = requestedStation;
            hasPendingRequest = false;
        }

        if (juce::isPositiveAndBelow (stationToLoad, meterStationCount))
            startStationPlayback (stationToLoad);
    }
}

bool RandomRadioFXAudioProcessor::ensureVlcInitialized()
{
    const juce::ScopedLock vlcGuard (vlcLock);

    if (vlcInstance != nullptr && vlcPlayer != nullptr)
        return true;

    const auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    const auto appContentsDir = exeDir.getParentDirectory();
    const auto bundledPlugins = appContentsDir.getChildFile ("Frameworks/plugins");
    const auto systemPlugins = juce::File ("/Applications/VLC.app/Contents/MacOS/plugins");
    const auto pluginPath = bundledPlugins.isDirectory() ? bundledPlugins.getFullPathName()
                                                         : systemPlugins.getFullPathName();
    setenv ("VLC_PLUGIN_PATH", pluginPath.toRawUTF8(), 1);

    const char* const args[]
    {
        "--no-video",
        "--intf=dummy",
        "--network-caching=350",
        "--http-reconnect",
        "--no-metadata-network-access",
        "--quiet"
    };

    vlcInstance = libvlc_new (static_cast<int> (std::size (args)), args);
    if (vlcInstance == nullptr)
    {
        const auto* err = libvlc_errmsg();
        const juce::ScopedLock lock (statusLock);
        connectionStatus = err != nullptr ? "libVLC init failed: " + juce::String (err)
                                          : "libVLC init failed";
        return false;
    }

    vlcPlayer = libvlc_media_player_new (vlcInstance);
    if (vlcPlayer == nullptr)
    {
        const juce::ScopedLock lock (statusLock);
        connectionStatus = "libVLC player creation failed";
        shutdownVlc();
        return false;
    }

    libvlc_audio_set_callbacks (vlcPlayer,
                                &RandomRadioFXAudioProcessor::vlcPlayCallback,
                                &RandomRadioFXAudioProcessor::vlcPauseCallback,
                                &RandomRadioFXAudioProcessor::vlcResumeCallback,
                                &RandomRadioFXAudioProcessor::vlcFlushCallback,
                                &RandomRadioFXAudioProcessor::vlcDrainCallback,
                                this);
    libvlc_audio_set_format (vlcPlayer, "S16N", static_cast<unsigned int> (juce::jlimit (22050.0, 192000.0, currentSampleRate)), 2);

    const juce::ScopedLock lock (statusLock);
    connectionStatus = "libVLC ready";
    return true;
}

void RandomRadioFXAudioProcessor::shutdownVlc()
{
    const juce::ScopedLock vlcGuard (vlcLock);

    if (vlcPlayer != nullptr)
    {
        libvlc_media_player_stop (vlcPlayer);
        libvlc_media_player_release (vlcPlayer);
        vlcPlayer = nullptr;
    }

    if (vlcInstance != nullptr)
    {
        libvlc_release (vlcInstance);
        vlcInstance = nullptr;
    }
}

void RandomRadioFXAudioProcessor::startCapture()
{
    if (captureWriter != nullptr || currentSampleRate <= 0.0)
        return;

    auto dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                    .getChildFile ("Radio Music Captures");
    dir.createDirectory();

    const auto fileName = "RadioMusic-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".wav";
    currentCaptureFile = dir.getChildFile (fileName);

    std::unique_ptr<juce::FileOutputStream> stream (currentCaptureFile.createOutputStream());
    if (stream == nullptr)
        return;

    juce::WavAudioFormat wav;
    auto* writer = wav.createWriterFor (stream.release(),
                                        currentSampleRate,
                                        2,
                                        24,
                                        {},
                                        0);
    if (writer == nullptr)
        return;

    const juce::ScopedLock lock (captureLock);
    captureWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer, captureThread, 32768);
}

void RandomRadioFXAudioProcessor::stopCapture()
{
    const juce::ScopedLock lock (captureLock);
    captureWriter.reset();
}

bool RandomRadioFXAudioProcessor::isHostRecording() const
{
    if (auto* playHead = getPlayHead(); playHead != nullptr)
    {
        juce::AudioPlayHead::CurrentPositionInfo pos;
        if (playHead->getCurrentPosition (pos))
            return pos.isRecording;
    }

    return false;
}

void RandomRadioFXAudioProcessor::vlcPlayCallback (void* data, const void* samples, unsigned int count, int64_t)
{
    if (data == nullptr || samples == nullptr || count == 0)
        return;

    static_cast<RandomRadioFXAudioProcessor*> (data)->handleVlcSamples (samples, static_cast<int> (count));
}

void RandomRadioFXAudioProcessor::vlcPauseCallback (void*, int64_t) {}
void RandomRadioFXAudioProcessor::vlcResumeCallback (void*, int64_t) {}
void RandomRadioFXAudioProcessor::vlcFlushCallback (void* data, int64_t)
{
    if (data == nullptr)
        return;

    static_cast<RandomRadioFXAudioProcessor*> (data)->audioFifo.reset();
}

void RandomRadioFXAudioProcessor::vlcDrainCallback (void* data)
{
    if (data == nullptr)
        return;

    static_cast<RandomRadioFXAudioProcessor*> (data)->audioFifo.reset();
}

void RandomRadioFXAudioProcessor::handleVlcSamples (const void* samples, int frameCount)
{
    if (frameCount <= 0)
        return;

    int sampleCount = frameCount * 2;

    if ((sampleCount & 1) != 0)
        --sampleCount;

    if (sampleCount <= 0)
        return;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    audioFifo.prepareToWrite (sampleCount, start1, size1, start2, size2);

    const auto* pcm = reinterpret_cast<const int16_t*> (samples);
    constexpr float s16Scale = 1.0f / 32768.0f;

    if (size1 > 0)
    {
        auto* dst = audioRing.data() + static_cast<size_t> (start1);
        for (int i = 0; i < size1; ++i)
            dst[i] = static_cast<float> (pcm[i]) * s16Scale;
    }

    if (size2 > 0)
    {
        auto* dst = audioRing.data() + static_cast<size_t> (start2);
        for (int i = 0; i < size2; ++i)
            dst[i] = static_cast<float> (pcm[size1 + i]) * s16Scale;
    }

    audioFifo.finishedWrite (size1 + size2);

    if (waitingForAudio)
    {
        waitingForAudio = false;
        const juce::ScopedLock lock (statusLock);
        if (juce::isPositiveAndBelow (activeStationIndex, meterStationCount))
            connectionStatus = "Connected: " + juce::String (stations[static_cast<size_t> (activeStationIndex)].name)
                             + " | Preset: " + getActivePresetName();
    }
}

void RandomRadioFXAudioProcessor::processEffects (juce::AudioBuffer<float>& buffer)
{
    const auto channels = buffer.getNumChannels();
    const auto samples = buffer.getNumSamples();

    dryBuffer.makeCopyOf (buffer, true);

    const auto wet = parameters.getRawParameterValue ("wet")->load();
    const auto stutterMix = parameters.getRawParameterValue ("stutterMix")->load();
    const auto stutterRate = parameters.getRawParameterValue ("stutterRate")->load();
    const auto grainMix = parameters.getRawParameterValue ("grainMix")->load();
    const auto grainSizeMs = parameters.getRawParameterValue ("grainSizeMs")->load();
    const auto grainDensity = parameters.getRawParameterValue ("grainDensity")->load();
    const auto microMix = parameters.getRawParameterValue ("microMix")->load();
    const auto microMs = parameters.getRawParameterValue ("microMs")->load();
    const auto crush = juce::jlimit (0.0f, 1.0f, parameters.getRawParameterValue ("softCrush")->load());

    constexpr float extremeMul = 10000.0f;
    const auto stutterMixX = juce::jlimit (0.0f, 1.0f, std::pow (stutterMix, 2.0f));
    const auto grainMixX = juce::jlimit (0.0f, 1.0f, std::pow (grainMix, 2.0f));
    const auto microMixX = juce::jlimit (0.0f, 1.0f, std::pow (microMix, 2.0f));

    microLength = juce::jlimit (1, historySize / 6, static_cast<int> (currentSampleRate * microMs / 1000.0f));

    for (int s = 0; s < samples; ++s)
    {
        if (microRefreshCountdown <= 0)
        {
            microStartPos = (historyWritePos - microLength + historySize) % historySize;
            microReadPos = 0;
            const auto refreshSeconds = juce::jmap (microMixX, 0.02f, 0.00005f);
            microRefreshCountdown = juce::jmax (1, static_cast<int> (refreshSeconds * currentSampleRate / extremeMul));
        }
        --microRefreshCountdown;

        if (! stutterActive && stutterMixX > 0.001f)
        {
            const auto p = juce::jlimit (0.0f, 1.0f, (stutterRate * extremeMul) / static_cast<float> (currentSampleRate * 0.12f));
            if (random.nextFloat() < p)
            {
                stutterLength = juce::jlimit (1, historySize / 8, static_cast<int> ((0.0002 + 0.01 * random.nextFloat()) * currentSampleRate));
                stutterReadPos = (historyWritePos - stutterLength + historySize) % historySize;
                stutterRemaining = static_cast<int> ((0.002 + 3.5 * random.nextFloat()) * currentSampleRate);
                stutterActive = true;
            }
        }

        if (! grainActive && grainMixX > 0.001f)
        {
            const auto p = juce::jlimit (0.0f, 1.0f, (grainDensity * extremeMul) / static_cast<float> (currentSampleRate * 0.08f));
            if (random.nextFloat() < p)
            {
                grainLength = juce::jlimit (1, historySize / 10, static_cast<int> (currentSampleRate * grainSizeMs / 1000.0f));
                grainStartPos = (historyWritePos - random.nextInt (juce::jmax (grainLength, static_cast<int> (0.5 * currentSampleRate))) + historySize) % historySize;
                grainPos = 0;
                grainActive = true;
            }
        }

        if (! stretchActive)
        {
            const auto p = juce::jlimit (0.0f, 1.0f, 1.0f / static_cast<float> (currentSampleRate * 0.06));
            if (random.nextFloat() < p)
            {
                stretchActive = true;
                stretchRate = random.nextFloat() < 0.55f
                                ? juce::jmap (random.nextFloat(), 0.005f, 0.45f)
                                : juce::jmap (random.nextFloat(), 2.5f, 18.0f);
                stretchRemaining = static_cast<int> ((0.25f + random.nextFloat() * 4.0f) * currentSampleRate);
                stretchReadPos = static_cast<float> ((historyWritePos - random.nextInt (juce::jmax (1, static_cast<int> (0.6 * currentSampleRate))) + historySize) % historySize);
                stretchMix = juce::jmap (random.nextFloat(), 0.55f, 1.0f);
            }
        }

        for (int c = 0; c < channels; ++c)
        {
            const auto in = buffer.getSample (c, s);
            historyBuffer.setSample (c, historyWritePos, in);

            float fxSample = in;

            if (stutterActive)
            {
                if (random.nextFloat() < (0.02f + 0.85f * stutterMixX))
                    stutterReadPos = random.nextInt (historySize);
                fxSample = juce::jmap (stutterMixX, fxSample, historyBuffer.getSample (c, stutterReadPos));
            }

            if (grainActive)
            {
                const auto readPos = (grainStartPos + grainPos) % historySize;
                const auto env = std::sin (juce::MathConstants<float>::pi * static_cast<float> (grainPos) / static_cast<float> (juce::jmax (1, grainLength)));
                fxSample = juce::jmap (grainMixX, fxSample, historyBuffer.getSample (c, readPos) * env);

                if (random.nextFloat() < (0.01f + 0.9f * grainMixX))
                    grainStartPos = random.nextInt (historySize);
            }

            if (microMixX > 0.001f)
            {
                const auto readPos = (microStartPos + microReadPos) % historySize;
                auto microSample = historyBuffer.getSample (c, readPos);
                if (random.nextFloat() < (0.02f + 0.9f * microMixX))
                    microSample = -microSample;
                fxSample = juce::jmap (microMixX, fxSample, microSample);
            }

            if (stretchActive)
            {
                const auto rp = stretchReadPos;
                const auto i0 = static_cast<int> (rp) % historySize;
                const auto i1 = (i0 + 1) % historySize;
                const auto frac = rp - static_cast<float> (i0);
                const auto s0 = historyBuffer.getSample (c, i0);
                const auto s1 = historyBuffer.getSample (c, i1);
                const auto stretchSample = juce::jmap (frac, s0, s1);
                fxSample = juce::jmap (stretchMix, fxSample, stretchSample);
            }

            if (crush > 0.0001f)
            {
                const auto bits = 16.0f - (14.0f * crush);
                const auto levels = std::pow (2.0f, juce::jlimit (2.0f, 16.0f, bits) - 1.0f);
                const auto step = 1.0f / juce::jmax (2.0f, levels);
                const auto crushed = std::round (fxSample / step) * step;
                const auto softCrushed = std::tanh (crushed * (1.0f + 2.0f * crush));
                fxSample = juce::jmap (crush, fxSample, softCrushed);
            }

            const auto wetSample = juce::jmap (wet, dryBuffer.getSample (c, s), fxSample);
            buffer.setSample (c, s, std::tanh (wetSample * 8.0f));
        }

        historyWritePos = (historyWritePos + 1) % historySize;

        if (stutterActive)
        {
            stutterReadPos = (stutterReadPos + 1) % historySize;
            if (--stutterRemaining <= 0)
                stutterActive = false;
        }

        if (grainActive && ++grainPos >= grainLength)
            grainActive = false;

        const auto step = 1 + static_cast<int> (microMixX * 64.0f * random.nextFloat());
        const auto signedStep = random.nextFloat() < (0.45f * microMixX) ? -step : step;
        microReadPos = (microReadPos + signedStep + juce::jmax (1, microLength) * 16) % juce::jmax (1, microLength);

        if (stretchActive)
        {
            stretchReadPos += stretchRate;
            while (stretchReadPos >= static_cast<float> (historySize))
                stretchReadPos -= static_cast<float> (historySize);
            while (stretchReadPos < 0.0f)
                stretchReadPos += static_cast<float> (historySize);

            if (--stretchRemaining <= 0)
                stretchActive = false;
        }
    }
}

void RandomRadioFXAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto captureEnabled = parameters.getRawParameterValue ("captureOnRecord")->load() > 0.5f;
    if (captureEnabled && isHostRecording())
        startCapture();
    else
        stopCapture();

    switchIntervalSamples = static_cast<int64_t> (currentSampleRate * parameters.getRawParameterValue ("switchSec")->load());
    const auto liveInMix = juce::jlimit (0.0f, 1.0f, parameters.getRawParameterValue ("liveInMix")->load());
    const bool useLiveOnly = liveInMix > 0.0001f;

    const auto outChannels = juce::jmax (1, buffer.getNumChannels());
    const auto samples = buffer.getNumSamples();
    juce::AudioBuffer<float> inputBuffer;
    inputBuffer.makeCopyOf (buffer, true);

    workingBuffer.setSize (2, samples, false, false, true);
    workingBuffer.clear();

    if (useLiveOnly)
    {
        if (! liveInputMode)
        {
            const juce::ScopedLock vlcGuard (vlcLock);
            if (vlcPlayer != nullptr)
                libvlc_media_player_stop (vlcPlayer);

            waitingForAudio = false;
            audioFifo.reset();
            {
                const juce::ScopedLock lock (statusLock);
                connectionStatus = "Live input mode (radio muted)";
            }
            liveInputMode = true;
        }
    }
    else
    {
        if (liveInputMode)
        {
            liveInputMode = false;
            queueStationLoad (chooseRandomStationExcluding (activeStationIndex));
        }

        const auto neededFloats = samples * 2;
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        audioFifo.prepareToRead (neededFloats, start1, size1, start2, size2);

        int copied = 0;
        auto copyInterleaved = [&copied, samples] (juce::AudioBuffer<float>& dst, int dstStart, const float* src, int floatCount)
        {
            const int frames = floatCount / 2;
            for (int i = 0; i < frames; ++i)
            {
                if (dstStart + i >= samples)
                    break;
                dst.setSample (0, dstStart + i, src[i * 2]);
                dst.setSample (1, dstStart + i, src[i * 2 + 1]);
            }
            copied += frames;
        };

        if (size1 > 0)
            copyInterleaved (workingBuffer, 0, audioRing.data() + static_cast<size_t> (start1), size1);
        if (size2 > 0)
            copyInterleaved (workingBuffer, copied, audioRing.data() + static_cast<size_t> (start2), size2);

        audioFifo.finishedRead (size1 + size2);
    }

    if (useLiveOnly)
    {
        const auto inChannels = inputBuffer.getNumChannels();
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto src = juce::jmin (ch, inChannels - 1);
            if (src >= 0)
                workingBuffer.copyFrom (ch, 0, inputBuffer, src, 0, samples);
        }
    }

    for (auto& m : stationMeterLevels)
        m.store (m.load (std::memory_order_relaxed) * 0.96f, std::memory_order_relaxed);

    if (activeStationIndex >= 0 && activeStationIndex < meterStationCount)
    {
        float mag = 0.0f;
        mag = juce::jmax (mag, workingBuffer.getMagnitude (0, 0, samples));
        mag = juce::jmax (mag, workingBuffer.getMagnitude (1, 0, samples));
        auto& meter = stationMeterLevels[static_cast<size_t> (activeStationIndex)];
        meter.store (juce::jmax (mag, meter.load (std::memory_order_relaxed)), std::memory_order_relaxed);
    }

    if (! useLiveOnly && waitingForAudio)
    {
        samplesSinceConnectRequest += samples;
        if (samplesSinceConnectRequest > static_cast<int64_t> (currentSampleRate * 6.0))
        {
            const juce::ScopedLock lock (statusLock);
            if (activeStationIndex >= 0 && activeStationIndex < meterStationCount)
                connectionStatus = "No audio from: " + juce::String (stations[static_cast<size_t> (activeStationIndex)].name);

            waitingForAudio = false;
            queueStationLoad (chooseRandomStationExcluding (activeStationIndex));
        }
    }

    samplesUntilSwitch -= samples;
    if (! useLiveOnly && samplesUntilSwitch <= 0 && activeStationIndex >= 0)
    {
        samplesUntilSwitch = switchIntervalSamples;
        {
            const juce::ScopedLock lock (statusLock);
            connectionStatus = "Auto-switching station...";
        }
        queueStationLoad (chooseRandomStationExcluding (activeStationIndex));
    }

    processEffects (workingBuffer);

    for (int c = 0; c < 2; ++c)
        workingBuffer.applyGain (c, 0, samples, 0.7f);

    buffer.clear();
    for (int ch = 0; ch < outChannels; ++ch)
    {
        const auto src = juce::jmin (ch, 1);
        buffer.copyFrom (ch, 0, workingBuffer, src, 0, samples);
    }

    {
        const juce::ScopedLock lock (captureLock);
        if (captureWriter != nullptr)
            captureWriter->write (buffer.getArrayOfReadPointers(), samples);
    }
}

bool RandomRadioFXAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* RandomRadioFXAudioProcessor::createEditor() { return new RandomRadioFXAudioProcessorEditor (*this); }

void RandomRadioFXAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (const auto state = parameters.copyState(); state.isValid())
    {
        std::unique_ptr<juce::XmlElement> xml (state.createXml());
        copyXmlToBinary (*xml, destData);
    }
}

void RandomRadioFXAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState != nullptr && xmlState->hasTagName (parameters.state.getType()))
        parameters.replaceState (juce::ValueTree::fromXml (*xmlState));
}

juce::AudioProcessorValueTreeState::ParameterLayout RandomRadioFXAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterFloat> ("switchSec", "Station Switch (s)", juce::NormalisableRange<float> (3.0f, 90.0f, 0.1f), 16.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("crossfadeMs", "Crossfade (ms)", juce::NormalisableRange<float> (30.0f, 4000.0f, 1.0f), 800.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("wet", "FX Wet", juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("liveInMix", "Live In", juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> ("stutterMix", "Stutter Mix", juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.35f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("stutterRate", "Stutter Rate", juce::NormalisableRange<float> (1.0f, 200000.0f, 0.1f), 22000.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> ("grainMix", "Granular Mix", juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.4f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("grainSizeMs", "Grain Size (ms)", juce::NormalisableRange<float> (1.0f, 180.0f, 0.1f), 8.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("grainDensity", "Grain Density", juce::NormalisableRange<float> (2.0f, 300000.0f, 0.1f), 50000.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> ("microMix", "Micro Loop Mix", juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("microMs", "Micro Loop Length (ms)", juce::NormalisableRange<float> (1.0f, 90.0f, 0.1f), 5.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat> ("softCrush", "Soft Crush", juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool> ("captureOnRecord", "Capture Rec", false));

    return { params.begin(), params.end() };
}

float RandomRadioFXAudioProcessor::getStationMeterLevel (int stationIndex) const
{
    if (! juce::isPositiveAndBelow (stationIndex, meterStationCount))
        return 0.0f;

    return stationMeterLevels[static_cast<size_t> (stationIndex)].load (std::memory_order_relaxed);
}

juce::String RandomRadioFXAudioProcessor::getStationMeterName (int stationIndex) const
{
    if (! juce::isPositiveAndBelow (stationIndex, meterStationCount))
        return {};

    return stations[static_cast<size_t> (stationIndex)].name;
}

juce::String RandomRadioFXAudioProcessor::getConnectionStatus() const
{
    const juce::ScopedLock lock (statusLock);
    return connectionStatus;
}

void RandomRadioFXAudioProcessor::nextStationNow()
{
    queueStationLoad (chooseRandomStationExcluding (activeStationIndex));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RandomRadioFXAudioProcessor();
}
