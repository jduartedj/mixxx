#include "spotify/spotifyprocess.h"

#include <QDir>
#include <QStandardPaths>
#include <QtDebug>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
const QString kFifoPrefix = QStringLiteral("/tmp/mixxx_spotify_");
const QString kDeviceName = QStringLiteral("Mixxx DJ");
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
          m_fifoPath(kFifoPrefix + QString::number(deckId)),
          m_pProcess(nullptr),
          m_fifoFd(-1),
          m_stopping(false) {
}

SpotifyProcess::~SpotifyProcess() {
    stop();
}

bool SpotifyProcess::createFifo() {
    // Remove stale FIFO if it exists
    removeFifo();

    if (mkfifo(m_fifoPath.toLocal8Bit().constData(), 0600) != 0) {
        qWarning() << "SpotifyProcess: Failed to create FIFO at" << m_fifoPath
                    << ":" << strerror(errno);
        return false;
    }
    qDebug() << "SpotifyProcess: Created FIFO at" << m_fifoPath;
    return true;
}

void SpotifyProcess::removeFifo() {
    if (m_fifoFd >= 0) {
        ::close(m_fifoFd);
        m_fifoFd = -1;
    }
    QFile::remove(m_fifoPath);
}

bool SpotifyProcess::openFifoForReading() {
    // Open FIFO in non-blocking mode initially so we don't block waiting
    // for the writer (librespot). We'll switch to blocking once connected.
    m_fifoFd = ::open(m_fifoPath.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
    if (m_fifoFd < 0) {
        qWarning() << "SpotifyProcess: Failed to open FIFO for reading:"
                    << strerror(errno);
        return false;
    }
    return true;
}

bool SpotifyProcess::start() {
    if (m_pProcess && m_pProcess->state() != QProcess::NotRunning) {
        qWarning() << "SpotifyProcess: Already running";
        return true;
    }

    if (!createFifo()) {
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
    args << QStringLiteral("--backend") << QStringLiteral("pipe")
         << QStringLiteral("--device") << m_fifoPath
         << QStringLiteral("--format") << QStringLiteral("F32")
         << QStringLiteral("--bitrate") << QString::number(kBitrate)
         << QStringLiteral("--name") << kDeviceName + QStringLiteral(" ") + QString::number(m_deckId)
         << QStringLiteral("--cache") << cacheDir
         << QStringLiteral("--disable-discovery")
         << QStringLiteral("--enable-oauth");

    qDebug() << "SpotifyProcess: Starting librespot:" << m_librespotPath << args;

    m_stopping = false;
    m_pProcess->start(m_librespotPath, args);

    if (!m_pProcess->waitForStarted(5000)) {
        qWarning() << "SpotifyProcess: Failed to start librespot";
        delete m_pProcess;
        m_pProcess = nullptr;
        removeFifo();
        return false;
    }

    // Open the read end of the FIFO
    if (!openFifoForReading()) {
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

    removeFifo();
    emit processStopped();
}

bool SpotifyProcess::isRunning() const {
    return m_pProcess && m_pProcess->state() == QProcess::Running;
}

QString SpotifyProcess::fifoPath() const {
    return m_fifoPath;
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
