#include "lastfm_source.hpp"
#include "../gui/widgets/lastfm.hpp"
#include "../util/config.hpp"
#include "../util/utility.hpp"
#include "util/platform.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <curl/curl.h>

long lastfm_request(QJsonDocument& response_json, const QString& url);

lastfm_source::lastfm_source()
    : music_source(S_SOURCE_LAST_FM, T_SOURCE_LASTFM, new lastfm)
{
    m_capabilities = CAP_ALBUM | CAP_COVER | CAP_TITLE | CAP_ARTIST | CAP_DURATION;
}

void lastfm_source::load()
{
    m_username = utf8_to_qt(CGET_STR(CFG_LASTFM_USERNAME));
    m_api_key = utf8_to_qt(CGET_STR(CFG_LASTFM_API_KEY));
    if (m_api_key.isEmpty()) {
        m_custom_api_key = false;
        m_api_key = utf8_to_qt(LASTFM_CREDENTIALS);
    } else {
        m_custom_api_key = true;
    }
}

void lastfm_source::refresh()
{
    if (m_api_key.isEmpty()) {
        berr("No lastfm api key");
        return;
    }

    if (m_username.isEmpty())
        return;

    /* last.fm doesn't want apps to constantly send requets
     * to their API points
     * so this source uses slower refresh than the user might configure
     * in the gui if the shared api key is used
     */
    if (!m_custom_api_key && os_gettime_ns() < m_next_refresh)
        return;
    m_current.clear();
    QString track_request = "https://ws.audioscrobbler.com/2.0/?method=user.getrecenttracks&user="
        + m_username + "&api_key=" + m_api_key + "&limit=1&format=json";
    QJsonDocument response;
    auto code = lastfm_request(response, track_request);
    if (code == HTTP_OK) {
        auto recent_tracks = response["recenttracks"];

        if (recent_tracks.isObject() && recent_tracks["track"].isArray()) {
            auto track_arr = recent_tracks["track"].toArray();
            if (track_arr.size() > 0) {
                auto song = track_arr[0].toObject();
                if (!song.isEmpty())
                    parse_song(song);
            }
        }

        /* Since we don't know the progress of the song there's no way to
         * determine when the next request would be due, so a query every
         * five seconds should be slow enough, unless a custom api key is
         * used.
         */
        m_next_refresh = os_gettime_ns() + 5000000000;
    } else {
        berr("Received error code from last.fm request: %li", code);
        m_next_refresh = os_gettime_ns() + 1500000000;
    }
}

void lastfm_source::parse_song(const QJsonObject& s)
{
    if (s["@attr"].isObject()) {
        m_current.set_playing(s["@attr"]["nowplaying"].toString() == "true");

        if (m_current.playing()) {
            auto covers = s["image"];
            if (covers.isArray() && covers.toArray().size() > 0) {
                auto cover_array = covers.toArray();
                auto cover = cover_array[cover_array.size() - 1];
                if (cover.isObject()) {
                    m_current.set_cover_link(cover.toObject()["#text"].toString());
                }
            }
        }
        util::download_cover(m_current);
    }

    if (s["artist"].isObject())
        m_current.append_artist(s["artist"]["#text"].toString());

    if (s["album"].isObject())
        m_current.set_album(s["album"]["#text"].toString());

    if (s["name"].isString())
        m_current.set_title(s["name"].toString());

    if (!m_current.artists().empty() && !m_current.title().isEmpty()) {
        /* Try and get song duration */

        QString track_request = "https://ws.audioscrobbler.com/2.0/?method="
                                "track.getInfo&api_key="
            + m_api_key + "&artist=" + QUrl::toPercentEncoding(m_current.artists()[0]) + "&track=" + QUrl::toPercentEncoding(m_current.title()) + "&format=json";

        QJsonDocument response;
        auto code = lastfm_request(response, track_request);
        if (code == HTTP_OK) {
            auto track = response["track"];
            if (track.isObject()) {
                auto duration = track["duration"];
                if (duration.isString()) {
                    bool ok = false;
                    int i = duration.toString().toInt(&ok);
                    if (ok)
                        m_current.set_duration(i);
                }
            }
        }
    }
}

bool lastfm_source::execute_capability(capability)
{
    return true;
}

bool lastfm_source::valid_format(const QString& str)
{
    static QRegularExpression reg("/%[p|P]|%[r|R]|%[b|B]|%[y|Y]|%[d|D]|%[n|N]/gm");
    return !reg.match(str).hasMatch();
}

bool lastfm_source::enabled() const
{
    return true;
}

/* === cURL stuff == */

long lastfm_request(QJsonDocument& response_json, const QString& url)
{
    CURL* curl = curl_easy_init();
    std::string response;
    long http_code = -1;
    //curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, qt_to_utf8(url));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, util::write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
#ifdef DEBUG
    curl_easy_setopt(curl, CURLOPT_VERBOSE, CURL_DEBUG);
#endif
    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        QJsonParseError err;
        response_json = QJsonDocument::fromJson(response.c_str(), &err);
        if (response_json.isNull() && !response.empty())
            berr("Failed to parse json response: %s, Error: %s", response.c_str(), qt_to_utf8(err.errorString()));
    } else {
        berr("CURL failed while sending spotify command");
    }

    curl_easy_cleanup(curl);
    return http_code;
}
