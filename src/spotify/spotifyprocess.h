#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mixxx {

/// Manages a librespot subprocess that outputs raw PCM audio via a pipe.
/// On Linux, uses a FIFO (named pipe). On Windows, reads from process stdout.
/// Supports both real-time streaming and full-track download to WAV.
class SpotifyProcess : public QObject {
    Q_OBJECT

  public:
    /// @param librespotPath Path to the librespot binary
    /// @param deckId Unique identifier for this instance (used in pipe/device name)
    /// @param parent QObject parent
    explicit SpotifyProcess(
            const QString& librespotPath,
            int deckId,
            QObject* parent = nullptr);
    ~SpotifyProcess() override;

    /// Start the librespot subprocess. Creates the pipe and launches the process.
    /// @return true if started successfully
    bool start();

    /// Stop the librespot subprocess and clean up the pipe.
    void stop();

    /// @return true if the subprocess is running
    bool isRunning() const;

    /// @return Path to the pipe for reading PCM audio (Linux: FIFO path, Windows: N/A)
    QString pipePath() const;

    /// @return The Spotify Connect device name this instance uses
    QString deviceName() const;

    /// Read audio data from the pipe.
    /// @return number of bytes read, 0 if no data available, -1 on error
    qint64 readAudioData(char* data, qint64 maxSize);

    /// Wait for audio data to become available.
    /// @return true if data is available, false on timeout
    bool waitForAudioData(int msTimeout);

    /// Download a Spotify track as a WAV file.
    /// Starts librespot (if not running), waits for device registration,
    /// starts playback via Spotify Web API, captures raw F32LE audio,
    /// then converts to WAV via ffmpeg.
    ///
    /// This is a BLOCKING call — it runs for roughly durationMs + overhead.
    ///
    /// @param trackUri Spotify URI (e.g. "spotify:track:4cOdK2wGLETKBW3PvgPWqT")
    /// @param outputPath Where to save the WAV file (e.g. "~/.cache/mixxx/spotify_tracks/ID.wav")
    /// @param durationMs Track duration in milliseconds (from Spotify API)
    /// @param accessToken Spotify Web API access token for playback control
    /// @return true if download succeeded and WAV is valid
    bool downloadTrack(const QString& trackUri,
                       const QString& outputPath,
                       int durationMs,
                       const QString& accessToken);

#ifndef _WIN32
    /// @return File descriptor for the open FIFO (read end), or -1 if not open (Linux only)
    int fifoFd() const {
        return m_fifoFd;
    }
#endif

  signals:
    void processStarted();
    void processStopped();
    void processError(const QString& error);
    void downloadProgress(int percentComplete);

  private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);

  private:
    bool createPipe();
    void removePipe();
    bool openPipeForReading();

    /// Find the Spotify Connect device ID for this librespot instance.
    /// Queries /me/player/devices and looks for our device name.
    /// @return device ID or empty string if not found
    QString findDeviceId(const QString& accessToken);

    /// Start playback of a track on a specific device via Spotify Web API.
    /// @return true if playback started (HTTP 204)
    bool startPlayback(const QString& trackUri,
                       const QString& deviceId,
                       const QString& accessToken);

    /// Pause playback via Spotify Web API.
    void pausePlayback(const QString& accessToken);

    /// Convert a raw F32LE file to WAV using ffmpeg.
    /// @return true if conversion succeeded
    bool convertToWav(const QString& rawPath, const QString& wavPath);

    /// Ensure librespot credentials exist in the cache directory.
    /// Copies from spotify-player cache if available.
    void ensureCredentials();

    QString m_librespotPath;
    int m_deckId;
    QString m_pipePath;
    QProcess* m_pProcess;
    bool m_stopping;

    /// Cache directory for librespot data (credentials, audio cache)
    QString m_cacheDir;

#ifdef _WIN32
    // Windows: read from QProcess stdout, no separate pipe handle needed
#else
    // Linux: FIFO file descriptor
    int m_fifoFd;
#endif
};

} // namespace mixxx
