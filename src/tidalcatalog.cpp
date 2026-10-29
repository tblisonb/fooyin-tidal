/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "tidalcatalog.h"

#include "tidalapi.h"
#include "tidalsession.h"

#include <utils/stringutils.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QThread>

#include <map>

using namespace Qt::StringLiterals;

namespace {
constexpr auto ApiUrl = "https://openapi.tidal.com/v2";
// When TIDAL says to slow down (429): wait, then try again a few times.
constexpr int RateLimitRetries = 3;
constexpr int RateLimitWaitMs  = 2000;

using FooyinTidal::TidalItem;

//! A JSON:API document's `included` resources, by type and ID.
class Included
{
public:
    explicit Included(const QJsonObject& doc)
    {
        for(const auto& value : doc.value("included"_L1).toArray()) {
            const QJsonObject resource = value.toObject();
            m_resources.emplace(std::pair{resource.value("type"_L1).toString(), resource.value("id"_L1).toString()},
                                resource);
        }
    }

    [[nodiscard]] QJsonObject find(const QString& type, const QString& id) const
    {
        const auto it = m_resources.find({type, id});
        return it == m_resources.end() ? QJsonObject{} : it->second;
    }

    //! The resources a relationship of `resource` links to, as far as they're included.
    [[nodiscard]] QList<QJsonObject> related(const QJsonObject& resource, const QString& relationship) const
    {
        QList<QJsonObject> result;
        const QJsonArray links
            = resource.value("relationships"_L1).toObject().value(relationship).toObject().value("data"_L1).toArray();
        for(const auto& link : links) {
            const QJsonObject linkage = link.toObject();
            const QJsonObject found = find(linkage.value("type"_L1).toString(), linkage.value("id"_L1).toString());
            if(!found.isEmpty()) {
                result.push_back(found);
            }
        }
        return result;
    }

