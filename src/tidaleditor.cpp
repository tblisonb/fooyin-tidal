/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "tidaleditor.h"

#include "tidalapi.h"
#include "tidalcache.h"
#include "tidalsession.h"

#include <gui/statusevent.h>
#include <utils/utils.h>

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QMenu>
#include <QSet>
#include <QtConcurrentRun>

#include <algorithm>

Q_DECLARE_LOGGING_CATEGORY(TIDAL)

using namespace Qt::StringLiterals;

namespace {
using FooyinTidal::TidalCatalog;
using FooyinTidal::TidalItem;

// How long fooyin's status bar shows an edit's outcome.
constexpr int StatusTimeoutMs = 8000;

QString tr(const char* text, int n = -1)
{
    return QCoreApplication::translate("FooyinTidal::TidalEditor", text, nullptr, n);
}

//! The favorites list items of this kind are kept in.
TidalCatalog::List favoritesList(TidalItem::Kind kind)
{
    switch(kind) {
        case TidalItem::Kind::Album:
            return TidalCatalog::List::FavoriteAlbums;
        case TidalItem::Kind::Artist:
            return TidalCatalog::List::FavoriteArtists;
        case TidalItem::Kind::Playlist:
            return TidalCatalog::List::Playlists;
        case TidalItem::Kind::Track:
            break;
    }
    return TidalCatalog::List::FavoriteTracks;
}

QString quoted(const QString& title)
{
    return u"“%1”"_s.arg(title);
}

//! “Title” for one item, "3 tracks" for more.
QString describe(const QList<TidalItem>& items)
{
    if(items.size() == 1) {
        return quoted(items.front().title);
    }
    const bool tracks
        = std::ranges::all_of(items, [](const TidalItem& item) { return item.kind == TidalItem::Kind::Track; });
    return tracks ? tr("%n track(s)", static_cast<int>(items.size()))
                  : tr("%n item(s)", static_cast<int>(items.size()));
}

QString describe(const Fooyin::TrackList& tracks)
{
    if(tracks.size() == 1) {
        return quoted(tracks.front().effectiveTitle());
    }
    return tr("%n track(s)", static_cast<int>(tracks.size()));
}

//! A playlist name as a menu entry, where & would mark a shortcut.
QString menuText(QString text)
{
    return text.replace(u'&', u"&&"_s);
}

bool isTidalTrack(const Fooyin::Track& track)
{
    return track.filepath().startsWith(u"tidal://"_s);
}

//! The TIDAL tracks for fooyin tracks, and the ones TIDAL doesn't have.
struct Found
{
    QStringList ids;
    QStringList missing;
};

std::expected<Found, QString> findOnTidal(TidalCatalog& catalog, const Fooyin::TrackList& tracks)
{
    Found found;
    for(const Fooyin::Track& track : tracks) {
        const auto id = catalog.findTrack(track);
        if(!id) {
            return std::unexpected(id.error());
        }
        if(id->isEmpty()) {
            found.missing.append(quoted(track.effectiveTitle()));
        }
        else {
            found.ids.append(*id);
        }
    }
    if(found.ids.isEmpty()) {
        return std::unexpected(found.missing.size() == 1 ? tr("%1 isn't on TIDAL").arg(found.missing.front())
                                                         : tr("none of those tracks are on TIDAL"));
    }
    return found;
}

//! What an edit did, and which tracks it left out because TIDAL hasn't got them.
QString outcome(const QString& done, const Found& found)
{
    if(found.missing.isEmpty()) {
        return done;
    }
    return tr("%1 (not on TIDAL: %2)").arg(done, found.missing.join(u", "_s));
}

std::expected<QString, QString> fetchVersion(TidalCatalog& catalog, TidalCatalog::List list)
{
    if(!TidalCatalog::hasVersion(list)) {
        return QString{};
    }
    return catalog.version(list);
}
} // namespace

