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

private:
    std::expected<QJsonObject, QString> get(const QString& path, QUrlQuery query,
                                            const BlockingHttp::Cancelled& cancelled = {});

    std::shared_ptr<TidalSession> m_account;
    BlockingHttp m_http;
};

//! Milliseconds in an ISO 8601 duration such as PT3M59S.
uint64_t durationMs(const QString& isoDuration);
} // namespace FooyinTidal
