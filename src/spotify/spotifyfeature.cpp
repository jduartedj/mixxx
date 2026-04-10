#include "spotify/spotifyfeature.h"

#include <QCoreApplication>
#include <QDir>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QtDebug>

#include "library/baseexternalplaylistmodel.h"
#include "library/baseexternaltrackmodel.h"
#include "library/basetrackcache.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/queryutil.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"

namespace mixxx {

const QString SpotifyFeature::kPlaylistsNode = QStringLiteral("Playlists");
const QString SpotifyFeature::kSavedTracksNode = QStringLiteral("Saved Tracks");
const QString SpotifyFeature::kSearchNode = QStringLiteral("Search");
const QString SpotifyFeature::kSpotifyLibraryTable = QStringLiteral("spotify_library");
const QString SpotifyFeature::kSpotifyPlaylistsTable = QStringLiteral("spotify_playlists");
const QString SpotifyFeature::kSpotifyPlaylistTracksTable = QStringLiteral("spotify_playlist_tracks");

SpotifyFeature::SpotifyFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : BaseExternalLibraryFeature(pLibrary, pConfig, QStringLiteral("spotify")),
          m_pApiClient(std::make_unique<SpotifyApiClient>()),
          m_pDownloader(nullptr),
          m_pTokenServerProcess(nullptr),
          m_pSidebarModel(nullptr),
          m_pTrackModel(nullptr),
          m_pPlaylistModel(nullptr),
          m_currentPlaylistId(-1),
          m_isActivated(false),
          m_nextPlaylistId(1) {
    // Start the token server automatically
    startTokenServer();

    // Connect API signals
    connect(m_pApiClient.get(),
            &SpotifyApiClient::playlistsReceived,
            this,
            &SpotifyFeature::onPlaylistsReceived);
    connect(m_pApiClient.get(),
            &SpotifyApiClient::tracksReceived,
            this,
            &SpotifyFeature::onTracksReceived);
    connect(m_pApiClient.get(),
            &SpotifyApiClient::audioFeaturesReceived,
            this,
            &SpotifyFeature::onAudioFeaturesReceived);
    connect(m_pApiClient.get(),
            &SpotifyApiClient::error,
            this,
            &SpotifyFeature::onApiError);

    // Create SQL tables
    createDbTables();

    // Set up track cache and models
    QString tableName = kSpotifyLibraryTable;
    QString idColumn = LIBRARYTABLE_ID;
    QStringList columns = {
            LIBRARYTABLE_ID,
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_YEAR,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT,
            LIBRARYTABLE_RATING,
            LIBRARYTABLE_DURATION,
            LIBRARYTABLE_BITRATE,
            LIBRARYTABLE_BPM,
            LIBRARYTABLE_KEY,
    };
    QStringList searchColumns = {
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_GENRE,
    };

    m_trackSource = QSharedPointer<BaseTrackCache>::create(
            m_pTrackCollection,
            tableName,
            idColumn,
            columns,
            searchColumns,
            false);

    m_pTrackModel = new BaseExternalTrackModel(this,
            pLibrary->trackCollectionManager(),
            "mixxx.db.model.spotify",
            kSpotifyLibraryTable,
            m_trackSource);

    m_pPlaylistModel = new BaseExternalPlaylistModel(this,
            pLibrary->trackCollectionManager(),
            "mixxx.db.model.spotify_playlist",
            kSpotifyPlaylistsTable,
            kSpotifyPlaylistTracksTable,
            m_trackSource);

    buildSidebarModel();
}

SpotifyFeature::~SpotifyFeature() {
    if (m_pTrackModel) {
        delete m_pTrackModel;
    }
    if (m_pPlaylistModel) {
        delete m_pPlaylistModel;
    }
    stopTokenServer();
    dropDbTables();
}

/*static*/ bool SpotifyFeature::isSupported() {
    // Check if token server is likely running (we can't block here to test)
    // Just check if librespot binary exists
    QStringList paths = {
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
    for (const auto& path : paths) {
        if (QFile::exists(path)) {
            return true;
        }
    }
    return false;
}

QVariant SpotifyFeature::title() {
    return QVariant(QStringLiteral("Spotify"));
}

bool SpotifyFeature::dropAccept(const QList<QUrl>& urls, QObject* pSource) {
    Q_UNUSED(urls)
    Q_UNUSED(pSource)
    return false;
}

bool SpotifyFeature::dragMoveAccept(const QList<QUrl>& urls) {
    Q_UNUSED(urls)
    return false;
}

TreeItemModel* SpotifyFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void SpotifyFeature::buildSidebarModel() {
    m_pSidebarModel = new TreeItemModel(this);

    auto pRootItem = TreeItem::newRoot(this);
    pRootItem->appendChild(kPlaylistsNode);
    pRootItem->appendChild(kSavedTracksNode);
    pRootItem->appendChild(kSearchNode);

    m_pSidebarModel->setRootItem(std::move(pRootItem));
}

void SpotifyFeature::activate() {
    qDebug() << "SpotifyFeature: Activated";
    if (!m_isActivated) {
        clearDbTables();
        m_isActivated = true;
    }
    m_currentPlaylistId = -1; // Show all tracks
    loadPlaylists();
    m_pTrackModel->setSearch("");
    emit showTrackModel(m_pTrackModel);
    emit enableCoverArtDisplay(false);
}

void SpotifyFeature::activateChild(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }

    TreeItem* pItem = static_cast<TreeItem*>(index.internalPointer());
    if (!pItem) {
        return;
    }

    QString itemName = pItem->getLabel();
    qDebug() << "SpotifyFeature: Activated child:" << itemName;

    if (itemName == kPlaylistsNode) {
        loadPlaylists();
        m_currentPlaylistId = -1;
        m_pTrackModel->setSearch("");
        emit showTrackModel(m_pTrackModel);
    } else if (itemName == kSavedTracksNode) {
        clearDbTables();
        m_pApiClient->fetchSavedTracks();
        m_currentPlaylistId = -1;
        emit showTrackModel(m_pTrackModel);
    } else if (itemName == kSearchNode) {
        bool ok;
        QString query = QInputDialog::getText(
                nullptr,
                QStringLiteral("Spotify Search"),
                QStringLiteral("Search tracks:"),
                QLineEdit::Normal,
                QString(),
                &ok);
        if (ok && !query.isEmpty()) {
            clearDbTables();
            m_pApiClient->searchTracks(query);
            emit showTrackModel(m_pTrackModel);
        }
    } else {
        // It's a playlist name — find it and load tracks
        for (const auto& pl : m_playlists) {
            QJsonObject obj = pl.toObject();
            if (obj.value(QStringLiteral("name")).toString() == itemName) {
                QString playlistId = obj.value(QStringLiteral("id")).toString();
                int dbPlaylistId = m_playlistDbIds.value(itemName, -1);
                if (dbPlaylistId > 0) {
                    m_currentPlaylistId = dbPlaylistId;
                    m_pPlaylistModel->setPlaylistById(dbPlaylistId);
                    emit showTrackModel(m_pPlaylistModel);
                }
                m_pApiClient->fetchPlaylistTracks(playlistId);
                break;
            }
        }
    }
}

void SpotifyFeature::onRightClick(const QPoint& globalPos) {
    Q_UNUSED(globalPos)
}

void SpotifyFeature::onRightClickChild(const QPoint& globalPos, const QModelIndex& index) {
    Q_UNUSED(globalPos)
    Q_UNUSED(index)
}

void SpotifyFeature::loadPlaylists() {
    m_pApiClient->fetchPlaylists();
}

void SpotifyFeature::onPlaylistsReceived(const QJsonArray& playlists) {
    m_playlists = playlists;

    // Rebuild sidebar with playlist names under the Playlists node
    auto pRootItem = TreeItem::newRoot(this);

    auto* pPlaylistsItem = pRootItem->appendChild(kPlaylistsNode);
    m_nextPlaylistId = 1;
    m_playlistDbIds.clear();
    for (const auto& pl : playlists) {
        QJsonObject obj = pl.toObject();
        QString name = obj.value(QStringLiteral("name")).toString();
        int trackCount = obj.value(QStringLiteral("tracks"))
                                 .toObject()
                                 .value(QStringLiteral("total"))
                                 .toInt();
        insertPlaylistIntoDb(m_nextPlaylistId, name);
        m_playlistDbIds[name] = m_nextPlaylistId;
        m_nextPlaylistId++;

        pPlaylistsItem->appendChild(
                QStringLiteral("%1 (%2)").arg(name).arg(trackCount));
    }

    pRootItem->appendChild(kSavedTracksNode);
    pRootItem->appendChild(kSearchNode);

    m_pSidebarModel->setRootItem(std::move(pRootItem));

    qDebug() << "SpotifyFeature: Loaded" << playlists.size() << "playlists";
}

void SpotifyFeature::onTracksReceived(const QJsonArray& tracks) {
    m_currentTracks = tracks;

    // Insert tracks into DB
    insertTracksIntoDb(tracks);

    // Fetch audio features for all tracks
    QStringList trackIds;
    for (const auto& track : tracks) {
        QJsonObject obj = track.toObject();
        QString id = obj.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            trackIds.append(id);
        }
    }

