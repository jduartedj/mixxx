#include "spotify/spotifyapiclient.h"

#include <QJsonDocument>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QtDebug>

namespace mixxx {

const QString SpotifyApiClient::kTokenServerUrl =
        QStringLiteral("http://127.0.0.1:37353/api/getToken");
const QString SpotifyApiClient::kSpotifyApiBase =
        QStringLiteral("https://api.spotify.com/v1");
const int SpotifyApiClient::kMaxRequestsPerMinute = 170; // Leave some headroom

SpotifyApiClient::SpotifyApiClient(QObject* parent)
        : QObject(parent),
          m_pNetwork(new QNetworkAccessManager(this)),
          m_tokenExpiresIn(0),
          m_fetchingToken(false),
          m_requestsThisMinute(0) {
    m_pTokenRefreshTimer = new QTimer(this);
    m_pTokenRefreshTimer->setSingleShot(true);
    connect(m_pTokenRefreshTimer, &QTimer::timeout, this, &SpotifyApiClient::refreshToken);

    m_pRateLimitTimer = new QTimer(this);
    m_pRateLimitTimer->setInterval(60000);
    connect(m_pRateLimitTimer, &QTimer::timeout, this, [this]() {
        m_requestsThisMinute = 0;
    });
    m_pRateLimitTimer->start();
}

SpotifyApiClient::~SpotifyApiClient() = default;

void SpotifyApiClient::ensureToken(std::function<void()> callback) {
    if (!m_accessToken.isEmpty()) {
        callback();
        return;
    }

    m_pendingCallbacks.append(callback);

    if (m_fetchingToken) {
        return;
    }

    m_fetchingToken = true;
    refreshToken();
}

void SpotifyApiClient::refreshToken() {
    QNetworkRequest request{QUrl(kTokenServerUrl)};
    QNetworkReply* reply = m_pNetwork->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onTokenReceived(reply);
    });
}

void SpotifyApiClient::onTokenReceived(QNetworkReply* reply) {
    reply->deleteLater();
    m_fetchingToken = false;

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "SpotifyApiClient: Token fetch failed:" << reply->errorString();
        emit error(QStringLiteral("Failed to get Spotify token: ") + reply->errorString());
        m_pendingCallbacks.clear();
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonObject obj = doc.object();
    m_accessToken = obj.value(QStringLiteral("accessToken")).toString();
    m_tokenExpiresIn = obj.value(QStringLiteral("expiresIn")).toInt(3600);

    if (m_accessToken.isEmpty()) {
        qWarning() << "SpotifyApiClient: Empty access token received";
        emit error(QStringLiteral("Empty Spotify access token"));
        m_pendingCallbacks.clear();
        return;
    }

    qDebug() << "SpotifyApiClient: Token received, expires in" << m_tokenExpiresIn << "s";

    // Schedule refresh at 80% of expiry time
    m_pTokenRefreshTimer->start(m_tokenExpiresIn * 800); // ms, 80%

    emit authenticated();

    // Execute pending callbacks
    auto callbacks = m_pendingCallbacks;
    m_pendingCallbacks.clear();
    for (const auto& cb : callbacks) {
        cb();
    }
}

QNetworkReply* SpotifyApiClient::makeRequest(
        const QString& endpoint,
        const QByteArray& method,
        const QJsonObject& body) {
    if (m_requestsThisMinute >= kMaxRequestsPerMinute) {
        qWarning() << "SpotifyApiClient: Rate limit reached, dropping request";
        return nullptr;
    }
    m_requestsThisMinute++;

    QUrl url(kSpotifyApiBase + endpoint);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", ("Bearer " + m_accessToken).toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    if (method == "GET") {
        return m_pNetwork->get(request);
    } else if (method == "PUT") {
        return m_pNetwork->put(request, QJsonDocument(body).toJson());
    } else if (method == "POST") {
        return m_pNetwork->post(request, QJsonDocument(body).toJson());
    }
    return nullptr;
}

