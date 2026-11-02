/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include "tidalcatalog.h"

#include <core/track.h>

#include <QObject>

#include <functional>
#include <memory>
#include <optional>

class QMenu;

namespace FooyinTidal {
class TidalCache;
class TidalSession;

/*!
 * Changes to your TIDAL playlists and favorites, from the panel and from fooyin's own menus. Runs
 * them in the background, says how they went, and tells the panels which lists changed. Tracks
 * that aren't TIDAL's are looked up on TIDAL first (`TidalCatalog::findTrack`).
 */
class TidalEditor : public QObject
{
    Q_OBJECT

public:
    //! A list an edit changes, so the panels and the cache catch up with TIDAL.
    struct Changed
    {
        TidalCatalog::List list;
        QString id;
        //! Taken out of the list at once, before TIDAL is asked for it again: its items' IDs, or
        //! for a playlist, its entries' IDs.
        QStringList removed;
    };
    //! Runs on a worker thread; returns what to say when it's done.
    using Edit = std::function<std::expected<QString, QString>(TidalCatalog& catalog)>;

    TidalEditor(std::shared_ptr<TidalSession> account, std::shared_ptr<TidalCache> cache,
                QObject* parent = nullptr);
    ~TidalEditor() override;

    [[nodiscard]] bool isLoggedIn() const;

    //! "Add to TIDAL Playlist" and "Add to/Remove from TIDAL Favorites" for fooyin tracks.
    void addTrackActions(QMenu* menu, const Fooyin::TrackList& tracks, const QString& playlistMenuTitle = {});
    //! The playlist menu's entries: New Playlist… and your own playlists.
    void addPlaylistEntries(QMenu* menu, const Fooyin::TrackList& tracks);
    //! Favorites actions for albums, artists and playlists (and tracks) as the panel lists them.
    void addFavoriteActions(QMenu* menu, const QList<TidalItem>& items);
    //! Adds fooyin tracks to your favorites (for the Playback menu).
    void addFavorite(const Fooyin::TrackList& tracks);
    void removeFromPlaylist(const QString& playlistId, const QString& playlistTitle, const QList<TidalItem>& entries);

    [[nodiscard]] QList<TidalItem> ownPlaylists();
    [[nodiscard]] bool isOwnPlaylist(const TidalItem& playlist);

Q_SIGNALS:
    //! "Saving to TIDAL…", then how it went.
    void notice(const QString& message);
    void changed(const QList<FooyinTidal::TidalEditor::Changed>& lists);

private:
    void edit(const Edit& change, const QString& failed, const QList<Changed>& changed);
    void addToNewPlaylist(const Fooyin::TrackList& tracks);
    void setTrackFavorite(const Fooyin::TrackList& tracks, bool favorite);
    //! Fetches a list into the cache in the background if it isn't there (for the menus).
    void prefetch(TidalCatalog::List list);
    //! Whether each item is a favorite: nothing when that favorites list isn't cached.
    [[nodiscard]] std::optional<bool> allFavorite(const QStringList& ids, TidalCatalog::List list, bool favorite);

    std::shared_ptr<TidalSession> m_account;
    std::shared_ptr<TidalCache> m_cache;
    QList<TidalCatalog::List> m_prefetching;
};
} // namespace FooyinTidal
