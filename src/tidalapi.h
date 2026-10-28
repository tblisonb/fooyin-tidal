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

#include <expected>
#include <memory>

namespace FooyinTidal {
class TidalSession;

//! The fooyin path of a TIDAL track: tidal://track/<id>.
QString trackPath(const QString& trackId);
//! The track ID in a tidal://track path, a tidal.com or listen.tidal.com link, or a bare ID.
QString trackIdFrom(const QString& pathOrLink);

//! What v1's playbackinfo gives for a track: a manifest to fetch the audio with.
struct TidalStream
{
    QString audioQuality;
    int bitDepth{0};
    int sampleRate{0};
    //! Every URL to fetch, in order; joined, they make one file (for DASH, a fragmented MP4).
    QStringList urls;
    QString codec;
    //! For DASH: where each media segment starts, in ms. urls[0] is the initialization segment and
    //! urls[i + 1] the media segment starting at segmentStartsMs[i]. Empty for a single file.
    QList<uint64_t> segmentStartsMs;

    //! The media segment holding `ms` (the last one starting at or before it), as an index into
    //! segmentStartsMs. Only for DASH streams.
    [[nodiscard]] qsizetype segmentAt(uint64_t ms) const;
    //! The URLs to fetch to play from `segment` on: the initialization segment, then the rest.
    [[nodiscard]] QStringList urlsFrom(qsizetype segment) const;
};

/*!
 * Calls to TIDAL's private v1 API with the playback login. Blocking: use it on a worker thread,
 * or from a QtConcurrent task. One instance per thread.
 */
class TidalApi
{
public:
    explicit TidalApi(std::shared_ptr<TidalSession> session);

    //! The track's details, as a fooyin track that plays through this plugin.
    std::expected<Fooyin::Track, QString> track(const QString& trackId);
    //! Where to fetch the track's audio. Asks for hi-res; TIDAL gives the best it has up to that.
    std::expected<TidalStream, QString> stream(const QString& trackId, const BlockingHttp::Cancelled& cancelled = {});

private:
    std::expected<QByteArray, QString> getV1(const QString& path, const QUrlQuery& query,
                                             const BlockingHttp::Cancelled& cancelled = {});

    std::shared_ptr<TidalSession> m_session;
    BlockingHttp m_http;
};
} // namespace FooyinTidal