namespace FooyinTidal {
TidalEditor::TidalEditor(std::shared_ptr<TidalSession> account, std::shared_ptr<TidalCache> cache, QObject* parent)
    : QObject{parent}
    , m_account{std::move(account)}
    , m_cache{std::move(cache)}
{ }

TidalEditor::~TidalEditor() = default;

bool TidalEditor::isLoggedIn() const
{
    return m_account->isLoggedIn();
}

void TidalEditor::addTrackActions(QMenu* menu, const Fooyin::TrackList& tracks, const QString& playlistMenuTitle)
{
    if(tracks.empty() || !isLoggedIn()) {
        return;
    }

    auto* playlists = menu->addMenu(playlistMenuTitle.isEmpty() ? tr("Add to TIDAL Pla&ylist") : playlistMenuTitle);
    addPlaylistEntries(playlists, tracks);

    // TIDAL tracks can be checked against your favorites; others are looked up when it's done.
    const bool allTidal = std::ranges::all_of(tracks, isTidalTrack);
    std::optional<bool> allFavorites{false};
    std::optional<bool> noFavorites{true};
    if(allTidal) {
        QStringList ids;
        for(const Fooyin::Track& track : tracks) {
            ids.append(trackIdFrom(track.filepath()));
        }
        allFavorites = allFavorite(ids, TidalCatalog::List::FavoriteTracks, true);
        noFavorites  = allFavorite(ids, TidalCatalog::List::FavoriteTracks, false);
    }
    if(!allFavorites.value_or(false)) {
        menu->addAction(tr("Add to TIDAL &Favorites"), this, [this, tracks]() { setTrackFavorite(tracks, true); });
    }
    if(!noFavorites.value_or(false)) {
        menu->addAction(tr("Remove from TIDAL F&avorites"), this,
                        [this, tracks]() { setTrackFavorite(tracks, false); });
    }
}

void TidalEditor::addPlaylistEntries(QMenu* menu, const Fooyin::TrackList& tracks)
{
    menu->addAction(tr("&New Playlist…"), this, [this, tracks]() { addToNewPlaylist(tracks); });
    menu->addSeparator();

    const QList<TidalItem> own = ownPlaylists();
    if(own.isEmpty()) {
        auto* none = menu->addAction(m_prefetching.contains(TidalCatalog::List::Playlists)
                                         ? tr("Loading your playlists…")
                                         : tr("No playlists of your own yet"));
        none->setEnabled(false);
        return;
    }

    // Most recently changed first, as TIDAL lists them.
    for(const TidalItem& playlist : own) {
        menu->addAction(menuText(playlist.title), this, [this, playlist, tracks]() {
            edit(
                [id = playlist.id, title = playlist.title, tracks](TidalCatalog& catalog) -> std::expected<QString, QString> {
                    const auto found = findOnTidal(catalog, tracks);
                    if(!found) {
                        return std::unexpected(found.error());
                    }
                    if(const auto added = catalog.addToPlaylist(id, found->ids); !added) {
                        return std::unexpected(added.error());
                    }
                    return outcome(tr("Added %1 to %2").arg(describe(tracks), title), *found);
                },
                tr("Couldn't add to %1").arg(playlist.title),
                {{TidalCatalog::List::PlaylistTracks, playlist.id, {}}, {TidalCatalog::List::Playlists, {}, {}}});
        });
    }
}

void TidalEditor::addFavoriteActions(QMenu* menu, const QList<TidalItem>& items)
{
    if(items.isEmpty() || !isLoggedIn()) {
        return;
    }
    const TidalItem::Kind kind = items.front().kind;
    if(!std::ranges::all_of(items, [kind](const TidalItem& item) { return item.kind == kind; })) {
        return;
    }

    QStringList ids;
    for(const TidalItem& item : items) {
        ids.append(item.id);
    }
    const TidalCatalog::List list = favoritesList(kind);
    // When the favorites list isn't cached yet, both are offered.
    const std::optional<bool> allFavorites = allFavorite(ids, list, true);
    const std::optional<bool> noFavorites  = allFavorite(ids, list, false);
    const QString what                      = describe(items);

    if(!allFavorites.value_or(false)) {
        menu->addAction(tr("Add to TIDAL &Favorites"), this, [this, kind, ids, list, what]() {
            edit(
                [kind, ids, what](TidalCatalog& catalog) -> std::expected<QString, QString> {
                    if(const auto done = catalog.setFavorite(kind, ids, true); !done) {
                        return std::unexpected(done.error());
                    }
                    return tr("Added %1 to your TIDAL favorites").arg(what);
                },
                tr("Couldn't add %1 to your favorites").arg(what), {{list, {}, {}}});
        });
    }
    if(!noFavorites.value_or(false)) {
        menu->addAction(tr("Remove from TIDAL F&avorites"), this, [this, kind, ids, list, what]() {
            edit(
                [kind, ids, what](TidalCatalog& catalog) -> std::expected<QString, QString> {
                    if(const auto done = catalog.setFavorite(kind, ids, false); !done) {
                        return std::unexpected(done.error());
                    }
                    return tr("Removed %1 from your TIDAL favorites").arg(what);
                },
                tr("Couldn't remove %1 from your favorites").arg(what), {{list, {}, ids}});
        });
    }
}

void TidalEditor::addFavorite(const Fooyin::TrackList& tracks)
{
    setTrackFavorite(tracks, true);
}

void TidalEditor::setTrackFavorite(const Fooyin::TrackList& tracks, bool favorite)
{
    const QString what = describe(tracks);
    // Only TIDAL tracks are offered for removal, so their IDs are known now.
    QStringList removed;
    if(!favorite) {
        for(const Fooyin::Track& track : tracks) {
            removed.append(trackIdFrom(track.filepath()));
        }
    }
    edit(
        [tracks, favorite, what](TidalCatalog& catalog) -> std::expected<QString, QString> {
            const auto found = findOnTidal(catalog, tracks);
            if(!found) {
                return std::unexpected(found.error());
            }
            if(const auto done = catalog.setFavorite(TidalItem::Kind::Track, found->ids, favorite); !done) {
                return std::unexpected(done.error());
            }
            return outcome(favorite ? tr("Added %1 to your TIDAL favorites").arg(what)
                                    : tr("Removed %1 from your TIDAL favorites").arg(what),
                           *found);
        },
        favorite ? tr("Couldn't add %1 to your favorites").arg(what)
                 : tr("Couldn't remove %1 from your favorites").arg(what),
        {{TidalCatalog::List::FavoriteTracks, {}, removed}});
}

void TidalEditor::addToNewPlaylist(const Fooyin::TrackList& tracks)
{
    bool ok{false};
    const QString name = QInputDialog::getText(Fooyin::Utils::getMainWindow(), tr("New TIDAL Playlist"), tr("Name:"),
                                               QLineEdit::Normal, {}, &ok)
                             .trimmed();
    if(!ok || name.isEmpty()) {
        return;
    }
    edit(
        [name, tracks](TidalCatalog& catalog) -> std::expected<QString, QString> {
            // Looked up first, so a playlist isn't left empty when TIDAL has none of them.
            const auto found = findOnTidal(catalog, tracks);
            if(!found) {
                return std::unexpected(found.error());
            }
            const auto id = catalog.createPlaylist(name);
            if(!id) {
                return std::unexpected(id.error());
            }
            if(const auto added = catalog.addToPlaylist(*id, found->ids); !added) {
                return std::unexpected(added.error());
            }
            return outcome(tr("Made the TIDAL playlist %1 with %2").arg(name, describe(tracks)), *found);
        },
        tr("Couldn't make the TIDAL playlist %1").arg(name), {{TidalCatalog::List::Playlists, {}, {}}});
}

void TidalEditor::removeFromPlaylist(const QString& playlistId, const QString& playlistTitle,
                                     const QList<TidalItem>& entries)
{
    QStringList keys;
    for(const TidalItem& entry : entries) {
        keys.append(entry.playlistItemId);
    }
    const QString what = describe(entries);
    edit(
        [playlistId, playlistTitle, entries, what](TidalCatalog& catalog) -> std::expected<QString, QString> {
            if(const auto done = catalog.removeFromPlaylist(playlistId, entries); !done) {
                return std::unexpected(done.error());
            }
            return tr("Removed %1 from %2").arg(what, playlistTitle);
        },
        tr("Couldn't remove from %1").arg(playlistTitle),
        {{TidalCatalog::List::PlaylistTracks, playlistId, keys}, {TidalCatalog::List::Playlists, {}, {}}});
}

void TidalEditor::edit(const Edit& change, const QString& failed, const QList<Changed>& changed)
{
    Q_EMIT notice(tr("Saving to TIDAL…"));

    using Result  = std::expected<QString, QString>;
    auto* watcher = new QFutureWatcher<Result>(this);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, failed, changed]() {
        const Result result = watcher->result();
        watcher->deleteLater();
        QString message;
        if(!result) {
            qCWarning(TIDAL) << failed << "-" << result.error();
            message = u"%1: %2"_s.arg(failed, result.error());
        }
        else {
            message = *result;
            for(const Changed& list : changed) {
                // Checked with TIDAL the next time it's shown.
                if(TidalCache::Entry* entry = m_cache->find(list.list, list.id)) {
                    entry->checked = {};
                }
            }
            Q_EMIT this->changed(changed);
        }
        Q_EMIT notice(message);
        // For edits made outside the panel: fooyin's status bar, if the layout has one.
        Fooyin::StatusEvent::post(message, StatusTimeoutMs);
    });
    watcher->setFuture(QtConcurrent::run([account = m_account, change]() {
        TidalCatalog catalog{account};
        return change(catalog);
    }));
}

