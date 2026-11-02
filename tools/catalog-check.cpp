/*
 * Runs the panel's calls to TIDAL's official API, without fooyin, with the plugin's account login,
 * and prints what the panel would list. scripts/catalog-check.sh runs it inside fooyin's Flatpak.
 *
 *   catalog-check <account.json> search <query>
 *   catalog-check <account.json> <list> [id] [--all]
 *
 * Lists: favorite-tracks, favorite-albums, favorite-artists, playlists, album, playlist, artist.
 */

#include "tidalcache.h"
#include "tidalcatalog.h"
#include "tidalsession.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QLoggingCategory>

#include <cstdio>
#include <map>

Q_LOGGING_CATEGORY(TIDAL, "fooyin.tidal")

using namespace FooyinTidal;
using namespace Qt::StringLiterals;

namespace {
void print(const QList<TidalItem>& items)
{
    for(const TidalItem& item : items) {
        std::printf("  %-12s %s | %s | %s | %s", qPrintable(item.id), qPrintable(item.title), qPrintable(item.artist),
                    qPrintable(item.album), qPrintable(item.info));
        if(item.added.isValid()) {
            std::printf(" | added %s", qPrintable(item.added.toString(Qt::ISODate)));
        }
        if(item.modified.isValid()) {
            std::printf(" | modified %s", qPrintable(item.modified.toString(Qt::ISODate)));
        }
        if(item.kind == TidalItem::Kind::Track && !item.track.trackNumber().isEmpty()) {
            std::printf(" | #%s.%s %s", qPrintable(item.track.discNumber()), qPrintable(item.track.trackNumber()),
                        qPrintable(item.track.date()));
        }
        std::printf("\n");
    }
}
} // namespace

