#include "sources/soundsourcespotify.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QStandardPaths>
#include <QDir>
#include <QThread>
#include <QtDebug>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace mixxx {

const QString SoundSourceProviderSpotify::kDisplayName =
        QStringLiteral("Spotify (librespot)");
const QStringList SoundSourceProviderSpotify::kSupportedFileTypes = {
        QStringLiteral("spotify"),
};

SoundSourceSpotify::SoundSourceSpotify(const QUrl& url)
        : SoundSource(url, QStringLiteral("spotify")),
          m_ringBuffer(kRingBufferFrames * kChannelCount),
          m_ringWritePos(0),
          m_ringReadPos(0),
          m_readerRunning(false),
          m_curFrameIndex(0),
          m_trackDurationMs(0) {
}

SoundSourceSpotify::~SoundSourceSpotify() {
    close();
}

QString SoundSourceSpotify::extractTrackUri() const {
    // URL format: spotify://track/TRACK_ID or spotify:track:TRACK_ID
    QUrl url = getUrl();
    QString path = url.path();
    if (path.startsWith('/')) {
        path = path.mid(1);
    }

    // Handle spotify://track/ID format
    if (url.scheme() == QStringLiteral("spotify")) {
        return QStringLiteral("spotify:track:") + path.section('/', -1);
    }

    return url.toString();
}

AudioSource::OpenResult SoundSourceSpotify::tryOpen(
        OpenMode mode,
        const OpenParams& params) {
    Q_UNUSED(mode)
    Q_UNUSED(params)

    m_trackUri = extractTrackUri();
    if (m_trackUri.isEmpty()) {
        qWarning() << "SoundSourceSpotify: Invalid track URI from URL:" << getUrl();
        return OpenResult::Failed;
    }

    qDebug() << "SoundSourceSpotify: Opening track:" << m_trackUri;

    // Find the librespot binary
    // Look in common locations
    QString librespotPath;
    QStringList searchPaths = {
#ifdef _WIN32
            QDir::homePath() + QStringLiteral("/librespot/target/release/librespot.exe"),
            QStringLiteral("C:/Users/jduar/librespot/target/release/librespot.exe"),
            QCoreApplication::applicationDirPath() + QStringLiteral("/librespot.exe"),
#else
            QDir::homePath() + QStringLiteral("/clawd/mixxx-spotify/librespot/target/release/librespot"),
            QStringLiteral("/usr/local/bin/librespot"),
            QStringLiteral("/usr/bin/librespot"),
#endif
    };
    for (const auto& path : searchPaths) {
        if (QFile::exists(path)) {
            librespotPath = path;
            break;
        }
    }

    if (librespotPath.isEmpty()) {
        qWarning() << "SoundSourceSpotify: librespot binary not found";
        return OpenResult::Failed;
    }

    // Start librespot subprocess
    static int sDeckCounter = 0;
    m_pProcess = std::make_unique<SpotifyProcess>(librespotPath, sDeckCounter++);
    if (!m_pProcess->start()) {
        qWarning() << "SoundSourceSpotify: Failed to start librespot";
        m_pProcess.reset();
        return OpenResult::Failed;
    }

    // Initialize API client
    m_pApiClient = std::make_unique<SpotifyApiClient>();

    // Set audio parameters
    // librespot outputs 44100 Hz, stereo, F32
    initChannelCountOnce(kChannelCount);
    initSampleRateOnce(kSampleRate);

    // For now, estimate duration. We'll get actual duration from Spotify API.
    // Default to 5 minutes if unknown. The API client will update this async.
    m_trackDurationMs = 300000; // 5 minutes default
    SINT totalFrames = static_cast<SINT>(
            (static_cast<double>(m_trackDurationMs) / 1000.0) * kSampleRate.value());
    initFrameIndexRangeOnce(IndexRange::forward(0, totalFrames));

    // Start the reader thread
    m_readerRunning = true;
    m_pReaderThread = std::make_unique<QThread>();
    QObject::connect(m_pReaderThread.get(), &QThread::started, [this]() {
        readerThreadFunc();
    });
    m_pReaderThread->start();

    // Tell Spotify to play this track on our librespot device
    // We need to wait a moment for librespot to register as a device
    QTimer::singleShot(2000, m_pApiClient.get(), [this]() {
        m_pApiClient->play(m_trackUri);
    });

    qDebug() << "SoundSourceSpotify: Opened successfully, track:" << m_trackUri;
    return OpenResult::Succeeded;
}

void SoundSourceSpotify::close() {
    // Stop reader thread
    m_readerRunning = false;
    if (m_pReaderThread && m_pReaderThread->isRunning()) {
        m_pReaderThread->quit();
        m_pReaderThread->wait(3000);
    }
    m_pReaderThread.reset();

    // Stop librespot
    if (m_pProcess) {
        m_pProcess->stop();
        m_pProcess.reset();
    }

    // Cleanup API client
    m_pApiClient.reset();

    // Reset ring buffer
    m_ringWritePos = 0;
    m_ringReadPos = 0;
    m_curFrameIndex = 0;
}

