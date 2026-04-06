#pragma once

#include "sources/soundsourceprovider.h"
#include "spotify/spotifyapiclient.h"
#include "spotify/spotifyprocess.h"
#include "util/samplebuffer.h"

#include <QMutex>
#include <QThread>
#include <atomic>
#include <memory>

namespace mixxx {

/// SoundSource that reads raw PCM audio from a librespot FIFO pipe.
/// This bridges Spotify streaming into Mixxx's audio engine.
class SoundSourceSpotify final : public SoundSource {
  public:
    explicit SoundSourceSpotify(const QUrl& url);
    ~SoundSourceSpotify() override;

    void close() override;

  protected:
    ReadableSampleFrames readSampleFramesClamped(
            const WritableSampleFrames& sampleFrames) override;

  private:
    OpenResult tryOpen(
            OpenMode mode,
            const OpenParams& params) override;

    /// Read PCM data from the FIFO into the ring buffer.
    /// Called from the reader thread.
    void readerThreadFunc();

    /// Extract the Spotify track URI from the QUrl.
    QString extractTrackUri() const;

    // librespot subprocess
    std::unique_ptr<SpotifyProcess> m_pProcess;

    // Spotify API client for playback control
    std::unique_ptr<SpotifyApiClient> m_pApiClient;

    // Ring buffer for PCM data from FIFO
    static constexpr int kRingBufferFrames = 44100 * 30; // 30 seconds buffer
    static constexpr int kChannelCount = 2;
    SampleBuffer m_ringBuffer;
    std::atomic<SINT> m_ringWritePos;
    std::atomic<SINT> m_ringReadPos;
    QMutex m_ringMutex;

    // Reader thread
    std::unique_ptr<QThread> m_pReaderThread;
    std::atomic<bool> m_readerRunning;

    // Playback state
    SINT m_curFrameIndex;
    QString m_trackUri;
    int m_trackDurationMs;

    // Audio format constants
    static constexpr audio::SampleRate kSampleRate = audio::SampleRate(44100);
};

/// Provider factory that registers the spotify:// URL scheme.
class SoundSourceProviderSpotify : public SoundSourceProvider {
  public:
    static const QString kDisplayName;
    static const QStringList kSupportedFileTypes;

    QString getDisplayName() const override {
        return kDisplayName;
    }

    QStringList getSupportedFileTypes() const override {
        return kSupportedFileTypes;
    }

    SoundSourceProviderPriority getPriorityHint(
            const QString& supportedFileType) const override {
        Q_UNUSED(supportedFileType)
        return SoundSourceProviderPriority::Default;
    }

    SoundSourcePointer newSoundSource(const QUrl& url) override {
        return newSoundSourceFromUrl<SoundSourceSpotify>(url);
    }
};

} // namespace mixxx