void SpotifyApiClient::fetchPlaylists(int offset, int limit) {
    ensureToken([this, offset, limit]() {
        QString endpoint = QStringLiteral("/me/playlists?offset=%1&limit=%2")
                                   .arg(offset)
                                   .arg(limit);
        QNetworkReply* reply = makeRequest(endpoint);
        if (!reply) {
            return;
        }
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                emit error(QStringLiteral("Fetch playlists: ") + reply->errorString());
                return;
            }
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            emit playlistsReceived(doc.object().value(QStringLiteral("items")).toArray());
        });
    });
}

void SpotifyApiClient::fetchPlaylistTracks(const QString& playlistId, int offset, int limit) {
    Q_UNUSED(offset)
    Q_UNUSED(limit)
    ensureToken([this, playlistId]() {
        // Use /playlists/{id} instead of /playlists/{id}/tracks
        // because the /tracks sub-endpoint returns 403 in Spotify dev mode
        QString endpoint = QStringLiteral("/playlists/%1")
                                   .arg(playlistId);
        QNetworkReply* reply = makeRequest(endpoint);
        if (!reply) {
            return;
        }
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                emit error(QStringLiteral("Fetch tracks: ") + reply->errorString());
                return;
            }
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QJsonObject playlist = doc.object();
            QJsonArray items = playlist.value(QStringLiteral("tracks"))
                                       .toObject()
                                       .value(QStringLiteral("items"))
                                       .toArray();
            // Extract the track objects from the wrapper
            QJsonArray tracks;
            for (const auto& item : items) {
                QJsonObject trackObj = item.toObject().value(QStringLiteral("track")).toObject();
                if (!trackObj.isEmpty()) {
                    tracks.append(trackObj);
                }
            }
            emit tracksReceived(tracks);
        });
    });
}

void SpotifyApiClient::fetchSavedTracks(int offset, int limit) {
    ensureToken([this, offset, limit]() {
        QString endpoint = QStringLiteral("/me/tracks?offset=%1&limit=%2")
                                   .arg(offset)
                                   .arg(limit);
        QNetworkReply* reply = makeRequest(endpoint);
        if (!reply) {
            return;
        }
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                emit error(QStringLiteral("Fetch saved tracks: ") + reply->errorString());
                return;
            }
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QJsonArray items = doc.object().value(QStringLiteral("items")).toArray();
            QJsonArray tracks;
            for (const auto& item : items) {
                QJsonObject trackObj = item.toObject().value(QStringLiteral("track")).toObject();
                if (!trackObj.isEmpty()) {
                    tracks.append(trackObj);
                }
            }
            emit tracksReceived(tracks);
        });
    });
}

void SpotifyApiClient::searchTracks(const QString& query, int limit) {
    ensureToken([this, query, limit]() {
        QUrl url(kSpotifyApiBase + QStringLiteral("/search"));
        QUrlQuery params;
        params.addQueryItem(QStringLiteral("q"), query);
        params.addQueryItem(QStringLiteral("type"), QStringLiteral("track"));
        params.addQueryItem(QStringLiteral("limit"), QString::number(limit));
        url.setQuery(params);

        QNetworkRequest request(url);
        request.setRawHeader("Authorization", ("Bearer " + m_accessToken).toUtf8());

        m_requestsThisMinute++;
        QNetworkReply* reply = m_pNetwork->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                emit error(QStringLiteral("Search: ") + reply->errorString());
                return;
            }
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            emit tracksReceived(
                    doc.object()
                            .value(QStringLiteral("tracks"))
                            .toObject()
                            .value(QStringLiteral("items"))
                            .toArray());
        });
    });
}

