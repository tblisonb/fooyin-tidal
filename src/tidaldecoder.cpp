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
#include "tidalapi.h"
#include "tidalsession.h"

#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(TIDAL)

using namespace Qt::StringLiterals;

namespace FooyinTidal {
TidalDecoder::TidalDecoder(std::shared_ptr<TidalSession> session, DecoderFactory createDecoder)
    : m_session{std::move(session)}
    , m_createDecoder{std::move(createDecoder)}
{ }

TidalDecoder::~TidalDecoder()
{
    // Stop the download before the decoder that reads from it goes.
    if(m_device) {
        m_device->abort();
    }
    m_decoder.reset();
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
    // The stream is read straight through for now; seeking will jump to the right segment.
    return false;
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
    const QString trackId = trackIdFrom(source.filepath);
    if(trackId.isEmpty()) {
        qCWarning(TIDAL) << "Not a TIDAL track:" << source.filepath;
        return {};
    }

    TidalApi api{m_session};
    const auto token  = abortToken();
    const auto stream = api.stream(trackId, [token]() { return token.stop_requested(); });
    if(!stream) {
        qCWarning(TIDAL) << "Can't play TIDAL track" << trackId << "-" << stream.error();
        return {};
    }
    qCInfo(TIDAL) << "Playing TIDAL track" << trackId << "at" << stream->audioQuality << stream->bitDepth << "bit"
                  << stream->sampleRate << "Hz," << stream->urls.size() << "segments";

    m_decoder = m_createDecoder ? m_createDecoder() : nullptr;
    if(!m_decoder) {
        qCWarning(TIDAL) << "fooyin's FFmpeg decoder isn't available";
        return {};
    }

    m_device = std::make_unique<SegmentStreamDevice>(stream->urls);
    if(!m_device->start()) {
        return {};
    }
    m_abortTarget = m_device.get();
    if(token.stop_requested()) {
        // Aborted while the stream was being set up, before interruptRead() could see the device.
        m_device->abort();
        return {};
    }

    // The joined segments are a fragmented MP4; the name tells FFmpeg what to expect.
    Fooyin::AudioSource streamSource;
    streamSource.filepath = u"tidal-%1.mp4"_s.arg(trackId);
    streamSource.device   = m_device.get();

    m_decoder->setPlaybackHints(playbackHints());
    auto format = m_decoder->init(streamSource, track, options);
    if(!format) {
        const QString error = m_device->lastError();
        qCWarning(TIDAL) << "Couldn't decode TIDAL track" << trackId << (error.isEmpty() ? QString{} : u"-"_s + error);
    }
    return format;
}

void TidalDecoder::start()
{
    if(m_decoder) {
        m_decoder->start();
    }
}

void TidalDecoder::stop()
{
    if(m_decoder) {
        m_decoder->stop();
    }
}

void TidalDecoder::seek(uint64_t pos)
{
    // Not seekable yet (see isSeekable()): the stream always plays from the start.
    if(pos > 0) {
        qCDebug(TIDAL) << "Seeking isn't supported for TIDAL tracks yet";
    }
}

Fooyin::AudioDecoder::ReadResult TidalDecoder::readAudio(size_t bytes)
{
    return m_decoder ? m_decoder->readAudio(bytes) : ReadResult::errorResult(u"Not initialised"_s);
}

Fooyin::AudioBuffer TidalDecoder::readBuffer(size_t bytes)
{
    return m_decoder ? m_decoder->readBuffer(bytes) : Fooyin::AudioBuffer{};
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
    if(auto* device = m_abortTarget.load()) {
        device->abort();
    }
}
} // namespace FooyinTidal
