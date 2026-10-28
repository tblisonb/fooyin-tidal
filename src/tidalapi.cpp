/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "tidalapi.h"

#include "tidalsession.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {
constexpr auto ApiV1         = "https://api.tidal.com/v1/";
constexpr auto Scheme        = "tidal";
constexpr auto StreamQuality = "HI_RES_LOSSLESS";

// TIDAL's DASH manifests for audio are one representation with a SegmentTemplate: an
// initialization segment, then numbered media segments listed by a SegmentTimeline.
std::expected<FooyinTidal::TidalStream, QString> parseDash(const QByteArray& manifest)
{
    FooyinTidal::TidalStream stream;
    QString initialization;
    QString media;
    int startNumber{1};
    uint64_t timescale{1};
    // Segment start times in the manifest's timescale, from its SegmentTimeline.
    QList<uint64_t> starts;
    uint64_t nextStart{0};

    QXmlStreamReader xml{manifest};
    while(!xml.atEnd()) {
        if(xml.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }
        const auto name  = xml.name();
        const auto attrs = xml.attributes();
        if(name == "ContentProtection"_L1) {
            return std::unexpected(u"TIDAL sent an encrypted stream"_s);
        }
        if(name == "Representation"_L1) {
            stream.codec = attrs.value("codecs"_L1).toString();
        }
        else if(name == "SegmentTemplate"_L1) {
            initialization = attrs.value("initialization"_L1).toString();
            media          = attrs.value("media"_L1).toString();
            if(attrs.hasAttribute("startNumber"_L1)) {
                startNumber = attrs.value("startNumber"_L1).toInt();
            }
            timescale = std::max<uint64_t>(1, attrs.value("timescale"_L1).toULongLong());
        }
        else if(name == "S"_L1) {
            // <S t="start" d="duration" r="repeats after the first"/>; t is optional after the first.
            if(attrs.hasAttribute("t"_L1)) {
                nextStart = attrs.value("t"_L1).toULongLong();
            }
            const uint64_t duration = attrs.value("d"_L1).toULongLong();
            const int repeats       = attrs.value("r"_L1).toInt();
            for(int i{0}; i <= repeats; ++i) {
                starts.append(nextStart);
                nextStart += duration;
            }
        }
    }

    if(xml.hasError()) {
        return std::unexpected(u"Couldn't read TIDAL's stream manifest: %1"_s.arg(xml.errorString()));
    }
    if(initialization.isEmpty() || !media.contains("$Number$"_L1) || starts.isEmpty()) {
        return std::unexpected(u"TIDAL's stream manifest isn't in a form this plugin knows"_s);
    }

    stream.urls.append(initialization);
    for(qsizetype i{0}; i < starts.size(); ++i) {
        stream.urls.append(QString{media}.replace("$Number$"_L1, QString::number(startNumber + i)));
        stream.segmentStartsMs.append(starts.at(i) * 1000 / timescale);
    }
    return stream;
}

// The other manifest type: a JSON list of URLs to plain files.
std::expected<FooyinTidal::TidalStream, QString> parseBts(const QByteArray& manifest)
{
    const QJsonObject json = QJsonDocument::fromJson(manifest).object();
    const QString encryption = json.value("encryptionType"_L1).toString(u"NONE"_s);
    if(encryption != "NONE"_L1) {
        return std::unexpected(u"TIDAL sent an encrypted stream (%1)"_s.arg(encryption));
    }

    FooyinTidal::TidalStream stream;
    stream.codec = json.value("codecs"_L1).toString();
    for(const auto& url : json.value("urls"_L1).toArray()) {
        stream.urls.append(url.toString());
    }
    if(stream.urls.isEmpty()) {
        return std::unexpected(u"TIDAL's stream manifest has no URLs"_s);
    }
    // Several URLs are mirrors of the same file, not parts of it.
    stream.urls = {stream.urls.constFirst()};
    return stream;
}
} // namespace