    if (!trackIds.isEmpty()) {
        m_pApiClient->fetchAudioFeatures(trackIds);
    }

    // Rebuild the track model
    m_trackSource->buildIndex();
    m_pTrackModel->setSearch("");
    if (m_currentPlaylistId > 0) {
        m_pPlaylistModel->setPlaylistById(m_currentPlaylistId);
        emit showTrackModel(m_pPlaylistModel);
    } else {
        emit showTrackModel(m_pTrackModel);
    }

    qDebug() << "SpotifyFeature: Received" << tracks.size() << "tracks";
}

void SpotifyFeature::onAudioFeaturesReceived(const QJsonArray& features) {
    updateAudioFeaturesInDb(features);
    for (const auto& feat : features) {
        QJsonObject obj = feat.toObject();
        if (obj.isEmpty()) {
            continue;
        }
        QString id = obj.value(QStringLiteral("id")).toString();
        m_audioFeaturesCache[id] = obj;

        // Log useful DJ info
        double tempo = obj.value(QStringLiteral("tempo")).toDouble();
        int key = obj.value(QStringLiteral("key")).toInt(-1);
        int mode = obj.value(QStringLiteral("mode")).toInt(-1);
        QString camelot = SpotifyApiClient::toCamelotKey(key, mode);

        qDebug() << "  AudioFeatures:" << id
                  << "BPM:" << tempo
                  << "Key:" << camelot
                  << "Energy:" << obj.value(QStringLiteral("energy")).toDouble()
                  << "Dance:" << obj.value(QStringLiteral("danceability")).toDouble();
    }
}