    //! A relationship's resources, in its order.
    [[nodiscard]] QList<QJsonObject> resolve(const QJsonArray& links) const
    {
        QList<QJsonObject> result;
        for(const auto& link : links) {
            const QJsonObject linkage = link.toObject();
            QJsonObject found = find(linkage.value("type"_L1).toString(), linkage.value("id"_L1).toString());
            if(!found.isEmpty()) {
                // Keep the linkage's meta (track and volume numbers in an album).
                if(linkage.contains("meta"_L1)) {
                    found["linkMeta"_L1] = linkage.value("meta"_L1);
                }
                result.push_back(found);
            }
        }
        return result;
    }

private:
    std::map<std::pair<QString, QString>, QJsonObject> m_resources;
};

QJsonObject attributes(const QJsonObject& resource)
{
    return resource.value("attributes"_L1).toObject();
}

QStringList names(const QList<QJsonObject>& artists)
{
    QStringList result;
    for(const auto& artist : artists) {
        result.push_back(attributes(artist).value("name"_L1).toString());
    }
    return result;
}

QString formatDuration(uint64_t ms)
{
    return Fooyin::Utils::msToString(ms);
}

TidalItem trackItem(const QJsonObject& resource, const Included& included)
{
    const QJsonObject attrs = attributes(resource);
    const QString id        = resource.value("id"_L1).toString();

    QString title = attrs.value("title"_L1).toString();
    if(const QString version = attrs.value("version"_L1).toString(); !version.isEmpty()) {
        title += u" (%1)"_s.arg(version);
    }
    const QStringList artists = names(included.related(resource, u"artists"_s));
    const auto albums         = included.related(resource, u"albums"_s);
    const QJsonObject album   = albums.isEmpty() ? QJsonObject{} : albums.front();
    const QJsonObject albumAttrs = attributes(album);
    QStringList albumArtists     = names(included.related(album, u"artists"_s));
    if(albumArtists.isEmpty() && !artists.isEmpty()) {
        albumArtists = {artists.front()};
    }
    const uint64_t duration = FooyinTidal::durationMs(attrs.value("duration"_L1).toString());

    Fooyin::Track track{FooyinTidal::trackPath(id)};
    track.setTitle(title);
    track.setArtists(artists);
    track.setAlbumArtists(albumArtists);
    track.setAlbum(albumAttrs.value("title"_L1).toString());
    track.setDate(albumAttrs.value("releaseDate"_L1).toString());
    const QJsonObject meta = resource.value("linkMeta"_L1).toObject();
    if(meta.contains("trackNumber"_L1)) {
        track.setTrackNumber(QString::number(meta.value("trackNumber"_L1).toInt()));
        track.setDiscNumber(QString::number(meta.value("volumeNumber"_L1).toInt(1)));
        if(const int volumes = albumAttrs.value("numberOfVolumes"_L1).toInt(); volumes > 0) {
            track.setDiscTotal(QString::number(volumes));
        }
    }
    if(const QString isrc = attrs.value("isrc"_L1).toString(); !isrc.isEmpty()) {
        track.replaceExtraTag(u"ISRC"_s, isrc);
    }
    track.setDuration(duration);
    track.setCodec(u"FLAC"_s);
    track.setMetadataWasRead(true);

    TidalItem item;
    item.kind   = TidalItem::Kind::Track;
    item.id     = id;
    item.title  = title;
    item.artist = artists.join(u", "_s);
    item.album  = track.album();
    item.info   = formatDuration(duration);
    item.track  = track;
    return item;
}

TidalItem albumItem(const QJsonObject& resource, const Included& included)
{
    const QJsonObject attrs = attributes(resource);

    TidalItem item;
    item.kind   = TidalItem::Kind::Album;
    item.id     = resource.value("id"_L1).toString();
    item.title  = attrs.value("title"_L1).toString();
    item.artist = names(included.related(resource, u"artists"_s)).join(u", "_s);

    QStringList info;
    if(const QString date = attrs.value("releaseDate"_L1).toString(); !date.isEmpty()) {
        info.push_back(date.left(4));
    }
    if(const QString type = attrs.value("type"_L1).toString(); !type.isEmpty() && type != "ALBUM"_L1) {
        info.push_back(type == "EP"_L1 ? type : type.left(1) + type.mid(1).toLower());
    }
    item.info = info.join(u" · "_s);
    return item;
}

TidalItem artistItem(const QJsonObject& resource)
{
    TidalItem item;
    item.kind  = TidalItem::Kind::Artist;
    item.id    = resource.value("id"_L1).toString();
    item.title = attributes(resource).value("name"_L1).toString();
    return item;
}

TidalItem playlistItem(const QJsonObject& resource)
{
    const QJsonObject attrs = attributes(resource);

    TidalItem item;
    item.kind  = TidalItem::Kind::Playlist;
    item.id    = resource.value("id"_L1).toString();
    item.title = attrs.value("name"_L1).toString();

    const QString type = attrs.value("playlistType"_L1).toString();
    item.artist        = type == "MIX"_L1 ? u"Mix"_s : type == "EDITORIAL"_L1 ? u"TIDAL"_s : QString{};
    if(attrs.contains("numberOfTrackItems"_L1)) {
        item.info = u"%1 tracks"_s.arg(attrs.value("numberOfTrackItems"_L1).toInt());
    }
    return item;
}

//! Items of the kinds the panel shows, in the order of `links`.
QList<TidalItem> itemsFrom(const QJsonArray& links, const Included& included)
{
    QList<TidalItem> items;
    for(const auto& resource : included.resolve(links)) {
        const QString type = resource.value("type"_L1).toString();
        if(type == "tracks"_L1) {
            items.push_back(trackItem(resource, included));
        }
        else if(type == "albums"_L1) {
            items.push_back(albumItem(resource, included));
        }
        else if(type == "artists"_L1) {
            items.push_back(artistItem(resource));
        }
        else if(type == "playlists"_L1) {
            items.push_back(playlistItem(resource));
        }
        // Videos are left out.
    }
    return items;
}

QString describeError(const FooyinTidal::HttpResult& result)
{
    if(result.error.isEmpty()) {
        const QJsonArray errors = QJsonDocument::fromJson(result.body).object().value("errors"_L1).toArray();
        if(!errors.isEmpty()) {
            const QJsonObject error = errors.first().toObject();
            return u"HTTP %1: %2"_s.arg(result.status).arg(error.value("detail"_L1).toString());
        }
    }
    return result.describe();
}
} // namespace