void SoundSourceSpotify::readerThreadFunc() {
    // Buffer for reading from pipe
    // Read in chunks of 4096 frames (4096 * 2 channels * 4 bytes = 32KB)
    constexpr int kReadFrames = 4096;
    constexpr int kReadSamples = kReadFrames * kChannelCount;
    constexpr int kReadBytes = kReadSamples * sizeof(float);
    std::vector<char> readBuf(kReadBytes);

    while (m_readerRunning) {
        // Calculate available space in ring buffer
        SINT writePos = m_ringWritePos.load(std::memory_order_relaxed);
        SINT readPos = m_ringReadPos.load(std::memory_order_acquire);
        SINT totalSamples = kRingBufferFrames * kChannelCount;
        SINT used = (writePos - readPos + totalSamples) % totalSamples;
        SINT available = totalSamples - used - kChannelCount; // Leave gap to distinguish full/empty

        if (available < kReadSamples) {
            // Buffer full, wait a bit
            QThread::msleep(10);
            continue;
        }

        // Wait for data to be available
        if (!m_pProcess->waitForAudioData(50)) {
            continue;
        }

        qint64 bytesRead = m_pProcess->readAudioData(readBuf.data(), kReadBytes);
        if (bytesRead < 0) {
            qWarning() << "SoundSourceSpotify: Pipe read error";
            break;
        }
        if (bytesRead == 0) {
            // No data available yet
            QThread::msleep(5);
            continue;
        }

        // Write the read samples into the ring buffer
        int samplesRead = static_cast<int>(bytesRead / sizeof(float));
        const float* floatBuf = reinterpret_cast<const float*>(readBuf.data());
        SINT wp = writePos % totalSamples;

        for (int i = 0; i < samplesRead; i++) {
            m_ringBuffer[wp] = floatBuf[i];
            wp = (wp + 1) % totalSamples;
        }

        m_ringWritePos.store(
                (writePos + samplesRead) % totalSamples,
                std::memory_order_release);
    }
}

ReadableSampleFrames SoundSourceSpotify::readSampleFramesClamped(
        const WritableSampleFrames& sampleFrames) {
    const SINT firstFrameIndex = sampleFrames.frameIndexRange().start();
    const SINT numberOfFrames = sampleFrames.frameLength();

    // Handle seeking
    if (firstFrameIndex != m_curFrameIndex) {
        // Seek requested
        int seekPositionMs = static_cast<int>(
                (static_cast<double>(firstFrameIndex) / kSampleRate.value()) * 1000.0);
        qDebug() << "SoundSourceSpotify: Seeking to frame" << firstFrameIndex
                  << "(" << seekPositionMs << "ms)";

        // Flush ring buffer
        m_ringReadPos.store(m_ringWritePos.load(std::memory_order_relaxed),
                std::memory_order_release);

        // Send seek command via API
        if (m_pApiClient) {
            m_pApiClient->seek(seekPositionMs);
        }

        m_curFrameIndex = firstFrameIndex;

        // Wait briefly for new data after seek
        QThread::msleep(200);
    }

    // Read from ring buffer
    CSAMPLE* pOutput = sampleFrames.writableData();
    SINT framesRead = 0;
    SINT totalSamples = kRingBufferFrames * kChannelCount;

    for (SINT i = 0; i < numberOfFrames; i++) {
        SINT rp = m_ringReadPos.load(std::memory_order_acquire);
        SINT wp = m_ringWritePos.load(std::memory_order_relaxed);

        if (rp == wp) {
            // Buffer underrun — fill with silence
            break;
        }

        // Read one frame (2 samples for stereo)
        for (int ch = 0; ch < kChannelCount; ch++) {
            SINT readIdx = (rp + ch) % totalSamples;
            pOutput[(i * kChannelCount) + ch] = m_ringBuffer[readIdx];
        }

        m_ringReadPos.store(
                (rp + kChannelCount) % totalSamples,
                std::memory_order_release);
        framesRead++;
    }

    m_curFrameIndex += framesRead;

    // Fill remaining with silence if buffer underrun
    if (framesRead < numberOfFrames) {
        SINT silenceStart = framesRead * kChannelCount;
        SINT silenceEnd = numberOfFrames * kChannelCount;
        for (SINT i = silenceStart; i < silenceEnd; i++) {
            pOutput[i] = 0.0f;
        }
    }

    return ReadableSampleFrames(
            IndexRange::forward(firstFrameIndex, framesRead),
            SampleBuffer::ReadableSlice(pOutput, framesRead * kChannelCount));
}

} // namespace mixxx