void SpotifyApiClient::fetchAudioFeatures(const QStringList& trackIds) {
    ensureToken([this, trackIds]() {
        // Batch in groups of 100
        for (int i = 0; i < trackIds.size(); i += 100) {
            QStringList batch = trackIds.mid(i, 100);
            QString endpoint = QStringLiteral("/audio-features?ids=") + batch.join(QStringLiteral(","));
            QNetworkReply* reply = makeRequest(endpoint);
            if (!reply) {
                return;
            }
            connect(reply, &QNetworkReply::finished, this, [this, reply]() {
                reply->deleteLater();
                if (reply->error() != QNetworkReply::NoError) {
                    emit error(QStringLiteral("Audio features: ") + reply->errorString());
                    return;
                }
                QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
                emit audioFeaturesReceived(
                        doc.object().value(QStringLiteral("audio_features")).toArray());
            });
        }
    });
}

void SpotifyApiClient::play(const QString& trackUri, int positionMs, const QString& deviceId) {
    ensureToken([this, trackUri, positionMs, deviceId]() {
        QJsonObject body;
        QJsonArray uris;
        uris.append(trackUri);
        body[QStringLiteral("uris")] = uris;
        body[QStringLiteral("position_ms")] = positionMs;

        QString endpoint = QStringLiteral("/me/player/play");
        if (!deviceId.isEmpty()) {
            endpoint += QStringLiteral("?device_id=") + deviceId;
        }

        QNetworkReply* reply = makeRequest(endpoint, "PUT", body);
        if (!reply) {
            return;
        }
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                emit error(QStringLiteral("Play: ") + reply->errorString());
                return;
            }
            emit playbackStarted();
        });
    });
}

void SpotifyApiClient::seek(int positionMs) {
    ensureToken([this, positionMs]() {
        QString endpoint = QStringLiteral("/me/player/seek?position_ms=%1").arg(positionMs);
        QNetworkReply* reply = makeRequest(endpoint, "PUT");
        if (!reply) {
            return;
        }
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                emit error(QStringLiteral("Seek: ") + reply->errorString());
                return;
            }
            emit seekComplete();
        });
    });
}

void SpotifyApiClient::pause() {
    ensureToken([this]() {
        QNetworkReply* reply = makeRequest(QStringLiteral("/me/player/pause"), "PUT");
        if (!reply) {
            return;
        }
        connect(reply, &QNetworkReply::finished, this, [reply]() {
            reply->deleteLater();
        });
    });
}

void SpotifyApiClient::getDevices() {
    ensureToken([this]() {
        QNetworkReply* reply = makeRequest(QStringLiteral("/me/player/devices"));
        if (!reply) {
            return;
        }
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                emit error(QStringLiteral("Devices: ") + reply->errorString());
                return;
            }
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            emit devicesReceived(doc.object().value(QStringLiteral("devices")).toArray());
        });
    });
}

void SpotifyApiClient::transferPlayback(const QString& deviceId, bool play) {
    ensureToken([this, deviceId, play]() {
        QJsonObject body;
        QJsonArray ids;
        ids.append(deviceId);
        body[QStringLiteral("device_ids")] = ids;
        body[QStringLiteral("play")] = play;

        QNetworkReply* reply = makeRequest(QStringLiteral("/me/player"), "PUT", body);
        if (!reply) {
            return;
        }
        connect(reply, &QNetworkReply::finished, this, [reply]() {
            reply->deleteLater();
        });
    });
}

/*static*/ QString SpotifyApiClient::toCamelotKey(int key, int mode) {
    // Spotify API: key = pitch class (0-11), mode = 0(minor)/1(major)
    // Camelot wheel mapping
    static const int majorMap[] = {8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1};
    static const int minorMap[] = {5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10};

    if (key < 0 || key > 11) {
        return QString();
    }

    if (mode == 1) {
        // Major
        return QStringLiteral("%1B").arg(majorMap[key]);
    } else {
        // Minor
        return QStringLiteral("%1A").arg(minorMap[key]);
    }
}

} // namespace mixxx

#include "moc_spotifyapiclient.cpp"
