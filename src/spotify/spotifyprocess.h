#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

namespace mixxx {

/// Manages a librespot subprocess that outputs raw PCM audio to a FIFO pipe.
/// Each instance represents one librespot process bound to a specific deck.
class SpotifyProcess : public QObject {
    Q_OBJECT

  public:
    /// @param librespotPath Path to the librespot binary
    /// @param deckId Unique identifier for this deck (used in FIFO name)
    /// @param parent QObject parent
    explicit SpotifyProcess(
            const QString& librespotPath,
            int deckId,
            QObject* parent = nullptr);
    ~SpotifyProcess() override;

    /// Start the librespot subprocess. Creates the FIFO and launches the process.
    /// @return true if started successfully
    bool start();

    /// Stop the librespot subprocess and clean up the FIFO.
    void stop();

    /// @return true if the subprocess is running
    bool isRunning() const;

    /// @return Path to the FIFO pipe for reading PCM audio
    QString fifoPath() const;

    /// @return File descriptor for the open FIFO (read end), or -1 if not open
    int fifoFd() const {
        return m_fifoFd;
    }

  signals:
    void processStarted();
    void processStopped();
    void processError(const QString& error);

  private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);

  private:
    bool createFifo();
    void removeFifo();
    bool openFifoForReading();

    QString m_librespotPath;
    int m_deckId;
    QString m_fifoPath;
    QProcess* m_pProcess;
    int m_fifoFd;
    bool m_stopping;
};

} // namespace mixxx
