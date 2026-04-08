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
/// Each instance represents one librespot process bound to a specific deck.
class SpotifyProcess : public QObject {
    Q_OBJECT

  public:
    /// @param librespotPath Path to the librespot binary
    /// @param deckId Unique identifier for this deck (used in pipe name)
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

    /// Read audio data from the pipe.
    /// @return number of bytes read, 0 if no data available, -1 on error
    qint64 readAudioData(char* data, qint64 maxSize);

    /// Wait for audio data to become available.
    /// @return true if data is available, false on timeout
    bool waitForAudioData(int msTimeout);

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

  private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);

  private:
    bool createPipe();
    void removePipe();
    bool openPipeForReading();

    QString m_librespotPath;
    int m_deckId;
    QString m_pipePath;
    QProcess* m_pProcess;
    bool m_stopping;

#ifdef _WIN32
    // Windows: read from QProcess stdout, no separate pipe handle needed
#else
    // Linux: FIFO file descriptor
    int m_fifoFd;
#endif
};

} // namespace mixxx