void SpotifyFeature::onApiError(const QString& message) {
    qWarning() << "SpotifyFeature: API error:" << message;
}

QString SpotifyFeature::findTokenServerPath() const {
    QStringList paths = {
#ifdef _WIN32
            QDir::homePath() + QStringLiteral("/spotify-token-generator/index.js"),
            QCoreApplication::applicationDirPath() + QStringLiteral("/spotify-token-generator/index.js"),
#else
            QDir::homePath() + QStringLiteral("/spotify-token-generator/index.js"),
            QStringLiteral("/opt/spotify-token-generator/index.js"),
#endif
    };
    for (const auto& path : paths) {
        if (QFile::exists(path)) {
            return path;
        }
    }
    return QString();
}

void SpotifyFeature::startTokenServer() {
    if (m_pTokenServerProcess &&
            m_pTokenServerProcess->state() != QProcess::NotRunning) {
        qDebug() << "SpotifyFeature: Token server already running";
        return;
    }

    QString serverPath = findTokenServerPath();
    if (serverPath.isEmpty()) {
        qWarning() << "SpotifyFeature: Token server index.js not found";
        return;
    }

    m_pTokenServerProcess = new QProcess(this);
    m_pTokenServerProcess->setWorkingDirectory(
            QFileInfo(serverPath).absolutePath());

    connect(m_pTokenServerProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus status) {
                qWarning() << "SpotifyFeature: Token server exited"
                           << "code:" << exitCode << "status:" << status;
            });

    // Find node executable
    QString nodeBin = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (nodeBin.isEmpty()) {
#ifdef _WIN32
        // Fallback: check common Windows locations
        QStringList nodePaths = {
                QStringLiteral("C:/nvm4w/nodejs/node.exe"),
                QDir::homePath() + QStringLiteral("/AppData/Local/nvm/v22.20.0/node.exe"),
        };
        for (const auto& p : nodePaths) {
            if (QFile::exists(p)) {
                nodeBin = p;
                break;
            }
        }
#endif
    }
    if (nodeBin.isEmpty()) {
        qWarning() << "SpotifyFeature: node executable not found, cannot start token server";
        return;
    }

    qDebug() << "SpotifyFeature: Starting token server:" << nodeBin << serverPath;
    m_pTokenServerProcess->start(nodeBin, {serverPath});

    if (!m_pTokenServerProcess->waitForStarted(5000)) {
        qWarning() << "SpotifyFeature: Failed to start token server";
        delete m_pTokenServerProcess;
        m_pTokenServerProcess = nullptr;
    } else {
        qDebug() << "SpotifyFeature: Token server started, PID:"
                 << m_pTokenServerProcess->processId();
    }
}

