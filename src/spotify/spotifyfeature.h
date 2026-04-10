#pragma once

#include "library/baseexternallibraryfeature.h"
#include "library/treeitemmodel.h"
#include "spotify/spotifyapiclient.h"
#include "spotify/spotifyprocess.h"

#include <QAction>
#include <QJsonArray>
#include <QProcess>
#include <QSharedPointer>
#include <QSqlDatabase>
#include <memory>

class BaseExternalTrackModel;
class BaseExternalPlaylistModel;
class BaseTrackCache;
class Library;

namespace mixxx {

/// Library feature that provides Spotify browsing in the Mixxx sidebar.
/// Shows playlists, saved tracks, and search functionality.
/// Downloads tracks as WAV files via librespot and stores them in the cache.
class SpotifyFeature : public BaseExternalLibraryFeature {
    Q_OBJECT

  public:
    SpotifyFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~SpotifyFeature() override;

    QVariant title() override;
    bool dropAccept(const QList<QUrl>& urls, QObject* pSource) override;
    bool dragMoveAccept(const QList<QUrl>& urls) override;
    TreeItemModel* sidebarModel() const override;

    /// Check if Spotify integration is available
    static bool isSupported();

    /// Get the cache directory for Spotify track downloads
    static QString getSpotifyTracksDir();

    /// Download a Spotify track as a WAV file (blocking).
    /// @return Path to WAV file, or empty string on failure
    QString downloadSpotifyTrack(const QString& trackId,
                                 const QString& trackUri,
                                 int durationMs);

    /// Check if a track is already downloaded
    bool isTrackDownloaded(const QString& trackId) const;

    /// Get the WAV path for a track
    QString trackWavPath(const QString& trackId) const;

  public slots:
    void activate() override;
    void activateChild(const QModelIndex& index) override;
    void onRightClick(const QPoint& globalPos) override;
    void onRightClickChild(const QPoint& globalPos,
            const QModelIndex& index) override;

  private slots:
    void onPlaylistsReceived(const QJsonArray& playlists);
    void onTracksReceived(const QJsonArray& tracks);
    void onAudioFeaturesReceived(const QJsonArray& features);
    void onApiError(const QString& message);

  private:
    void buildSidebarModel();
    void loadPlaylists();
    void startTokenServer();
    void stopTokenServer();
    QString findTokenServerPath() const;

    // SQL table management
    void createDbTables();
    void dropDbTables();
    void clearDbTables();
    void insertTracksIntoDb(const QJsonArray& tracks);
    void insertPlaylistIntoDb(int playlistId, const QString& name);
    void insertPlaylistTracksIntoDb(int playlistId, const QJsonArray& tracks);
    void updateAudioFeaturesInDb(const QJsonArray& features);

    std::unique_ptr<SpotifyApiClient> m_pApiClient;
    std::unique_ptr<SpotifyProcess> m_pDownloader;  // For downloading tracks
    QProcess* m_pTokenServerProcess;
    TreeItemModel* m_pSidebarModel;

    // SQL-backed track models
    BaseExternalTrackModel* m_pTrackModel;
    BaseExternalPlaylistModel* m_pPlaylistModel;
    QSharedPointer<BaseTrackCache> m_trackSource;

    // Playlist data cache
    QJsonArray m_playlists;
    QJsonArray m_currentTracks;
    QMap<QString, QJsonObject> m_audioFeaturesCache; // trackId → features
    QMap<QString, QString> m_trackUriCache;  // trackId → spotify:track:URI
    QMap<QString, int> m_trackDurationCache;  // trackId → durationMs

    // Track the currently active playlist (or -1 for "all tracks")
    int m_currentPlaylistId;
    bool m_isActivated;

    // Auto-incrementing playlist ID for DB
    int m_nextPlaylistId;

    // Map from playlist name to DB playlist ID
    QMap<QString, int> m_playlistDbIds;

    // Child node names
    static const QString kPlaylistsNode;
    static const QString kSavedTracksNode;
    static const QString kSearchNode;

    // SQL table names
    static const QString kSpotifyLibraryTable;
    static const QString kSpotifyPlaylistsTable;
    static const QString kSpotifyPlaylistTracksTable;
};

} // namespace mixxx
