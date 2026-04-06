#pragma once

#include "library/libraryfeature.h"
#include "library/treeitemmodel.h"
#include "spotify/spotifyapiclient.h"

#include <QAction>
#include <QJsonArray>
#include <QStandardItemModel>
#include <memory>

class Library;

namespace mixxx {

/// Library feature that provides Spotify browsing in the Mixxx sidebar.
/// Shows playlists, saved tracks, and search functionality.
class SpotifyFeature : public LibraryFeature {
    Q_OBJECT

  public:
    SpotifyFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~SpotifyFeature() override;

    QVariant title() override;
    bool dropAccept(const QList<QUrl>& urls, QObject* pSource) override;
    bool dragMoveAccept(const QUrl& url) override;
    void bindLibraryWidget(WLibrary* libraryWidget,
            KeyboardEventFilter* keyboard) override;
    void bindSidebarWidget(WLibrarySidebar* pSidebarWidget) override;
    TreeItemModel* sidebarModel() const override;

    /// Check if Spotify integration is available
    static bool isSupported();

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

    std::unique_ptr<SpotifyApiClient> m_pApiClient;
    TreeItemModel* m_pSidebarModel;

    // Playlist data cache
    QJsonArray m_playlists;
    QJsonArray m_currentTracks;
    QMap<QString, QJsonObject> m_audioFeaturesCache; // trackId → features

    // Child node names
    static const QString kPlaylistsNode;
    static const QString kSavedTracksNode;
    static const QString kSearchNode;
};

} // namespace mixxx