namespace FooyinTidal {
uint64_t durationMs(const QString& isoDuration)
{
    static const QRegularExpression pattern{uR"(^P(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+(?:\.\d+)?)S)?)?$)"_s};
    const QRegularExpressionMatch match = pattern.match(isoDuration);
    if(!match.hasMatch()) {
        return 0;
    }
    const double seconds = match.captured(1).toDouble() * 86400 + match.captured(2).toDouble() * 3600
                         + match.captured(3).toDouble() * 60 + match.captured(4).toDouble();
    return static_cast<uint64_t>(seconds * 1000);
}

TidalCatalog::TidalCatalog(std::shared_ptr<TidalSession> account)
    : m_account{std::move(account)}
{ }

std::expected<QJsonObject, QString> TidalCatalog::get(const QString& path, QUrlQuery query,
                                                      const BlockingHttp::Cancelled& cancelled)
{
    const auto token = m_account->accessToken(m_http);
    if(!token) {
        return std::unexpected(token.error());
    }
    query.addQueryItem(u"countryCode"_s, m_account->countryCode());

    QUrl url{QString::fromLatin1(ApiUrl) + path};
    url.setQuery(query);
    const BlockingHttp::Headers headers{{"Authorization", "Bearer " + token->toUtf8()},
                                        {"Accept", "application/vnd.api+json"}};

    for(int attempt{0};; ++attempt) {
        const HttpResult result = m_http.get(url, headers, cancelled);
        if(result.status == 429 && attempt < RateLimitRetries && !(cancelled && cancelled())) {
            QThread::msleep(RateLimitWaitMs * (attempt + 1));
            continue;
        }
        if(!result.ok()) {
            return std::unexpected(describeError(result));
        }
        return QJsonDocument::fromJson(result.body).object();
    }
}

std::expected<TidalSearch, QString> TidalCatalog::search(const QString& query)
{
    QUrlQuery params;
    params.addQueryItem(u"filter[query]"_s, QString::fromUtf8(QUrl::toPercentEncoding(query)));
    // Comma-separated: TIDAL honours only the first of repeated include parameters.
    params.addQueryItem(u"include"_s,
                        u"tracks,tracks.artists,tracks.albums,albums,albums.artists,artists,playlists"_s);

    const auto doc = get(u"/searchResults"_s, params);
    if(!doc) {
        return std::unexpected(doc.error());
    }

    const Included included{*doc};
    const QJsonArray data = doc->value("data"_L1).toArray();
    if(data.isEmpty()) {
        return TidalSearch{};
    }
    const QJsonObject relationships = data.first().toObject().value("relationships"_L1).toObject();
    const auto relation             = [&](const char* name) {
        return itemsFrom(relationships.value(QLatin1StringView{name}).toObject().value("data"_L1).toArray(), included);
    };

    return TidalSearch{.tracks    = relation("tracks"),
                       .albums    = relation("albums"),
                       .artists   = relation("artists"),
                       .playlists = relation("playlists")};
}

std::expected<TidalPage, QString> TidalCatalog::page(List list, const QString& id, const QString& cursor,
                                                     const BlockingHttp::Cancelled& cancelled)
{
    QString path;
    QString include;
    QString sort;
    const QString itemId = QString::fromUtf8(QUrl::toPercentEncoding(id));

    switch(list) {
        case List::FavoriteTracks:
            path    = u"/userCollectionTracks/me/relationships/items"_s;
            include = u"items,items.artists,items.albums"_s;
            sort    = u"-addedAt"_s;
            break;
        case List::FavoriteAlbums:
            path    = u"/userCollectionAlbums/me/relationships/items"_s;
            include = u"items,items.artists"_s;
            sort    = u"-addedAt"_s;
            break;
        case List::FavoriteArtists:
            path    = u"/userCollectionArtists/me/relationships/items"_s;
            include = u"items"_s;
            sort    = u"-addedAt"_s;
            break;
        case List::Playlists:
            path    = u"/userCollectionPlaylists/me/relationships/items"_s;
            include = u"items"_s;
            sort    = u"-lastModifiedAt"_s;
            break;
        case List::AlbumTracks:
            path    = u"/albums/%1/relationships/items"_s.arg(itemId);
            include = u"items,items.artists,items.albums"_s;
            break;
        case List::PlaylistTracks:
            path    = u"/playlists/%1/relationships/items"_s.arg(itemId);
            include = u"items,items.artists,items.albums"_s;
            break;
        case List::ArtistAlbums:
            path    = u"/artists/%1/relationships/albums"_s.arg(itemId);
            include = u"albums,albums.artists"_s;
            break;
    }

    QUrlQuery params;
    params.addQueryItem(u"include"_s, include);
    if(!sort.isEmpty()) {
        params.addQueryItem(u"sort"_s, sort);
    }
    if(!cursor.isEmpty()) {
        params.addQueryItem(u"page[cursor]"_s, QString::fromUtf8(QUrl::toPercentEncoding(cursor)));
    }

    const auto doc = get(path, params, cancelled);
    if(!doc) {
        return std::unexpected(doc.error());
    }

    TidalPage result;
    result.items = itemsFrom(doc->value("data"_L1).toArray(), Included{*doc});
    result.nextCursor
        = doc->value("links"_L1).toObject().value("meta"_L1).toObject().value("nextCursor"_L1).toString();
    return result;
}

std::expected<QList<TidalItem>, QString> TidalCatalog::all(List list, const QString& id,
                                                           const BlockingHttp::Cancelled& cancelled)
{
    QList<TidalItem> items;
    QString cursor;
    do {
        auto next = page(list, id, cursor, cancelled);
        if(!next) {
            return std::unexpected(next.error());
        }
        items.append(next->items);
        cursor = next->nextCursor;
    } while(!cursor.isEmpty() && !(cancelled && cancelled()));
    return items;
}
} // namespace FooyinTidal
