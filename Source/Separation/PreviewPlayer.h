/*
  ==============================================================================
    PreviewPlayer.h

    Shared audio-device owner used by the editor's two StemPreviewPanels
    (Vocals / Instrumental) to preview separated stems. Deliberately
    independent of the plugin's own audio I/O (PluginProcessor::processBlock
    is a passthrough and never touches these buffers) - this opens its own
    output device the same way a sample-browser plugin's preview player does.

    Both stems can genuinely play at once (needed for solo/mute to mean
    anything): each channel is its own AudioTransportSource -> a small
    metering tap -> a shared MixerAudioSource -> one AudioSourcePlayer/device.
    Per-channel gain (the LEVEL knob) and mute both collapse to a single
    AudioTransportSource::setGain() call per channel - mute is just gain 0,
    which also makes the meter naturally read 0 when muted with no special
    case needed in the metering tap itself.
  ==============================================================================
*/

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <memory>

class PreviewPlayer
{
public:
    enum class Channel { vocals, instrumental };

    explicit PreviewPlayer (juce::AudioFormatManager& formatManagerToUse)
        : formatManager (formatManagerToUse)
    {
        deviceManager.initialiseWithDefaultDevices (0, 2);

        vocalsState.meteringTap = std::make_unique<MeteringTap> (vocalsState.transportSource);
        instrumentalState.meteringTap = std::make_unique<MeteringTap> (instrumentalState.transportSource);

        mixer.addInputSource (vocalsState.meteringTap.get(), false);
        mixer.addInputSource (instrumentalState.meteringTap.get(), false);

        sourcePlayer.setSource (&mixer);
        deviceManager.addAudioCallback (&sourcePlayer);
    }

    ~PreviewPlayer()
    {
        vocalsState.transportSource.stop();
        instrumentalState.transportSource.stop();

        deviceManager.removeAudioCallback (&sourcePlayer);
        sourcePlayer.setSource (nullptr);
        mixer.removeAllInputs();

        vocalsState.transportSource.setSource (nullptr);
        instrumentalState.transportSource.setSource (nullptr);
    }

    /** Loads a new file for this channel (stopped, position 0); does not start playback. */
    void setFile (Channel ch, const juce::File& file)
    {
        auto& state = stateFor (ch);

        state.transportSource.stop();
        state.transportSource.setSource (nullptr);
        state.readerSource.reset();

        auto* reader = formatManager.createReaderFor (file);
        if (reader == nullptr)
        {
            state.file = juce::File();
            return;
        }

        state.readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader, true);
        state.transportSource.setSource (state.readerSource.get(), 0, nullptr, reader->sampleRate);
        state.file = file;
        state.updateGain();
    }

    void clear (Channel ch)
    {
        auto& state = stateFor (ch);
        state.transportSource.stop();
        state.transportSource.setSource (nullptr);
        state.readerSource.reset();
        state.file = juce::File();
    }

    bool hasFile (Channel ch) const noexcept { return stateFor (ch).file != juce::File(); }
    juce::File getFile (Channel ch) const { return stateFor (ch).file; }

    /** Resumes from the current position, or restarts from 0 if playback had
        reached the end - real pause/resume, not "always restart from 0". */
    void play (Channel ch)
    {
        auto& state = stateFor (ch);
        if (state.readerSource == nullptr)
            return;

        if (state.transportSource.hasStreamFinished()
            || state.transportSource.getCurrentPosition() >= state.transportSource.getLengthInSeconds() - 0.02)
            state.transportSource.setPosition (0.0);

        state.transportSource.start();
    }

    /** Stops without resetting position (a true pause). */
    void pause (Channel ch) { stateFor (ch).transportSource.stop(); }

    void togglePlayPause (Channel ch)
    {
        if (isPlaying (ch))
            pause (ch);
        else
            play (ch);
    }

    bool isPlaying (Channel ch) const noexcept { return stateFor (ch).transportSource.isPlaying(); }

    double getPositionSeconds (Channel ch) const { return stateFor (ch).transportSource.getCurrentPosition(); }
    double getLengthSeconds (Channel ch) const { return stateFor (ch).transportSource.getLengthInSeconds(); }

    void seekToSeconds (Channel ch, double seconds)
    {
        auto& state = stateFor (ch);
        state.transportSource.setPosition (juce::jlimit (0.0, state.transportSource.getLengthInSeconds(), seconds));
    }

    /** Preview-only gain (auditioning volume) - never affects the exported file. */
    void setGainDb (Channel ch, float db)
    {
        auto& state = stateFor (ch);
        state.gainDb = db;
        state.updateGain();
    }

    float getGainDb (Channel ch) const noexcept { return stateFor (ch).gainDb; }

    void setMuted (Channel ch, bool shouldBeMuted)
    {
        auto& state = stateFor (ch);
        state.muted = shouldBeMuted;
        state.updateGain();
    }

    bool isMuted (Channel ch) const noexcept { return stateFor (ch).muted; }

    /** 0..~1+ instantaneous peak magnitude of what's actually being played
        (post gain/mute) - drives the live level meter. audioChannel is 0 (left/
        mono) or 1 (right); out-of-range or a mono source's absent channel 1
        both just return 0. */
    float getLevel (Channel ch, int audioChannel) const noexcept
    {
        auto* tap = stateFor (ch).meteringTap.get();
        return tap != nullptr ? tap->getLevel (audioChannel) : 0.0f;
    }

private:
    //==============================================================================
    /** Wraps a channel's AudioTransportSource so the mixer can pull from it while
        this tap captures the level of what just got played (already gain/mute
        adjusted, since AudioTransportSource applies its own gain before this
        wrapper sees the block). */
    class MeteringTap : public juce::AudioSource
    {
    public:
        explicit MeteringTap (juce::AudioSource& sourceToWrap) : source (sourceToWrap) {}

        void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override
        {
            source.prepareToPlay (samplesPerBlockExpected, sampleRate);
        }

        void releaseResources() override { source.releaseResources(); }

        void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
        {
            source.getNextAudioBlock (info);

            if (info.buffer != nullptr && info.numSamples > 0)
            {
                auto numCh = info.buffer->getNumChannels();
                levels[0].store (numCh > 0 ? info.buffer->getMagnitude (0, info.startSample, info.numSamples) : 0.0f);
                levels[1].store (numCh > 1 ? info.buffer->getMagnitude (1, info.startSample, info.numSamples)
                                            : levels[0].load());
            }
        }

        float getLevel (int audioChannel) const noexcept
        {
            return juce::isPositiveAndBelow (audioChannel, 2) ? levels[(size_t) audioChannel].load() : 0.0f;
        }

    private:
        juce::AudioSource& source;
        std::atomic<float> levels[2] { { 0.0f }, { 0.0f } };
    };

    struct ChannelState
    {
        juce::File file;
        std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
        juce::AudioTransportSource transportSource;
        std::unique_ptr<MeteringTap> meteringTap;
        float gainDb = 0.0f;
        bool muted = false;

        void updateGain()
        {
            transportSource.setGain (muted ? 0.0f : juce::Decibels::decibelsToGain (gainDb));
        }
    };

    ChannelState& stateFor (Channel ch) noexcept { return ch == Channel::vocals ? vocalsState : instrumentalState; }
    const ChannelState& stateFor (Channel ch) const noexcept { return ch == Channel::vocals ? vocalsState : instrumentalState; }

    juce::AudioFormatManager& formatManager;
    juce::AudioDeviceManager deviceManager;
    juce::MixerAudioSource mixer;
    juce::AudioSourcePlayer sourcePlayer;

    ChannelState vocalsState, instrumentalState;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreviewPlayer)
};
