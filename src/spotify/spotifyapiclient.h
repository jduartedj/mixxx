#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QTimer>

namespace mixxx {

/// Qt HTTP client for the Spotify Web API.
/// Uses the local token server at localhost:37353 for authentication.
class SpotifyApiClient : public QObject {
    Q_OBJECT

  public:
    explicit SpotifyApiClient(QObject* parent = nullptr);
    ~SpotifyApiClient() override;

    /// Fetch the user's playlists
    void fetchPlaylists(int offset = 0, int limit = 50);

    /// Fetch tracks from a playlist
    void fetchPlaylistTracks(const QString& playlistId, int offset = 0, int limit = 100);

    /// Fetch user's saved/liked tracks
    void fetchSavedTracks(int offset = 0, int limit = 50);

    /// Search for tracks
    void searchTracks(const QString& query, int limit = 50);

    /// Fetch audio features for a batch of track IDs (up to 100)
    void fetchAudioFeatures(const QStringList& trackIds);

    /// Start playback of a track URI on a specific device
    void play(const QString& trackUri, int positionMs = 0, const QString& deviceId = QString());

    /// Seek to position in currently playing track
    void seek(int positionMs);

    /// Pause playback
    void pause();

    /// Get available playback devices
    void getDevices();

    /// Transfer playback to a device
    void transferPlayback(const QString& deviceId, bool play = false);

    /// @return true if we have a valid access token
    bool isAuthenticated() const {
        return !m_accessToken.isEmpty();
    }

    /// @return the access token for API calls (for download operations)
    QString getAccessToken() const {
        return m_accessToken;
    }

    /// Convert Spotify key/mode to Camelot notation
    static QString toCamelotKey(int key, int mode);

  signals:
    void playlistsReceived(const QJsonArray& playlists);
    void tracksReceived(const QJsonArray& tracks);
    void audioFeaturesReceived(const QJsonArray& features);
    void devicesReceived(const QJsonArray& devices);
    void playbackStarted();
    void seekComplete();
    void error(const QString& message);
    void authenticated();

  private slots:
    void onTokenReceived(QNetworkReply* reply);
    void refreshToken();

  private:
    void ensureToken(std::function<void()> callback);
    QNetworkReply* makeRequest(
            const QString& endpoint,
            const QByteArray& method = "GET",
            const QJsonObject& body = QJsonObject());

    QNetworkAccessManager* m_pNetwork;
    QString m_accessToken;
    QTimer* m_pTokenRefreshTimer;
    int m_tokenExpiresIn;

    // Pending callbacks waiting for token
    QList<std::function<void()>> m_pendingCallbacks;
    bool m_fetchingToken;

    // Rate limiting
    int m_requestsThisMinute;
    QTimer* m_pRateLimitTimer;

    static const QString kTokenServerUrl;
    static const QString kSpotifyApiBase;
    static const int kMaxRequestsPerMinute;
};

} // namespace mixxx
