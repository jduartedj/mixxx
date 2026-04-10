#include "spotify/spotifyprocess.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
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
const int kDeviceRegistrationWaitMs = 5000;
const int kPlaybackStartWaitMs = 1000;
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
    // Initialize cache dir
    m_cacheDir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
                 QStringLiteral("/mixxx/spotify_dl");
    QDir().mkpath(m_cacheDir);
    ensureCredentials();
}

SpotifyProcess::~SpotifyProcess() {
    stop();
}

QString SpotifyProcess::deviceName() const {
    return kDeviceName + QStringLiteral(" ") + QString::number(m_deckId);
}

bool SpotifyProcess::createPipe() {
#ifdef _WIN32
    qDebug() << "SpotifyProcess: Using stdout pipe (Windows)";
    return true;
#else
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
    if (m_pProcess) {
        m_pProcess->setReadChannel(QProcess::StandardOutput);
        return true;
    }
    return false;
#else
    m_fifoFd = ::open(m_pipePath.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
    if (m_fifoFd < 0) {
        qWarning() << "SpotifyProcess: Failed to open FIFO for reading:"
                    << strerror(errno);
        return false;
    }
    return true;
#endif
}

void SpotifyProcess::ensureCredentials() {
    // Try to copy credentials from spotify-player cache if available
    QString sourceCredPath;
#ifdef _WIN32
    sourceCredPath = QDir::homePath() + QStringLiteral("/.cache/spotify-player/credentials.json");
    if (!QFile::exists(sourceCredPath)) {
        sourceCredPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                         QStringLiteral("/spotify-player/credentials.json");
    }
#else
    sourceCredPath = QDir::homePath() + QStringLiteral("/.cache/spotify-player/credentials.json");
#endif

    QString destCredPath = m_cacheDir + QStringLiteral("/credentials.json");
    if (!QFile::exists(destCredPath) && QFile::exists(sourceCredPath)) {
        if (!QFile::copy(sourceCredPath, destCredPath)) {
            qWarning() << "SpotifyProcess: Failed to copy credentials from" << sourceCredPath;
        } else {
            qDebug() << "SpotifyProcess: Copied credentials to" << destCredPath;
#ifndef _WIN32
            // Make it readable only by owner (librespot complains about world-readable)
            chmod(destCredPath.toLocal8Bit().constData(), 0600);
#endif
        }
    }
}

bool SpotifyProcess::start() {
    if (m_pProcess && m_pProcess->state() != QProcess::NotRunning) {
        qDebug() << "SpotifyProcess: Already running";
        return true;
    }

    if (!createPipe()) {
        return false;
    }

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
#else
    args << QStringLiteral("--device") << m_pipePath;
#endif

    args << QStringLiteral("--format") << QStringLiteral("F32")
         << QStringLiteral("--bitrate") << QString::number(kBitrate)
         << QStringLiteral("--name") << deviceName()
         << QStringLiteral("--cache") << m_cacheDir
         << QStringLiteral("--disable-discovery")
         << QStringLiteral("--enable-oauth");

    qDebug() << "SpotifyProcess: Starting librespot:" << m_librespotPath;

    m_stopping = false;

#ifdef _WIN32
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

QString SpotifyProcess::findDeviceId(const QString& accessToken) {
    QUrl url(QStringLiteral("https://api.spotify.com/v1/me/player/devices"));
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", ("Bearer " + accessToken).toUtf8());

    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.get(request);

    // Wait for reply (with timeout)
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(5000);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start();
    loop.exec();

    QString deviceId;
    if (reply->error() == QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonArray devices = doc.object().value(QStringLiteral("devices")).toArray();
        for (const auto& dev : devices) {
            QJsonObject obj = dev.toObject();
            if (obj.value(QStringLiteral("name")).toString() == deviceName()) {
                deviceId = obj.value(QStringLiteral("id")).toString();
                qDebug() << "SpotifyProcess: Found device:" << deviceId;
                break;
            }
        }
    } else {
        qWarning() << "SpotifyProcess: Failed to get devices:" << reply->errorString();
    }

    reply->deleteLater();
    return deviceId;
}

bool SpotifyProcess::startPlayback(const QString& trackUri,
                                   const QString& deviceId,
                                   const QString& accessToken) {
    QUrl url(QStringLiteral("https://api.spotify.com/v1/me/player/play"));
    url.addQueryItem(QStringLiteral("device_id"), deviceId);

    QJsonObject body;
    QJsonArray uris;
    uris.append(trackUri);
    body[QStringLiteral("uris")] = uris;
    body[QStringLiteral("position_ms")] = 0;

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", ("Bearer " + accessToken).toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.put(request, QJsonDocument(body).toJson());

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(5000);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start();
    loop.exec();

    bool success = (reply->error() == QNetworkReply::NoError);
    if (!success) {
        qWarning() << "SpotifyProcess: Playback start failed:" << reply->errorString()
                    << "HTTP" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    }
    reply->deleteLater();
    return success;
}

void SpotifyProcess::pausePlayback(const QString& accessToken) {
    QUrl url(QStringLiteral("https://api.spotify.com/v1/me/player/pause"));
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", ("Bearer " + accessToken).toUtf8());

    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.put(request, QByteArray());

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(5000);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start();
    loop.exec();

    reply->deleteLater();
}

bool SpotifyProcess::convertToWav(const QString& rawPath, const QString& wavPath) {
    // Find ffmpeg executable
    QString ffmpegPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpegPath.isEmpty()) {
        qWarning() << "SpotifyProcess: ffmpeg not found in PATH";
        return false;
    }

    QProcess ffmpeg;
    QStringList args;
    args << QStringLiteral("-y")  // Overwrite output
         << QStringLiteral("-f") << QStringLiteral("f32le")  // Input format: 32-bit float LE
         << QStringLiteral("-ar") << QStringLiteral("44100")  // Sample rate
         << QStringLiteral("-ac") << QStringLiteral("2")  // Channels (stereo)
         << QStringLiteral("-i") << rawPath  // Input file
         << wavPath;  // Output file

    qDebug() << "SpotifyProcess: Converting to WAV:" << ffmpegPath << args;

    ffmpeg.start(ffmpegPath, args);
    if (!ffmpeg.waitForFinished(60000)) {  // 60 second timeout
        qWarning() << "SpotifyProcess: ffmpeg timed out or failed";
        ffmpeg.kill();
        return false;
    }

    if (ffmpeg.exitCode() != 0) {
        qWarning() << "SpotifyProcess: ffmpeg failed with code" << ffmpeg.exitCode();
        qWarning() << ffmpeg.readAllStandardError();
        return false;
    }

    return QFile::exists(wavPath) && QFile(wavPath).size() > 1000;
}

bool SpotifyProcess::downloadTrack(const QString& trackUri,
                                   const QString& outputPath,
                                   int durationMs,
                                   const QString& accessToken) {
    qDebug() << "SpotifyProcess: Starting download:" << trackUri << "→" << outputPath;

    if (accessToken.isEmpty()) {
        qWarning() << "SpotifyProcess: No access token provided";
        return false;
    }

    // Ensure output directory exists
    QFileInfo fileInfo(outputPath);
    QDir().mkpath(fileInfo.dir().path());

    // Start librespot if not running
    if (!isRunning()) {
        if (!start()) {
            qWarning() << "SpotifyProcess: Failed to start librespot";
            return false;
        }
    }

    // Wait for device registration
    QThread::msleep(kDeviceRegistrationWaitMs);

    // Find our device
    QString deviceId = findDeviceId(accessToken);
    if (deviceId.isEmpty()) {
        qWarning() << "SpotifyProcess: Device not found after waiting";
        return false;
    }

    // Start playback
    if (!startPlayback(trackUri, deviceId, accessToken)) {
        qWarning() << "SpotifyProcess: Failed to start playback";
        pausePlayback(accessToken);
        return false;
    }

    QThread::msleep(kPlaybackStartWaitMs);

    // Capture raw audio
    QString rawPath = outputPath + QStringLiteral(".raw");
    QFile rawFile(rawPath);
    if (!rawFile.open(QIODevice::WriteOnly)) {
        qWarning() << "SpotifyProcess: Failed to open raw file:" << rawPath;
        pausePlayback(accessToken);
        return false;
    }

    qDebug() << "SpotifyProcess: Recording audio for" << durationMs << "ms...";

    // Read from pipe for durationMs + 2 seconds to ensure we get all audio
    int recordTimeMs = durationMs + 2000;
    QTime startTime = QTime::currentTime();
    char buffer[32768];
    int totalBytesRead = 0;

    while (startTime.msecsTo(QTime::currentTime()) < recordTimeMs) {
        if (waitForAudioData(100)) {
            qint64 bytesRead = readAudioData(buffer, sizeof(buffer));
            if (bytesRead > 0) {
                rawFile.write(buffer, bytesRead);
                totalBytesRead += bytesRead;
            }
        }
        QCoreApplication::processEvents();  // Keep Qt responsive
    }

    rawFile.close();
    qDebug() << "SpotifyProcess: Recorded" << totalBytesRead << "bytes";

    // Stop playback
    pausePlayback(accessToken);

    // Check if we got data
    if (totalBytesRead < 1000) {
        qWarning() << "SpotifyProcess: Insufficient audio data recorded:" << totalBytesRead;
        QFile::remove(rawPath);
        return false;
    }

    // Convert to WAV
    if (!convertToWav(rawPath, outputPath)) {
        qWarning() << "SpotifyProcess: Failed to convert to WAV";
        QFile::remove(rawPath);
        return false;
    }

    // Clean up raw file
    QFile::remove(rawPath);

    qDebug() << "SpotifyProcess: Download complete:" << outputPath << "("
             << QFile(outputPath).size() << "bytes)";
    return true;
}

void SpotifyProcess::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    qDebug() << "SpotifyProcess: librespot finished, exit code:" << exitCode
             << "status:" << exitStatus;
    if (!m_stopping) {
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