void TidalEditor::prefetch(TidalCatalog::List list)
{
    if(!isLoggedIn() || m_prefetching.contains(list)) {
        return;
    }
    m_prefetching.append(list);

    struct Fetched
    {
        QString version;
        QList<QJsonObject> pages;
    };
    using Result  = std::expected<Fetched, QString>;
    auto* watcher = new QFutureWatcher<Result>(this);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, list]() {
        const Result result = watcher->result();
        watcher->deleteLater();
        m_prefetching.removeAll(list);
        if(!result) {
            qCInfo(TIDAL) << "Couldn't fetch a TIDAL list for the menus:" << result.error();
            return;
        }
        m_cache->store(list, {}, result->version, result->pages);
    });
    watcher->setFuture(QtConcurrent::run([account = m_account, list]() -> Result {
        TidalCatalog catalog{account};
        const auto version = fetchVersion(catalog, list);
        if(!version) {
            return std::unexpected(version.error());
        }
        auto pages = catalog.allDocuments(list);
        if(!pages) {
            return std::unexpected(pages.error());
        }
        return Fetched{.version = *version, .pages = std::move(*pages)};
    }));
}

QList<TidalItem> TidalEditor::ownPlaylists()
{
    const TidalCache::Entry* entry = m_cache->find(TidalCatalog::List::Playlists, {});
    // Listed before the panel asked TIDAL for playlists' owners: fetched again.
    if(entry && !entry->items.isEmpty()
       && std::ranges::none_of(entry->items, [](const TidalItem& item) { return !item.owners.isEmpty(); })) {
        m_cache->remove(TidalCatalog::List::Playlists, {});
        entry = nullptr;
    }
    if(!entry) {
        prefetch(TidalCatalog::List::Playlists);
        return {};
    }
    QList<TidalItem> own;
    const QString me = m_account->userId();
    for(const TidalItem& playlist : entry->items) {
        if(!me.isEmpty() && playlist.owners.contains(me)) {
            own.push_back(playlist);
        }
    }
    return own;
}

bool TidalEditor::isOwnPlaylist(const TidalItem& playlist)
{
    const QString me = m_account->userId();
    if(me.isEmpty() || playlist.kind != TidalItem::Kind::Playlist) {
        return false;
    }
    if(!playlist.owners.isEmpty()) {
        return playlist.owners.contains(me);
    }
    // Search results don't say who owns a playlist; your playlists list does.
    return std::ranges::any_of(ownPlaylists(), [&playlist](const TidalItem& own) { return own.id == playlist.id; });
}

std::optional<bool> TidalEditor::allFavorite(const QStringList& ids, TidalCatalog::List list, bool favorite)
{
    const TidalCache::Entry* entry = m_cache->find(list, {});
    if(!entry) {
        prefetch(list);
        return std::nullopt;
    }
    QSet<QString> favorites;
    for(const TidalItem& item : entry->items) {
        favorites.insert(item.id);
    }
    return std::ranges::all_of(ids, [&favorites, favorite](const QString& id) { return favorites.contains(id) == favorite; });
}
} // namespace FooyinTidal

#include "moc_tidaleditor.cpp"