namespace FooyinTidal {
qsizetype TidalStream::segmentAt(uint64_t ms) const
{
    const auto it = std::upper_bound(segmentStartsMs.cbegin(), segmentStartsMs.cend(), ms);
    return std::max<qsizetype>(0, std::distance(segmentStartsMs.cbegin(), it) - 1);
}

QStringList TidalStream::urlsFrom(qsizetype segment) const
{
    if(segmentStartsMs.isEmpty() || segment <= 0) {
        return urls;
    }
    return QStringList{urls.constFirst()} + urls.mid(1 + segment);
}

QString trackPath(const QString& trackId)
{
    return u"%1://track/%2"_s.arg(QLatin1StringView{Scheme}, trackId);
}

QString trackIdFrom(const QString& pathOrLink)
{
    static const QRegularExpression bareId{u"^\\d+$"_s};
    static const QRegularExpression inLink{u"(?:^tidal://|tidal\\.com/)(?:browse/)?track/(\\d+)"_s};

    const QString text = pathOrLink.trimmed();
    if(bareId.match(text).hasMatch()) {
        return text;
    }
    return inLink.match(text).captured(1);
}

TidalApi::TidalApi(std::shared_ptr<TidalSession> session)
    : m_session{std::move(session)}
{ }

std::expected<QByteArray, QString> TidalApi::getV1(const QString& path, const QUrlQuery& query,
                                                   const BlockingHttp::Cancelled& cancelled)
{
    const auto token = m_session->accessToken(m_http);
    if(!token) {
        return std::unexpected(token.error());
    }

    QUrl url{QLatin1StringView{ApiV1} + path};
    QUrlQuery fullQuery{query};
    fullQuery.addQueryItem(u"countryCode"_s, m_session->countryCode());
    url.setQuery(fullQuery);

    const HttpResult result = m_http.get(url, {{"Authorization", "Bearer " + token->toUtf8()}}, cancelled);
    if(!result.ok()) {
        return std::unexpected(result.describe());
    }
    return result.body;
}

std::expected<Fooyin::Track, QString> TidalApi::track(const QString& trackId)
{
    const auto body = getV1(u"tracks/%1"_s.arg(trackId), {});
    if(!body) {
        return std::unexpected(body.error());
    }

    const QJsonObject json = QJsonDocument::fromJson(*body).object();

    QString title = json.value("title"_L1).toString();
    if(const QString version = json.value("version"_L1).toString(); !version.isEmpty()) {
        title += u" (%1)"_s.arg(version);
    }

    QStringList artists;
    for(const auto& artist : json.value("artists"_L1).toArray()) {
        artists.append(artist.toObject().value("name"_L1).toString());
    }
    const QString mainArtist = json.value("artist"_L1).toObject().value("name"_L1).toString();
    if(artists.isEmpty() && !mainArtist.isEmpty()) {
        artists.append(mainArtist);
    }

    Fooyin::Track track{trackPath(trackId)};
    track.setTitle(title);
    track.setArtists(artists);
    track.setAlbumArtists(mainArtist.isEmpty() ? QStringList{} : QStringList{mainArtist});
    track.setAlbum(json.value("album"_L1).toObject().value("title"_L1).toString());
    track.setTrackNumber(QString::number(json.value("trackNumber"_L1).toInt()));
    track.setDiscNumber(QString::number(json.value("volumeNumber"_L1).toInt(1)));
    track.setDuration(static_cast<uint64_t>(json.value("duration"_L1).toInteger()) * 1000);
    track.setCodec(u"FLAC"_s);
    track.setMetadataWasRead(true);
    return track;
}

std::expected<TidalStream, QString> TidalApi::stream(const QString& trackId, const BlockingHttp::Cancelled& cancelled)
{
    QUrlQuery query;
    query.addQueryItem(u"audioquality"_s, QString::fromLatin1(StreamQuality));
    query.addQueryItem(u"playbackmode"_s, u"STREAM"_s);
    query.addQueryItem(u"assetpresentation"_s, u"FULL"_s);

    const auto body = getV1(u"tracks/%1/playbackinfo"_s.arg(trackId), query, cancelled);
    if(!body) {
        return std::unexpected(body.error());
    }

    const QJsonObject json = QJsonDocument::fromJson(*body).object();
    if(json.value("assetPresentation"_L1).toString() != "FULL"_L1) {
        return std::unexpected(u"TIDAL offered only a preview of this track"_s);
    }

    const QString mimeType   = json.value("manifestMimeType"_L1).toString();
    const QByteArray manifest = QByteArray::fromBase64(json.value("manifest"_L1).toString().toLatin1());

    std::expected<TidalStream, QString> stream;
    if(mimeType == "application/dash+xml"_L1) {
        stream = parseDash(manifest);
    }
    else if(mimeType == "application/vnd.tidal.bts"_L1) {
        stream = parseBts(manifest);
    }
    else {
        return std::unexpected(u"TIDAL sent a stream manifest of an unknown type (%1)"_s.arg(mimeType));
    }

    if(stream) {
        stream->audioQuality = json.value("audioQuality"_L1).toString();
        stream->bitDepth     = json.value("bitDepth"_L1).toInt();
        stream->sampleRate   = json.value("sampleRate"_L1).toInt();
    }
    return stream;
}
} // namespace FooyinTidal