void SpotifyFeature::stopTokenServer() {
    if (m_pTokenServerProcess) {
        if (m_pTokenServerProcess->state() != QProcess::NotRunning) {
            m_pTokenServerProcess->terminate();
            if (!m_pTokenServerProcess->waitForFinished(3000)) {
                m_pTokenServerProcess->kill();
            }
        }
        delete m_pTokenServerProcess;
        m_pTokenServerProcess = nullptr;
    }
}

void SpotifyFeature::createDbTables() {
    QSqlDatabase database = m_pTrackCollection->database();

    qDebug() << "SpotifyFeature: Creating Spotify library tables";

    QSqlQuery query(database);

    // Create spotify_library table
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + kSpotifyLibraryTable +
            " ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    artist TEXT,"
            "    title TEXT,"
            "    album TEXT,"
            "    album_artist TEXT DEFAULT '',"
            "    year INTEGER,"
            "    genre TEXT,"
            "    tracknumber TEXT,"
            "    location TEXT UNIQUE,"
            "    comment TEXT,"
            "    duration INTEGER,"
            "    bitrate TEXT,"
            "    bpm FLOAT,"
            "    key TEXT,"
            "    rating INTEGER"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        qWarning() << "SpotifyFeature: Failed to create spotify_library table";
    }

    // Create spotify_playlists table
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + kSpotifyPlaylistsTable +
            " ("
            "    id INTEGER PRIMARY KEY,"
            "    name TEXT UNIQUE"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        qWarning() << "SpotifyFeature: Failed to create spotify_playlists table";
    }

    // Create spotify_playlist_tracks table
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + kSpotifyPlaylistTracksTable +
            " ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    playlist_id INTEGER REFERENCES spotify_playlists(id),"
            "    track_id INTEGER REFERENCES spotify_library(id),"
            "    position INTEGER"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        qWarning() << "SpotifyFeature: Failed to create spotify_playlist_tracks table";
    }
}

void SpotifyFeature::dropDbTables() {
    QSqlDatabase database = m_pTrackCollection->database();
    QSqlQuery query(database);

    qDebug() << "SpotifyFeature: Dropping Spotify library tables";

    query.prepare("DROP TABLE IF EXISTS " + kSpotifyPlaylistTracksTable);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }

    query.prepare("DROP TABLE IF EXISTS " + kSpotifyPlaylistsTable);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }

    query.prepare("DROP TABLE IF EXISTS " + kSpotifyLibraryTable);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }
}

void SpotifyFeature::clearDbTables() {
    QSqlDatabase database = m_pTrackCollection->database();
    QSqlQuery query(database);

    qDebug() << "SpotifyFeature: Clearing Spotify library tables";

    query.prepare("DELETE FROM " + kSpotifyPlaylistTracksTable);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }

    query.prepare("DELETE FROM " + kSpotifyPlaylistsTable);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }

    query.prepare("DELETE FROM " + kSpotifyLibraryTable);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }
}

