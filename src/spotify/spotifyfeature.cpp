#include "spotify/spotifyfeature.h"

#include <QInputDialog>
#include <QMessageBox>
#include <QtDebug>

#include "library/library.h"

namespace mixxx {

const QString SpotifyFeature::kPlaylistsNode = QStringLiteral("Playlists");
const QString SpotifyFeature::kSavedTracksNode = QStringLiteral("Saved Tracks");
const QString SpotifyFeature::kSearchNode = QStringLiteral("Search");

SpotifyFeature::SpotifyFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("spotify")),
          m_pApiClient(std::make_unique<SpotifyApiClient>()),
          m_pSidebarModel(nullptr) {
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

    buildSidebarModel();
}

SpotifyFeature::~SpotifyFeature() = default;

/*static*/ bool SpotifyFeature::isSupported() {
    // Check if token server is likely running (we can't block here to test)
    // Just check if librespot binary exists
    QStringList paths = {
            QDir::homePath() + QStringLiteral("/clawd/mixxx-spotify/librespot/target/release/librespot"),
            QStringLiteral("/usr/local/bin/librespot"),
            QStringLiteral("/usr/bin/librespot"),
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

void SpotifyFeature::bindLibraryWidget(
        WLibrary* libraryWidget,
        KeyboardEventFilter* keyboard) {
    Q_UNUSED(libraryWidget)
    Q_UNUSED(keyboard)
    // TODO: Create a custom widget for displaying Spotify tracks
    // For now, we'll use the default table view
}

void SpotifyFeature::bindSidebarWidget(WLibrarySidebar* pSidebarWidget) {
    Q_UNUSED(pSidebarWidget)
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
    loadPlaylists();
    emit switchToView(QStringLiteral("spotify"));
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
    } else if (itemName == kSavedTracksNode) {
        m_pApiClient->fetchSavedTracks();
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
            m_pApiClient->searchTracks(query);
        }
    } else {
        // It's a playlist name — find it and load tracks
        for (const auto& pl : m_playlists) {
            QJsonObject obj = pl.toObject();
            if (obj.value(QStringLiteral("name")).toString() == itemName) {
                QString playlistId = obj.value(QStringLiteral("id")).toString();
                m_pApiClient->fetchPlaylistTracks(playlistId);
                break;
            }
        }
    }
}

void SpotifyFeature::onRightClick(const QPoint& globalPos) {
    Q_UNUSED(globalPos)
}

void SpotifyFeature::onRightClickChild(
        const QPoint& globalPos,
        const QModelIndex& index) {
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
    for (const auto& pl : playlists) {
        QJsonObject obj = pl.toObject();
        QString name = obj.value(QStringLiteral("name")).toString();
        int trackCount = obj.value(QStringLiteral("tracks"))
                                 .toObject()
                                 .value(QStringLiteral("total"))
                                 .toInt();
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

    qDebug() << "SpotifyFeature: Received" << tracks.size() << "tracks";

    // TODO: Display tracks in the library table
    // For now, log them
    for (const auto& track : tracks) {
        QJsonObject obj = track.toObject();
        QString name = obj.value(QStringLiteral("name")).toString();
        QJsonArray artists = obj.value(QStringLiteral("artists")).toArray();
        QString artist;
        if (!artists.isEmpty()) {
            artist = artists.first()
                             .toObject()
                             .value(QStringLiteral("name"))
                             .toString();
        }
        int durationMs = obj.value(QStringLiteral("duration_ms")).toInt();
        QString uri = obj.value(QStringLiteral("uri")).toString();
        qDebug() << "  Track:" << artist << "-" << name
                  << "(" << (durationMs / 1000) << "s)" << uri;
    }
}

void SpotifyFeature::onAudioFeaturesReceived(const QJsonArray& features) {
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

} // namespace mixxx
