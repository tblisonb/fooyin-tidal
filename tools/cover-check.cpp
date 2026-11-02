/*
 * Runs the plugin's cover reader for one track, without fooyin, the way fooyin's cover code calls
 * it: reads the track's details and its front cover into a file. Run it twice to see the cover
 * come from the cache. scripts/cover-check.sh runs it inside fooyin's Flatpak.
 *
 *   cover-check <session.json> <cover cache dir> <track id or link> <output file>
 */

#include "tidalapi.h"
#include "tidalcovers.h"
#include "tidallogin.h"
#include "tidalsession.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLoggingCategory>

#include <cstdio>

Q_LOGGING_CATEGORY(TIDAL, "fooyin.tidal")

using namespace FooyinTidal;

int main(int argc, char** argv)
{
    const QCoreApplication app{argc, argv};
    if(argc != 5) {
        std::fprintf(stderr, "usage: cover-check <session.json> <cover cache dir> <track id or link> <output file>\n");
        return 2;
    }

    auto session = std::make_shared<TidalSession>(QString::fromLocal8Bit(argv[1]), TidalLogin::playbackScopes());
    if(!session->isLoggedIn()) {
        std::fprintf(stderr, "FAIL: no playback login in %s\n", argv[1]);
        return 1;
    }
    auto covers = std::make_shared<TidalCovers>(QString::fromLocal8Bit(argv[2]), session);

    TidalReader reader{session, covers};
    Fooyin::AudioSource source;
    source.filepath = trackPath(trackIdFrom(QString::fromLocal8Bit(argv[3])));
    if(!reader.init(source)) {
        std::fprintf(stderr, "FAIL: not a TIDAL track: %s\n", argv[3]);
        return 1;
    }

    Fooyin::Track track{source.filepath};
    if(!reader.readTrack(source, track)) {
        std::fprintf(stderr, "FAIL: track details\n");
        return 1;
    }
    std::printf("Track: %s - %s (%s)\n", qPrintable(track.artists().join(u", ")), qPrintable(track.title()),
                qPrintable(track.album()));

    for(int attempt{1}; attempt <= 2; ++attempt) {
        QElapsedTimer timer;
        timer.start();
        const QByteArray cover = reader.readCover(source, track, Fooyin::Track::Cover::Front);
        if(cover.isEmpty()) {
            std::fprintf(stderr, "FAIL: no cover\n");
            return 1;
        }
        const QImage image = QImage::fromData(cover);
        std::printf("Cover, read %d: %lld bytes, %dx%d, %lld ms\n", attempt, static_cast<long long>(cover.size()),
                    image.width(), image.height(), static_cast<long long>(timer.elapsed()));
        if(image.isNull()) {
            std::fprintf(stderr, "FAIL: not an image\n");
            return 1;
        }
        if(attempt == 1) {
            QFile out{QString::fromLocal8Bit(argv[4])};
            if(!out.open(QIODevice::WriteOnly) || out.write(cover) != cover.size()) {
                std::fprintf(stderr, "FAIL: can't write %s\n", argv[4]);
                return 1;
            }
        }
    }
    std::printf("OK\n");
    return 0;
}