void SpotifyFeature::insertTracksIntoDb(const QJsonArray& tracks) {
    QSqlDatabase database = m_pTrackCollection->database();
    QSqlQuery query(database);

    query.prepare(
            "INSERT OR REPLACE INTO " + kSpotifyLibraryTable +
            " (artist, title, album, year, genre, tracknumber, location, "
            "comment, duration, bitrate, bpm, key, rating) "
            "VALUES (:artist, :title, :album, :year, :genre, :tracknumber, "
            ":location, :comment, :duration, :bitrate, :bpm, :key, :rating)");

    int trackId = 1;
    for (const auto& track : tracks) {
        QJsonObject obj = track.toObject();

        QString artist;
        QJsonArray artists = obj.value(QStringLiteral("artists")).toArray();
        if (!artists.isEmpty()) {
            artist = artists.first()
                             .toObject()
                             .value(QStringLiteral("name"))
                             .toString();
        }

        QString title = obj.value(QStringLiteral("name")).toString();
        QString album = obj.value(QStringLiteral("album"))
                                .toObject()
                                .value(QStringLiteral("name"))
                                .toString();
        int durationMs = obj.value(QStringLiteral("duration_ms")).toInt();
        QString trackId = obj.value(QStringLiteral("id")).toString();
        QString uri = obj.value(QStringLiteral("uri")).toString();

        // Cache track URI and duration for later download
        m_trackUriCache[trackId] = uri;
        m_trackDurationCache[trackId] = durationMs;

        // Use placeholder WAV path as location (download on first load)
        QString wavPath = trackWavPath(trackId);

        query.bindValue(":artist", artist);
        query.bindValue(":title", title);
        query.bindValue(":album", album);
        query.bindValue(":year", 0);
        query.bindValue(":genre", QString());
        query.bindValue(":tracknumber", QString());
        query.bindValue(":location", wavPath);
        query.bindValue(":comment", QString());
        query.bindValue(":duration", durationMs / 1000); // Convert to seconds
        query.bindValue(":bitrate", QString());
        query.bindValue(":bpm", 0.0);
        query.bindValue(":key", QString());
        query.bindValue(":rating", 0);

        if (!query.exec()) {
            LOG_FAILED_QUERY(query);
            qWarning() << "SpotifyFeature: Failed to insert track:" << title;
        }
        trackId++;
    }

    qDebug() << "SpotifyFeature: Inserted" << tracks.size() << "tracks into database";
}

void SpotifyFeature::insertPlaylistIntoDb(int playlistId, const QString& name) {
    QSqlDatabase database = m_pTrackCollection->database();
    QSqlQuery query(database);

    query.prepare(
            "INSERT OR REPLACE INTO " + kSpotifyPlaylistsTable +
            " (id, name) VALUES (:id, :name)");

    query.bindValue(":id", playlistId);
    query.bindValue(":name", name);

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        qWarning() << "SpotifyFeature: Failed to insert playlist:" << name;
    }
}

void SpotifyFeature::insertPlaylistTracksIntoDb(int playlistId, const QJsonArray& tracks) {
    QSqlDatabase database = m_pTrackCollection->database();
    QSqlQuery query(database);

    query.prepare(
            "INSERT INTO " + kSpotifyPlaylistTracksTable +
            " (playlist_id, track_id, position) "
            "VALUES (:playlist_id, :track_id, :position)");

    // First, get the track IDs from the library table by matching artist + title
    QSqlQuery selectQuery(database);

    int position = 0;
    for (const auto& track : tracks) {
        QJsonObject obj = track.toObject();
        QString uri = obj.value(QStringLiteral("uri")).toString();

        // Find the track ID by URI
        selectQuery.prepare(
                "SELECT id FROM " + kSpotifyLibraryTable +
                " WHERE location = :location");
        selectQuery.bindValue(":location", uri);

        if (!selectQuery.exec()) {
            LOG_FAILED_QUERY(selectQuery);
            continue;
        }

        if (selectQuery.next()) {
            int trackId = selectQuery.value(0).toInt();

            query.bindValue(":playlist_id", playlistId);
            query.bindValue(":track_id", trackId);
            query.bindValue(":position", position);

            if (!query.exec()) {
                LOG_FAILED_QUERY(query);
            }
            position++;
        }
    }

    qDebug() << "SpotifyFeature: Inserted" << tracks.size()
             << "playlist tracks into database";
}

