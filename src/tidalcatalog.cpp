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

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QThread>
#include <QUuid>

#include <map>

using namespace Qt::StringLiterals;

namespace {
constexpr auto ApiUrl = "https://openapi.tidal.com/v2";
// When TIDAL says to slow down (429): wait, then try again a few times.
constexpr int RateLimitRetries = 3;
constexpr int RateLimitWaitMs  = 2000;
constexpr qsizetype MaxItemsPerChange = 50;

QString encoded(const QString& id)
{
    return QString::fromUtf8(QUrl::toPercentEncoding(id));
}

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

QDateTime dateTime(const QJsonValue& value)
{
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}

//! When the resource was added to the list it came from (the linkage's meta), if TIDAL says.
QDateTime addedAt(const QJsonObject& resource)
{
    return dateTime(resource.value("linkMeta"_L1).toObject().value("addedAt"_L1));
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
    // fooyin keys full-size covers (and more) by the hash; without one every TIDAL track shares a key.
    track.generateHash();

    TidalItem item;
    item.kind   = TidalItem::Kind::Track;
    item.id     = id;
    item.title  = title;
    item.artist = artists.join(u", "_s);
    item.album     = track.album();
    item.info      = formatDuration(duration);
    item.infoOrder = static_cast<qint64>(duration);
    item.added     = addedAt(resource);
    item.track     = track;
    item.playlistItemId = resource.value("linkMeta"_L1).toObject().value("itemId"_L1).toString();
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
    item.added  = addedAt(resource);

    QStringList info;
    if(const QString date = attrs.value("releaseDate"_L1).toString(); !date.isEmpty()) {
        info.push_back(date.left(4));
        item.infoOrder = date.left(4).toLongLong();
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
    item.added = addedAt(resource);
    return item;
}

TidalItem playlistItem(const QJsonObject& resource)
{
    const QJsonObject attrs = attributes(resource);

    TidalItem item;
    item.kind  = TidalItem::Kind::Playlist;
    item.id    = resource.value("id"_L1).toString();
    item.title    = attrs.value("name"_L1).toString();
    item.added    = addedAt(resource);
    item.modified = dateTime(attrs.value("lastModifiedAt"_L1));

    for(const auto& owner :
        resource.value("relationships"_L1).toObject().value("owners"_L1).toObject().value("data"_L1).toArray()) {
        item.owners.append(owner.toObject().value("id"_L1).toString());
    }

    const QString type = attrs.value("playlistType"_L1).toString();
    item.artist        = type == "MIX"_L1 ? u"Mix"_s : type == "EDITORIAL"_L1 ? u"TIDAL"_s : QString{};
    if(attrs.contains("numberOfTrackItems"_L1)) {
        item.infoOrder = attrs.value("numberOfTrackItems"_L1).toInt();
        item.info      = u"%1 tracks"_s.arg(item.infoOrder);
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
    return request("GET", path, std::move(query), {}, cancelled);
}

std::expected<QJsonObject, QString> TidalCatalog::request(const QByteArray& method, const QString& path,
                                                          QUrlQuery query, const QJsonObject& body,
                                                          const BlockingHttp::Cancelled& cancelled)
{
    const auto token = m_account->accessToken(m_http);
    if(!token) {
        return std::unexpected(token.error());
    }
    query.addQueryItem(u"countryCode"_s, m_account->countryCode());

    QUrl url{QString::fromLatin1(ApiUrl) + path};
    url.setQuery(query);
    BlockingHttp::Headers headers{{"Authorization", "Bearer " + token->toUtf8()},
                                  {"Accept", "application/vnd.api+json"}};
    QByteArray payload;
    if(method != "GET") {
        payload = QJsonDocument{body}.toJson(QJsonDocument::Compact);
        headers.append(std::pair<QByteArray, QByteArray>{"Content-Type", "application/vnd.api+json"});
        // The same key on every retry, so a change TIDAL already made isn't made twice.
        headers.append(std::pair<QByteArray, QByteArray>{"Idempotency-Key", QUuid::createUuid().toByteArray(QUuid::WithoutBraces)});
    }

    for(int attempt{0};; ++attempt) {
        const HttpResult result = method == "GET" ? m_http.get(url, headers, cancelled)
                                                  : m_http.send(method, url, headers, payload, cancelled);
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

std::expected<void, QString> TidalCatalog::changeItems(const QByteArray& method, const QString& path,
                                                       const QJsonArray& data)
{
    // TIDAL takes at most 50 items a request.
    for(qsizetype first{0}; first < data.size(); first += MaxItemsPerChange) {
        QJsonArray chunk;
        for(qsizetype i{first}; i < std::min(data.size(), first + MaxItemsPerChange); ++i) {
            chunk.append(data.at(i));
        }
        if(const auto result = request(method, path, {}, QJsonObject{{u"data"_s, chunk}}); !result) {
            return std::unexpected(result.error());
        }
    }
    return {};
}

std::expected<void, QString> TidalCatalog::addToPlaylist(const QString& playlistId, const QStringList& trackIds)
{
    QJsonArray data;
    for(const QString& id : trackIds) {
        data.append(QJsonObject{{u"id"_s, id}, {u"type"_s, u"tracks"_s}});
    }
    return changeItems("POST", u"/playlists/%1/relationships/items"_s.arg(encoded(playlistId)), data);
}

std::expected<void, QString> TidalCatalog::removeFromPlaylist(const QString& playlistId,
                                                              const QList<TidalItem>& entries)
{
    QJsonArray data;
    for(const TidalItem& entry : entries) {
        if(entry.playlistItemId.isEmpty()) {
            return std::unexpected(u"TIDAL didn't give this playlist's entries IDs; reload it and try again"_s);
        }
        data.append(QJsonObject{{u"id"_s, entry.id},
                                {u"type"_s, u"tracks"_s},
                                {u"meta"_s, QJsonObject{{u"itemId"_s, entry.playlistItemId}}}});
    }
    return changeItems("DELETE", u"/playlists/%1/relationships/items"_s.arg(encoded(playlistId)), data);
}

std::expected<void, QString> TidalCatalog::setFavorite(TidalItem::Kind kind, const QStringList& ids, bool favorite)
{
    QString collection;
    QString type;
    switch(kind) {
        case TidalItem::Kind::Track:
            collection = u"userCollectionTracks"_s;
            type       = u"tracks"_s;
            break;
        case TidalItem::Kind::Album:
            collection = u"userCollectionAlbums"_s;
            type       = u"albums"_s;
            break;
        case TidalItem::Kind::Artist:
            collection = u"userCollectionArtists"_s;
            type       = u"artists"_s;
            break;
        case TidalItem::Kind::Playlist:
            collection = u"userCollectionPlaylists"_s;
            type       = u"playlists"_s;
            break;
    }
    QJsonArray data;
    for(const QString& id : ids) {
        data.append(QJsonObject{{u"id"_s, id}, {u"type"_s, type}});
    }
    return changeItems(favorite ? "POST" : "DELETE", u"/%1/me/relationships/items"_s.arg(collection), data);
}

std::expected<QString, QString> TidalCatalog::createPlaylist(const QString& name)
{
    const QJsonObject body{
        {u"data"_s,
         QJsonObject{{u"type"_s, u"playlists"_s},
                     {u"attributes"_s, QJsonObject{{u"name"_s, name}, {u"accessType"_s, u"UNLISTED"_s}}}}}};
    const auto document = request("POST", u"/playlists"_s, {}, body);
    if(!document) {
        return std::unexpected(document.error());
    }
    const QString id = document->value("data"_L1).toObject().value("id"_L1).toString();
    if(id.isEmpty()) {
        return std::unexpected(u"TIDAL didn't return the new playlist"_s);
    }
    return id;
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
    const auto doc = pageDocument(list, id, cursor, cancelled);
    if(!doc) {
        return std::unexpected(doc.error());
    }
    return parsePage(*doc);
}

TidalPage TidalCatalog::parsePage(const QJsonObject& document)
{
    TidalPage result;
    result.items = itemsFrom(document.value("data"_L1).toArray(), Included{document});
    result.nextCursor
        = document.value("links"_L1).toObject().value("meta"_L1).toObject().value("nextCursor"_L1).toString();
    return result;
}

std::expected<QJsonObject, QString> TidalCatalog::pageDocument(List list, const QString& id, const QString& cursor,
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
            // With owners: only your own playlists can be added to.
            include = u"items,items.owners"_s;
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

    return get(path, params, cancelled);
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

std::expected<QList<QJsonObject>, QString> TidalCatalog::allDocuments(List list, const QString& id,
                                                                      const BlockingHttp::Cancelled& cancelled)
{
    QList<QJsonObject> documents;
    QString cursor;
    do {
        auto next = pageDocument(list, id, cursor, cancelled);
        if(!next) {
            return std::unexpected(next.error());
        }
        cursor = parsePage(*next).nextCursor;
        documents.push_back(std::move(*next));
    } while(!cursor.isEmpty() && !(cancelled && cancelled()));
    if(cancelled && cancelled()) {
        return std::unexpected(u"Cancelled"_s);
    }
    return documents;
}

bool TidalCatalog::hasVersion(List list)
{
    // Albums don't change, and an artist's albums can't be checked without fetching them.
    return list != List::AlbumTracks && list != List::ArtistAlbums;
}

std::expected<QString, QString> TidalCatalog::version(List list, const QString& id,
                                                      const BlockingHttp::Cancelled& cancelled)
{
    QString path;
    switch(list) {
        case List::FavoriteTracks:
            path = u"/userCollectionTracks/me"_s;
            break;
        case List::FavoriteAlbums:
            path = u"/userCollectionAlbums/me"_s;
            break;
        case List::FavoriteArtists:
            path = u"/userCollectionArtists/me"_s;
            break;
        case List::Playlists:
            path = u"/userCollectionPlaylists/me"_s;
            break;
        case List::PlaylistTracks:
            path = u"/playlists/%1"_s.arg(QString::fromUtf8(QUrl::toPercentEncoding(id)));
            break;
        case List::AlbumTracks:
        case List::ArtistAlbums:
            return QString{};
    }

    const auto doc = get(path, {}, cancelled);
    if(!doc) {
        return std::unexpected(doc.error());
    }
    const QJsonObject attrs = attributes(doc->value("data"_L1).toObject());
    QString version         = u"%1/%2"_s.arg(attrs.value("lastModifiedAt"_L1).toString())
                          .arg(attrs.value("numberOfItems"_L1).toInteger());

    if(list == List::Playlists) {
        // The list shows each playlist's track count, and editing a playlist moves it to the top
        // (newest change first): the first page's changes count too.
        const auto first = page(list, {}, {}, cancelled);
        if(!first) {
            return std::unexpected(first.error());
        }
        QCryptographicHash changes{QCryptographicHash::Sha1};
        for(const TidalItem& item : first->items) {
            changes.addData(u"%1@%2/%3;"_s.arg(item.id, item.modified.toString(Qt::ISODateWithMs))
                                .arg(item.infoOrder)
                                .toUtf8());
        }
        version += u";"_s + QString::fromLatin1(changes.result().toHex());
    }
    return version;
}
} // namespace FooyinTidal
