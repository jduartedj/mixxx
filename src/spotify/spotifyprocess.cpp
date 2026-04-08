#include "spotify/spotifyprocess.h"

#include <QDir>
#include <QStandardPaths>
#include <QtDebug>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {
#ifdef _WIN32
const QString kDeviceName = QStringLiteral("Mixxx DJ");
#else
const QString kFifoPrefix = QStringLiteral("/tmp/mixxx_spotify_");
const QString kDeviceName = QStringLiteral("Mixxx DJ");
#endif
const int kBitrate = 320;
} // anonymous namespace

namespace mixxx {

SpotifyProcess::SpotifyProcess(
        const QString& librespotPath,
        int deckId,
        QObject* parent)
        : QObject(parent),
          m_librespotPath(librespotPath),
          m_deckId(deckId),
#ifdef _WIN32
          m_pipePath(QString()),
#else
          m_pipePath(kFifoPrefix + QString::number(deckId)),
          m_fifoFd(-1),
#endif
          m_pProcess(nullptr),
          m_stopping(false) {
}

SpotifyProcess::~SpotifyProcess() {
    stop();
}

bool SpotifyProcess::createPipe() {
#ifdef _WIN32
    // On Windows, we use QProcess stdout — no pipe to create
    qDebug() << "SpotifyProcess: Using stdout pipe (Windows)";
    return true;
#else
    // Remove stale FIFO if it exists
    removePipe();

    if (mkfifo(m_pipePath.toLocal8Bit().constData(), 0600) != 0) {
        qWarning() << "SpotifyProcess: Failed to create FIFO at" << m_pipePath
                    << ":" << strerror(errno);
        return false;
    }
    qDebug() << "SpotifyProcess: Created FIFO at" << m_pipePath;
    return true;
#endif
}

void SpotifyProcess::removePipe() {
#ifdef _WIN32
    // Nothing to clean up on Windows — stdout pipe dies with the process
#else
    if (m_fifoFd >= 0) {
        ::close(m_fifoFd);
        m_fifoFd = -1;
    }
    QFile::remove(m_pipePath);
#endif
}

bool SpotifyProcess::openPipeForReading() {
#ifdef _WIN32
    // On Windows, we read from QProcess stdout — already open
    if (m_pProcess) {
        m_pProcess->setReadChannel(QProcess::StandardOutput);
        return true;
    }
    return false;
#else
    // Open FIFO in non-blocking mode initially so we don't block waiting
    // for the writer (librespot). We'll switch to blocking once connected.
    m_fifoFd = ::open(m_pipePath.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
    if (m_fifoFd < 0) {
        qWarning() << "SpotifyProcess: Failed to open FIFO for reading:"
                    << strerror(errno);
        return false;
    }
    return true;
#endif
}

bool SpotifyProcess::start() {
    if (m_pProcess && m_pProcess->state() != QProcess::NotRunning) {
        qWarning() << "SpotifyProcess: Already running";
        return true;
    }

    if (!createPipe()) {
        return false;
    }

    // Build cache directory
    QString cacheDir = QStandardPaths::writableLocation(
                                QStandardPaths::GenericCacheLocation) +
            QStringLiteral("/mixxx/spotify_cache");
    QDir().mkpath(cacheDir);

    m_pProcess = new QProcess(this);
    connect(m_pProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &SpotifyProcess::onProcessFinished);
    connect(m_pProcess,
            &QProcess::errorOccurred,
            this,
            &SpotifyProcess::onProcessError);

    QStringList args;
    args << QStringLiteral("--backend") << QStringLiteral("pipe");

#ifdef _WIN32
    // On Windows, omit --device to use stdout (default pipe behavior)
#else
    // On Linux, write to the FIFO file
    args << QStringLiteral("--device") << m_pipePath;
#endif

    args << QStringLiteral("--format") << QStringLiteral("F32")
         << QStringLiteral("--bitrate") << QString::number(kBitrate)
         << QStringLiteral("--name") << kDeviceName + QStringLiteral(" ") + QString::number(m_deckId)
         << QStringLiteral("--cache") << cacheDir
         << QStringLiteral("--disable-discovery")
         << QStringLiteral("--enable-oauth");

    qDebug() << "SpotifyProcess: Starting librespot:" << m_librespotPath << args;

    m_stopping = false;

#ifdef _WIN32
    // On Windows, redirect stdout to our process so we can read audio data
    m_pProcess->setProcessChannelMode(QProcess::SeparateChannels);
    m_pProcess->setReadChannel(QProcess::StandardOutput);
#endif

    m_pProcess->start(m_librespotPath, args);

    if (!m_pProcess->waitForStarted(5000)) {
        qWarning() << "SpotifyProcess: Failed to start librespot";
        delete m_pProcess;
        m_pProcess = nullptr;
        removePipe();
        return false;
    }

    // Open the read end of the pipe
    if (!openPipeForReading()) {
        stop();
        return false;
    }

    qDebug() << "SpotifyProcess: librespot started, PID:" << m_pProcess->processId();
    emit processStarted();
    return true;
}

void SpotifyProcess::stop() {
    m_stopping = true;

    if (m_pProcess) {
        if (m_pProcess->state() != QProcess::NotRunning) {
            m_pProcess->terminate();
            if (!m_pProcess->waitForFinished(3000)) {
                qWarning() << "SpotifyProcess: librespot did not terminate, killing";
                m_pProcess->kill();
                m_pProcess->waitForFinished(1000);
            }
        }
        delete m_pProcess;
        m_pProcess = nullptr;
    }

    removePipe();
    emit processStopped();
}

bool SpotifyProcess::isRunning() const {
    return m_pProcess && m_pProcess->state() == QProcess::Running;
}

QString SpotifyProcess::pipePath() const {
    return m_pipePath;
}

qint64 SpotifyProcess::readAudioData(char* data, qint64 maxSize) {
#ifdef _WIN32
    if (!m_pProcess || m_pProcess->state() != QProcess::Running) {
        return -1;
    }
    qint64 available = m_pProcess->bytesAvailable();
    if (available <= 0) {
        return 0;
    }
    return m_pProcess->read(data, maxSize);
#else
    if (m_fifoFd < 0) {
        return -1;
    }
    ssize_t bytesRead = ::read(m_fifoFd, data, static_cast<size_t>(maxSize));
    if (bytesRead < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
    return static_cast<qint64>(bytesRead);
#endif
}

bool SpotifyProcess::waitForAudioData(int msTimeout) {
#ifdef _WIN32
    if (!m_pProcess) {
        return false;
    }
    return m_pProcess->waitForReadyRead(msTimeout);
#else
    if (m_fifoFd < 0) {
        return false;
    }
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(m_fifoFd, &readfds);
    struct timeval tv;
    tv.tv_sec = msTimeout / 1000;
    tv.tv_usec = (msTimeout % 1000) * 1000;
    int ret = select(m_fifoFd + 1, &readfds, nullptr, nullptr, &tv);
    return ret > 0;
#endif
}

void SpotifyProcess::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    qDebug() << "SpotifyProcess: librespot finished, exit code:" << exitCode
             << "status:" << exitStatus;
    if (!m_stopping) {
        // Unexpected exit — emit error for potential restart
        emit processError(QStringLiteral("librespot exited unexpectedly with code %1")
                                  .arg(exitCode));
    }
}

void SpotifyProcess::onProcessError(QProcess::ProcessError error) {
    qWarning() << "SpotifyProcess: Process error:" << error;
    if (!m_stopping) {
        emit processError(QStringLiteral("librespot process error: %1")
                                  .arg(static_cast<int>(error)));
    }
}

} // namespace mixxx

#include "moc_spotifyprocess.cpp"
