/*
 * Runs the panel's calls to TIDAL's official API, without fooyin, with the plugin's account login,
 * and prints what the panel would list. scripts/catalog-check.sh runs it inside fooyin's Flatpak.
 *
 *   catalog-check <account.json> search <query>
 *   catalog-check <account.json> <list> [id] [--all]
 *
 * Lists: favorite-tracks, favorite-albums, favorite-artists, playlists, album, playlist, artist.
 */

#include "tidalcatalog.h"
#include "tidalsession.h"

#include <QCoreApplication>
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
        std::fprintf(stderr, "usage: catalog-check <account.json> search <query> | <list> [id] [--all]\n");
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
    const QString id   = args.size() > 2 && args.at(2) != "--all"_L1 ? args.at(2) : QString{};

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
