/*
 * Runs the plugin's own streaming code for one track, without fooyin: refreshes the playback login,
 * asks TIDAL for the stream, and downloads every segment through SegmentStreamDevice into a file,
 * which ffprobe can then check. scripts/stream-check.sh runs it inside fooyin's Flatpak.
 *
 *   stream-check <session.json> <track id or link> <output file>
 */

#include "segmentstreamdevice.h"
#include "tidalapi.h"
#include "tidalsession.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLoggingCategory>

#include <cstdio>

Q_LOGGING_CATEGORY(TIDAL, "fooyin.tidal")

using namespace FooyinTidal;

int main(int argc, char** argv)
{
    const QCoreApplication app{argc, argv};
    if(argc != 4) {
        std::fprintf(stderr, "usage: stream-check <session.json> <track id or link> <output file>\n");
        return 2;
    }

    auto session = std::make_shared<TidalSession>(QString::fromLocal8Bit(argv[1]));
    if(!session->isLoggedIn()) {
        std::fprintf(stderr, "FAIL: no playback login in %s\n", argv[1]);
        return 1;
    }

    const QString trackId = trackIdFrom(QString::fromLocal8Bit(argv[2]));
    TidalApi api{session};

    const auto track = api.track(trackId);
    if(!track) {
        std::fprintf(stderr, "FAIL: track details: %s\n", qPrintable(track.error()));
        return 1;
    }
    std::printf("Track: %s - %s (%s), %llu s\n", qPrintable(track->artists().join(u", ")),
                qPrintable(track->title()), qPrintable(track->album()),
                static_cast<unsigned long long>(track->duration() / 1000));

    const auto stream = api.stream(trackId);
    if(!stream) {
        std::fprintf(stderr, "FAIL: stream: %s\n", qPrintable(stream.error()));
        return 1;
    }
    std::printf("Stream: %s, %d bit %d Hz, %s, %lld URLs\n", qPrintable(stream->audioQuality), stream->bitDepth,
                stream->sampleRate, qPrintable(stream->codec), static_cast<long long>(stream->urls.size()));

    QFile out{QString::fromLocal8Bit(argv[3])};
    if(!out.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "FAIL: can't write %s\n", argv[3]);
        return 1;
    }

    QElapsedTimer timer;
    timer.start();
    SegmentStreamDevice device{stream->urls};
    if(!device.start()) {
        std::fprintf(stderr, "FAIL: couldn't start the download\n");
        return 1;
    }

    // Read the way FFmpeg does: small blocking reads until the end.
    char buffer[32768];
    qint64 total{0};
    qint64 firstDataMs{-1};
    while(true) {
        const qint64 read = device.read(buffer, sizeof(buffer));
        if(read < 0) {
            std::fprintf(stderr, "FAIL: read error after %lld bytes: %s\n", static_cast<long long>(total),
                         qPrintable(device.lastError()));
            return 1;
        }
        if(read == 0) {
            break;
        }
        if(firstDataMs < 0) {
            firstDataMs = timer.elapsed();
        }
        out.write(buffer, read);
        total += read;
    }

    std::printf("Downloaded %lld bytes in %lld ms (first data after %lld ms) to %s\n", static_cast<long long>(total),
                static_cast<long long>(timer.elapsed()), static_cast<long long>(firstDataMs), argv[3]);
    return 0;
}
