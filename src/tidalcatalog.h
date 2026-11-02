/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include "blockinghttp.h"

#include <core/track.h>

#include <QDateTime>
#include <QJsonObject>

#include <expected>
#include <memory>

namespace FooyinTidal {
class TidalSession;

//! A track, album, artist or playlist as the panel lists it.
struct TidalItem
{
    enum class Kind : uint8_t
    {
        Track,
        Album,
        Artist,
        Playlist,
    };

    Kind kind{Kind::Track};
    QString id;
    QString title;
    //! Artists of a track or album; a playlist's type.
    QString artist;
    //! A track's album.
    QString album;
    //! A track's length, an album's year and type, a playlist's track count.
    QString info;
    //! What `info` sorts by: a track's length in ms, an album's year, a playlist's track count.
    qint64 infoOrder{0};
    //! When it was added to the favorites or playlist it was listed from, if TIDAL says.
    QDateTime added;
    //! A playlist's last change.
    QDateTime modified;
    //! For a track: the fooyin track that plays it.
    Fooyin::Track track;
};

//! One page of a list; more follow while nextCursor isn't empty.
struct TidalPage
{
    QList<TidalItem> items;
    QString nextCursor;
};

struct TidalSearch
{
    QList<TidalItem> tracks;
    QList<TidalItem> albums;
    QList<TidalItem> artists;
    QList<TidalItem> playlists;
};

/*!
 * Calls to TIDAL's official API (openapi.tidal.com/v2) with the account login. Blocking: use it on
 * a worker thread, or from a QtConcurrent task. One instance per thread.
 */
class TidalCatalog
{
public:
    using Cancelled = BlockingHttp::Cancelled;

    enum class List : uint8_t
    {
        FavoriteTracks,
        FavoriteAlbums,
        FavoriteArtists,
        Playlists,
        //! The rest take the album's, playlist's or artist's ID.
        AlbumTracks,
        PlaylistTracks,
        ArtistAlbums,
    };

    explicit TidalCatalog(std::shared_ptr<TidalSession> account);

    std::expected<TidalSearch, QString> search(const QString& query);
    std::expected<TidalPage, QString> page(List list, const QString& id = {}, const QString& cursor = {},
                                           const BlockingHttp::Cancelled& cancelled = {});
    //! Every page of a list, one after another.
    std::expected<QList<TidalItem>, QString> all(List list, const QString& id = {},
                                                 const BlockingHttp::Cancelled& cancelled = {});

    //! One page as TIDAL sent it (what the cache keeps); `parsePage` makes it a `TidalPage`.
    std::expected<QJsonObject, QString> pageDocument(List list, const QString& id = {}, const QString& cursor = {},
                                                     const BlockingHttp::Cancelled& cancelled = {});
    //! Every page of a list as TIDAL sent them.
    std::expected<QList<QJsonObject>, QString> allDocuments(List list, const QString& id = {},
                                                            const BlockingHttp::Cancelled& cancelled = {});
    static TidalPage parsePage(const QJsonObject& document);

    //! Whether a list's contents can be checked for changes with `version()`.
    static bool hasVersion(List list);
    /*!
     * A short request that tells whether a list has changed: the same string as before means the
     * same contents. From the list's last change and item count.
     */
    std::expected<QString, QString> version(List list, const QString& id = {},
                                            const BlockingHttp::Cancelled& cancelled = {});

private:
    std::expected<QJsonObject, QString> get(const QString& path, QUrlQuery query,
                                            const BlockingHttp::Cancelled& cancelled = {});

    std::shared_ptr<TidalSession> m_account;
    BlockingHttp m_http;
};

//! Milliseconds in an ISO 8601 duration such as PT3M59S.
uint64_t durationMs(const QString& isoDuration);
} // namespace FooyinTidal
