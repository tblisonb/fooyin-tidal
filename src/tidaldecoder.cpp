/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "tidaldecoder.h"

#include "segmentstreamdevice.h"
#include "tidalsession.h"

#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(TIDAL)

using namespace Qt::StringLiterals;

namespace {
// Segment URLs are signed; fetch the stream again for a seek once it's this old.
constexpr qint64 StreamMaxAgeMs = 5 * 60 * 1000;
} // namespace

namespace FooyinTidal {
TidalDecoder::TidalDecoder(std::shared_ptr<TidalSession> session, DecoderFactory createDecoder)
    : m_session{std::move(session)}
    , m_createDecoder{std::move(createDecoder)}
{ }

TidalDecoder::~TidalDecoder()
{
    closeStream();
}

QStringList TidalDecoder::extensions() const
{
    return {};
}

QStringList TidalDecoder::supportedSchemes() const
{
    return {u"tidal"_s};
}

bool TidalDecoder::needsMoreInput() const
{
    return m_decoder && m_decoder->needsMoreInput();
}

bool TidalDecoder::isSeekable() const
{
    // A single-file stream (rare) is read straight through; DASH streams seek by segment.
    return !m_stream.segmentStartsMs.isEmpty();
}

bool TidalDecoder::allowsConcurrentDecoding() const
{
    return m_decoder && m_decoder->allowsConcurrentDecoding();
}

int TidalDecoder::playbackPrebufferMs() const
{
    return m_decoder ? m_decoder->playbackPrebufferMs() : AudioDecoder::playbackPrebufferMs();
}

QStringList TidalDecoder::takeWarnings()
{
    return m_decoder ? m_decoder->takeWarnings() : QStringList{};
}

int TidalDecoder::bitrate() const
{
    return m_decoder ? m_decoder->bitrate() : 0;
}

std::optional<Fooyin::AudioFormat> TidalDecoder::init(const Fooyin::AudioSource& source, const Fooyin::Track& track,
                                                      DecoderOptions options)
{
    m_trackId = trackIdFrom(source.filepath);
    if(m_trackId.isEmpty()) {
        qCWarning(TIDAL) << "Not a TIDAL track:" << source.filepath;
        return {};
    }
    m_track   = track;
    m_options = options;

    if(!fetchStream()) {
        return {};
    }
    qCInfo(TIDAL) << "Playing TIDAL track" << m_trackId << "at" << m_stream.audioQuality << m_stream.bitDepth << "bit"
                  << m_stream.sampleRate << "Hz," << m_stream.urls.size() << "segments";

    return openStream(0);
}

bool TidalDecoder::fetchStream()
{
    TidalApi api{m_session};
    const auto token  = abortToken();
    const auto stream = api.stream(m_trackId, [token]() { return token.stop_requested(); });
    if(!stream) {
        qCWarning(TIDAL) << "Can't play TIDAL track" << m_trackId << "-" << stream.error();
        return false;
    }
    m_stream = *stream;
    m_streamAge.start();
    return true;
}

std::optional<Fooyin::AudioFormat> TidalDecoder::openStream(qsizetype segment)
{
    m_decoder = m_createDecoder ? m_createDecoder() : nullptr;
    if(!m_decoder) {
        qCWarning(TIDAL) << "fooyin's FFmpeg decoder isn't available";
        return {};
    }

    m_device = std::make_shared<SegmentStreamDevice>(m_stream.urlsFrom(segment));
    if(!m_device->start()) {
        return {};
    }
    m_abortTarget.store(m_device);
    if(abortToken().stop_requested()) {
        // Aborted while the stream was being set up, before interruptRead() could see the device.
        m_device->abort();
        return {};
    }

    // The joined segments are a fragmented MP4; the name tells FFmpeg what to expect.
    Fooyin::AudioSource streamSource;
    streamSource.filepath = u"tidal-%1.mp4"_s.arg(m_trackId);
    streamSource.device   = m_device.get();

    m_decoder->setPlaybackHints(playbackHints());
    auto format = m_decoder->init(streamSource, m_track, m_options);
    if(!format) {
        const QString error = m_device->lastError();
        qCWarning(TIDAL) << "Couldn't decode TIDAL track" << m_trackId
                         << (error.isEmpty() ? QString{} : u"-"_s + error);
    }
    return format;
}

void TidalDecoder::closeStream()
{
    m_abortTarget.store(nullptr);
    if(m_device) {
        m_device->abort();
    }
    m_decoder.reset();
    m_device.reset();
}

void TidalDecoder::start()
{
    m_started = true;
    if(m_decoder) {
        m_decoder->start();
    }
}

void TidalDecoder::stop()
{
    m_started = false;
    if(m_decoder) {
        m_decoder->stop();
    }
}

void TidalDecoder::seek(uint64_t pos)
{
    if(!isSeekable() || abortToken().stop_requested()) {
        return;
    }

    closeStream();
    if(!m_streamAge.isValid() || m_streamAge.elapsed() > StreamMaxAgeMs) {
        if(!fetchStream()) {
            return;
        }
    }

    const qsizetype segment = m_stream.segmentAt(pos);
    if(!openStream(segment)) {
        // Leave no decoder, so reads report the failure rather than reading a broken stream.
        closeStream();
        return;
    }
    if(m_started) {
        m_decoder->start();
    }
    m_seekTarget = pos > m_stream.segmentStartsMs.at(segment) ? std::optional{pos} : std::nullopt;
}

bool TidalDecoder::trimToSeekTarget(Fooyin::AudioBuffer& buffer)
{
    if(!m_seekTarget || !buffer.isValid() || buffer.byteCount() == 0) {
        return true;
    }

    const uint64_t target = *m_seekTarget;
    if(buffer.endTime() <= target) {
        buffer.clear();
        return false;
    }
    if(buffer.startTime() < target) {
        const auto format = buffer.format();
        const int frames  = format.framesForDuration(target - buffer.startTime());
        buffer.erase(static_cast<size_t>(format.bytesForFrames(frames)));
        buffer.setStartTime(target);
    }
    m_seekTarget.reset();
    return buffer.byteCount() > 0;
}

Fooyin::AudioDecoder::ReadResult TidalDecoder::readAudio(size_t bytes)
{
    if(!m_decoder) {
        return ReadResult::errorResult(u"Not initialised"_s);
    }
    while(true) {
        ReadResult result = m_decoder->readAudio(bytes);
        if(result.status != ReadStatus::DecodedAudio || trimToSeekTarget(result.buffer)) {
            return result;
        }
    }
}

Fooyin::AudioBuffer TidalDecoder::readBuffer(size_t bytes)
{
    if(!m_decoder) {
        return {};
    }
    while(true) {
        Fooyin::AudioBuffer buffer = m_decoder->readBuffer(bytes);
        if(!buffer.isValid() || trimToSeekTarget(buffer)) {
            return buffer;
        }
    }
}

void TidalDecoder::playbackHintsChanged(PlaybackHints hints)
{
    if(m_decoder) {
        m_decoder->setPlaybackHints(hints);
    }
}

void TidalDecoder::interruptRead()
{
    // Called once, when fooyin is done with this decoder. Ending the download fails any blocked
    // read, which ends FFmpeg's read too.
    if(const auto device = m_abortTarget.load()) {
        device->abort();
    }
}
} // namespace FooyinTidal