int main(int argc, char** argv)
{
    const QCoreApplication app{argc, argv};
    const QStringList args = app.arguments().mid(1);
    if(args.size() < 2) {
        std::fprintf(stderr, "usage: catalog-check <account.json> search <query> | create-playlist <name> | playlist-add|playlist-remove <playlist> <track>... | favorite|unfavorite <kind> <id>... | find <title> <artist> <album> <ms> [isrc] | raw <METHOD> <path> [json] | <list> [id] [--all | --version | --cache <dir>]\n");
        return 2;
    }

    auto account = std::make_shared<TidalSession>(args.at(0), QString{});
    if(!account->isLoggedIn()) {
        std::fprintf(stderr, "FAIL: no account login in %s\n", qPrintable(args.at(0)));
        return 1;
    }
    TidalCatalog catalog{account};

    if(args.at(1) == "search"_L1) {
        const auto result = catalog.search(args.mid(2).join(u' '));
        if(!result) {
            std::fprintf(stderr, "FAIL: %s\n", qPrintable(result.error()));
            return 1;
        }
        for(const auto& [name, items] : {std::pair{"Tracks", result->tracks}, std::pair{"Albums", result->albums},
                                         std::pair{"Artists", result->artists},
                                         std::pair{"Playlists", result->playlists}}) {
            std::printf("%s (%lld)\n", name, static_cast<long long>(items.size()));
            print(items);
        }
        return 0;
    }

    // Edits, through the same calls the panel makes.
    const auto done = [](const std::expected<void, QString>& result) {
        if(!result) {
            std::fprintf(stderr, "FAIL: %s\n", qPrintable(result.error()));
            return 1;
        }
        std::printf("OK\n");
        return 0;
    };
    if(args.at(1) == "create-playlist"_L1 && args.size() >= 3) {
        const auto id = catalog.createPlaylist(args.mid(2).join(u' '));
        if(!id) {
            std::fprintf(stderr, "FAIL: %s\n", qPrintable(id.error()));
            return 1;
        }
        std::printf("created %s\n", qPrintable(*id));
        return 0;
    }
    if(args.at(1) == "playlist-add"_L1 && args.size() >= 4) {
        return done(catalog.addToPlaylist(args.at(2), args.mid(3)));
    }
    if(args.at(1) == "playlist-remove"_L1 && args.size() >= 4) {
        // Every entry of these tracks, as the playlist lists them.
        const auto entries = catalog.all(TidalCatalog::List::PlaylistTracks, args.at(2));
        if(!entries) {
            std::fprintf(stderr, "FAIL: %s\n", qPrintable(entries.error()));
            return 1;
        }
        QList<TidalItem> remove;
        for(const TidalItem& entry : *entries) {
            if(args.mid(3).contains(entry.id)) {
                remove.push_back(entry);
            }
        }
        std::printf("removing %lld entries\n", static_cast<long long>(remove.size()));
        return done(catalog.removeFromPlaylist(args.at(2), remove));
    }
    if((args.at(1) == "favorite"_L1 || args.at(1) == "unfavorite"_L1) && args.size() >= 4) {
        static const std::map<QString, TidalItem::Kind> kinds{{u"track"_s, TidalItem::Kind::Track},
                                                              {u"album"_s, TidalItem::Kind::Album},
                                                              {u"artist"_s, TidalItem::Kind::Artist},
                                                              {u"playlist"_s, TidalItem::Kind::Playlist}};
        const auto kind = kinds.find(args.at(2));
        if(kind == kinds.end()) {
            std::fprintf(stderr, "Unknown kind %s\n", qPrintable(args.at(2)));
            return 2;
        }
        return done(catalog.setFavorite(kind->second, args.mid(3), args.at(1) == "favorite"_L1));
    }

    if(args.at(1) == "find"_L1 && args.size() >= 6) {
        // find <title> <artist> <album> <length ms> [ISRC]: the TIDAL track a local track matches.
        Fooyin::Track track{u"/music/local.flac"_s};
        track.setTitle(args.at(2));
        track.setArtists({args.at(3)});
        track.setAlbum(args.at(4));
        track.setDuration(args.at(5).toULongLong());
        if(args.size() > 6) {
            track.replaceExtraTag(u"ISRC"_s, args.at(6));
        }
        const auto id = catalog.findTrack(track);
        if(!id) {
            std::fprintf(stderr, "FAIL: %s\n", qPrintable(id.error()));
            return 1;
        }
        std::printf("%s\n", id->isEmpty() ? "no match" : qPrintable(*id));
        return 0;
    }

    if(args.at(1) == "raw"_L1 && args.size() >= 4) {
        // raw <METHOD> <path?query> [JSON body]: prints TIDAL's answer. Not for /users/me.
        const QUrl url{u"https://x"_s + args.at(3)};
        const QJsonObject body
            = args.size() > 4 ? QJsonDocument::fromJson(args.at(4).toUtf8()).object() : QJsonObject{};
        const auto result = catalog.request(args.at(2).toLatin1(), url.path(), QUrlQuery{url.query()}, body);
        if(!result) {
            std::fprintf(stderr, "FAIL: %s\n", qPrintable(result.error()));
            return 1;
        }
        std::printf("%s\n", QJsonDocument{*result}.toJson(QJsonDocument::Indented).constData());
        return 0;
    }

    static const std::map<QString, TidalCatalog::List> lists{
        {u"favorite-tracks"_s, TidalCatalog::List::FavoriteTracks},
        {u"favorite-albums"_s, TidalCatalog::List::FavoriteAlbums},
        {u"favorite-artists"_s, TidalCatalog::List::FavoriteArtists},
        {u"playlists"_s, TidalCatalog::List::Playlists},
        {u"album"_s, TidalCatalog::List::AlbumTracks},
        {u"playlist"_s, TidalCatalog::List::PlaylistTracks},
        {u"artist"_s, TidalCatalog::List::ArtistAlbums}};
    const auto list = lists.find(args.at(1));
    if(list == lists.end()) {
        std::fprintf(stderr, "Unknown list %s\n", qPrintable(args.at(1)));
        return 2;
    }
    const bool all     = args.contains(u"--all"_s);
    const QString id   = args.size() > 2 && !args.at(2).startsWith(u"--"_s) ? args.at(2) : QString{};

    if(args.contains(u"--version"_s)) {
        const auto version = catalog.version(list->second, id);
        if(!version) {
            std::fprintf(stderr, "FAIL: %s\n", qPrintable(version.error()));
            return 1;
        }
        std::printf("version: %s\n", version->isEmpty() ? "(none: not checked)" : qPrintable(*version));
        return 0;
    }

    if(const qsizetype cacheArg = args.indexOf(u"--cache"_s); cacheArg >= 0 && cacheArg + 1 < args.size()) {
        // What the panel does: fetch the list and its version, store it, then read it back from
        // disk as a new session would, and check the version again.
        const QString dir = args.at(cacheArg + 1);
        const auto version = catalog.version(list->second, id);
        const auto pages   = catalog.allDocuments(list->second, id);
        if(!version || !pages) {
            std::fprintf(stderr, "FAIL: %s\n", qPrintable(!version ? version.error() : pages.error()));
            return 1;
        }
        const qsizetype stored
            = TidalCache{dir, account}.store(list->second, id, *version, *pages)->items.size();
        TidalCache reread{dir, account};
        const TidalCache::Entry* entry = reread.find(list->second, id);
        if(!entry) {
            std::fprintf(stderr, "FAIL: not found in the cache after storing it\n");
            return 1;
        }
        const auto again = catalog.version(list->second, id);
        std::printf("stored %lld items in %lld pages; read back %lld items; version %s; still current: %s\n",
                    static_cast<long long>(stored), static_cast<long long>(pages->size()),
                    static_cast<long long>(entry->items.size()), qPrintable(entry->version),
                    again && *again == entry->version ? "yes" : "NO");
        return entry->items.size() == stored && again && *again == entry->version ? 0 : 1;
    }

    if(all) {
        const auto items = catalog.all(list->second, id);
        if(!items) {
            std::fprintf(stderr, "FAIL: %s\n", qPrintable(items.error()));
            return 1;
        }
        std::printf("%lld items\n", static_cast<long long>(items->size()));
        print(*items);
        return 0;
    }
    const auto page = catalog.page(list->second, id);
    if(!page) {
        std::fprintf(stderr, "FAIL: %s\n", qPrintable(page.error()));
        return 1;
    }
    print(page->items);
    std::printf("next cursor: %s\n", page->nextCursor.isEmpty() ? "(none)" : "yes");
    return 0;
}
