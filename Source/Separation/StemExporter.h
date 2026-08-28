/*
  ==============================================================================
    StemExporter.h

    Writes a separated stem (vocals.wav / instrumental.wav, as produced by
    SeparationEngine) out to a user-chosen format/sample-rate/bit-depth. Used
    by both the per-card "Export..." buttons and "EXPORT ALL", and by the
    drag-out-to-DAW path (which exports to a temp file first, then hands that
    file to the OS drag operation).

    MP3: JUCE's bundled MP3AudioFormat can only decode, not encode
    (MP3AudioFormat::createWriterFor() is unimplemented upstream - writing
    real MP3 would need a separately-licensed/bundled encoder such as LAME,
    which this project doesn't ship). Requesting MP3 here transparently
    exports WAV instead and returns a "Warning: ..." string explaining that,
    rather than silently producing a file with the wrong extension/content.
  ==============================================================================
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <memory>

class StemExporter
{
public:
    enum class Format { wav, flac, mp3 };
    enum class SampleRateChoice { sameAsInput, sr48k, sr96k };
    enum class BitDepth { bit24, bit32Float };

    struct OutputSettings
    {
        Format format = Format::wav;
        SampleRateChoice sampleRate = SampleRateChoice::sameAsInput;
        BitDepth bitDepth = BitDepth::bit24;
    };

    static juce::String getFileExtension (Format format)
    {
        switch (format)
        {
            case Format::flac: return ".flac";
            case Format::mp3:  return ".wav"; // see class comment - MP3 write isn't real, falls back to WAV
            case Format::wav:
            default:           return ".wav";
        }
    }

    /** Converts sourceFile (an existing mono/stereo stem produced by SeparationEngine)
        into targetFile per settings - real format conversion and, when the requested
        sample rate differs from the source, real resampling (juce::ResamplingAudioSource),
        not just a file copy.

        Returns an empty string on full success, a "Warning: ..." string if it succeeded
        but had to adjust something (MP3 request, unsupported bit depth for the chosen
        format), or any other non-empty string on hard failure. */
    static juce::String exportStem (const juce::File& sourceFile, const juce::File& targetFile,
                                     const OutputSettings& settingsIn)
    {
        auto settings = settingsIn;
        juce::String warning;

        if (settings.format == Format::mp3)
        {
            warning = "Warning: MP3 encoding isn't available in this build (JUCE ships MP3 decoding only, "
                       "with no bundled encoder) - exported as WAV instead.";
            settings.format = Format::wav;
        }

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (sourceFile));
        if (reader == nullptr)
            return "Couldn't read " + sourceFile.getFullPathName();

        const int numChannels = (int) reader->numChannels;
        const double sourceRate = reader->sampleRate;
        double targetRate = sourceRate;
        if (settings.sampleRate == SampleRateChoice::sr48k) targetRate = 48000.0;
        else if (settings.sampleRate == SampleRateChoice::sr96k) targetRate = 96000.0;

        std::unique_ptr<juce::AudioFormat> audioFormat;
        if (settings.format == Format::flac)
            audioFormat = std::make_unique<juce::FlacAudioFormat>();
        else
            audioFormat = std::make_unique<juce::WavAudioFormat>();

        int bitsPerSample = settings.bitDepth == BitDepth::bit32Float ? 32 : 24;
        auto possibleDepths = audioFormat->getPossibleBitDepths();
        if (! possibleDepths.contains (bitsPerSample))
        {
            bitsPerSample = possibleDepths.contains (24) ? 24 : possibleDepths.getLast();
            if (warning.isEmpty())
                warning = "Warning: " + audioFormat->getFormatName() + " doesn't support the requested bit depth - used "
                           + juce::String (bitsPerSample) + "-bit instead.";
        }

        targetFile.getParentDirectory().createDirectory();
        targetFile.deleteFile();

        std::unique_ptr<juce::OutputStream> outStream (targetFile.createOutputStream());
        if (outStream == nullptr)
            return "Couldn't create output file " + targetFile.getFullPathName();

        auto options = juce::AudioFormatWriterOptions()
                          .withSampleRate (targetRate)
                          .withNumChannels (numChannels)
                          .withBitsPerSample (bitsPerSample);

        std::unique_ptr<juce::AudioFormatWriter> writer (audioFormat->createWriterFor (outStream, options));
        if (writer == nullptr)
            return "Couldn't create a " + audioFormat->getFormatName() + " writer for " + targetFile.getFullPathName();

        if (juce::approximatelyEqual (targetRate, sourceRate))
        {
            if (! writer->writeFromAudioReader (*reader, 0, reader->lengthInSamples))
                return "Failed writing " + targetFile.getFullPathName();
        }
        else
        {
            juce::AudioFormatReaderSource readerSource (reader.get(), false);
            juce::ResamplingAudioSource resampler (&readerSource, false, numChannels);
            resampler.setResamplingRatio (sourceRate / targetRate);
            resampler.prepareToPlay (8192, sourceRate);

            juce::AudioBuffer<float> block (numChannels, 8192);
            auto remainingOutputSamples = (juce::int64) std::ceil ((double) reader->lengthInSamples * targetRate / sourceRate);

            while (remainingOutputSamples > 0)
            {
                int thisBlock = (int) juce::jmin<juce::int64> (8192, remainingOutputSamples);
                juce::AudioSourceChannelInfo info (&block, 0, thisBlock);
                resampler.getNextAudioBlock (info);
                writer->writeFromAudioSampleBuffer (block, 0, thisBlock);
                remainingOutputSamples -= thisBlock;
            }

            resampler.releaseResources();
        }

        return warning;
    }
};