void SpotifyFeature::updateAudioFeaturesInDb(const QJsonArray& features) {
    QSqlDatabase database = m_pTrackCollection->database();
    QSqlQuery query(database);

    query.prepare(
            "UPDATE " + kSpotifyLibraryTable +
            " SET bpm = :bpm, key = :key WHERE location = :location");

    for (const auto& feat : features) {
        QJsonObject obj = feat.toObject();
        if (obj.isEmpty()) {
            continue;
        }

        QString id = obj.value(QStringLiteral("id")).toString();
        double tempo = obj.value(QStringLiteral("tempo")).toDouble();
        int key = obj.value(QStringLiteral("key")).toInt(-1);
        int mode = obj.value(QStringLiteral("mode")).toInt(-1);
        QString camelot = SpotifyApiClient::toCamelotKey(key, mode);

        // Find track by ID and update BPM/key
        QSqlQuery findQuery(database);
        findQuery.prepare(
                "SELECT location FROM " + kSpotifyLibraryTable +
                " WHERE location LIKE :id");
        findQuery.bindValue(":id", "%:track:" + id);

        if (!findQuery.exec()) {
            continue;
        }

        while (findQuery.next()) {
            QString location = findQuery.value(0).toString();

            query.bindValue(":bpm", tempo);
            query.bindValue(":key", camelot);
            query.bindValue(":location", location);

            if (!query.exec()) {
                LOG_FAILED_QUERY(query);
            }
        }
    }
QString SpotifyFeature::getSpotifyTracksDir() {
    QString tracksDir = QStandardPaths::writableLocation(
                    QStandardPaths::GenericCacheLocation) +
            QStringLiteral("/mixxx/spotify_tracks");
    QDir().mkpath(tracksDir);
    return tracksDir;
}

bool SpotifyFeature::isTrackDownloaded(const QString& trackId) const {
    return QFile::exists(trackWavPath(trackId));
}

QString SpotifyFeature::trackWavPath(const QString& trackId) const {
    return getSpotifyTracksDir() + QStringLiteral("/") + trackId + QStringLiteral(".wav");
}

QString SpotifyFeature::downloadSpotifyTrack(const QString& trackId,
                                             const QString& trackUri,
                                             int durationMs) {
    QString wavPath = trackWavPath(trackId);

    // Already downloaded?
    if (QFile::exists(wavPath)) {
        qDebug() << "SpotifyFeature: Track already cached:" << wavPath;
        return wavPath;
    }

    qDebug() << "SpotifyFeature: Starting download of" << trackId;

    // Ensure API client is authenticated
    if (!m_pApiClient->isAuthenticated()) {
        qWarning() << "SpotifyFeature: Not authenticated, cannot download";
        return QString();
    }

    // Create downloader if needed
    if (!m_pDownloader) {
        // Find librespot path
        QStringList librespotPaths = {
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

        QString librespotPath;
        for (const auto& path : librespotPaths) {
            if (QFile::exists(path)) {
                librespotPath = path;
                break;
            }
        }

        if (librespotPath.isEmpty()) {
            qWarning() << "SpotifyFeature: librespot binary not found";
            return QString();
        }

        m_pDownloader = std::make_unique<SpotifyProcess>(librespotPath, 0);
    }

    // Get access token from API client
    QString accessToken = m_pApiClient->getAccessToken();

    if (accessToken.isEmpty()) {
        qWarning() << "SpotifyFeature: No access token available";
        return QString();
    }

    // Download the track
    if (!m_pDownloader->downloadTrack(trackUri, wavPath, durationMs, accessToken)) {
        qWarning() << "SpotifyFeature: Download failed for" << trackId;
        return QString();
    }

    qDebug() << "SpotifyFeature: Downloaded track:" << wavPath;
    return wavPath;
}



} // namespace mixxx

#include "moc_spotifyfeature.cpp"
