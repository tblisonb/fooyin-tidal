/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include "tidalapi.h"

#include <core/engine/audioinput.h>

#include <QElapsedTimer>

#include <atomic>
#include <functional>
#include <memory>

namespace FooyinTidal {
class SegmentStreamDevice;
class TidalSession;

/*!
 * Plays tidal://track/<id>. When a track starts, asks TIDAL for its stream, downloads it through a
 * SegmentStreamDevice and hands that to fooyin's own FFmpeg decoder, which does the decoding.
 * Everything else is passed through to that decoder.
 *
 * Seeking restarts the download at the segment holding the target, with a fresh FFmpeg decoder
 * (the segments carry their own timestamps), and drops decoded audio before the exact target.
 */
class TidalDecoder : public Fooyin::AudioDecoder
{
public:
    using DecoderFactory = std::function<std::unique_ptr<Fooyin::AudioDecoder>()>;

    TidalDecoder(std::shared_ptr<TidalSession> session, DecoderFactory createDecoder);
    ~TidalDecoder() override;

    [[nodiscard]] QStringList extensions() const override;
    [[nodiscard]] QStringList supportedSchemes() const override;
    [[nodiscard]] bool needsMoreInput() const override;
    [[nodiscard]] bool isSeekable() const override;
    [[nodiscard]] bool allowsConcurrentDecoding() const override;
    [[nodiscard]] int playbackPrebufferMs() const override;
    [[nodiscard]] QStringList takeWarnings() override;
    [[nodiscard]] int bitrate() const override;

    std::optional<Fooyin::AudioFormat> init(const Fooyin::AudioSource& source, const Fooyin::Track& track,
                                            DecoderOptions options) override;
    void start() override;
    void stop() override;
    void seek(uint64_t pos) override;
    ReadResult readAudio(size_t bytes) override;
    Fooyin::AudioBuffer readBuffer(size_t bytes) override;

protected:
    void playbackHintsChanged(PlaybackHints hints) override;
    void interruptRead() override;

private:
    bool fetchStream();
    //! Downloads from the media segment at `segment` on (0 is the start) into a new FFmpeg decoder.
    std::optional<Fooyin::AudioFormat> openStream(qsizetype segment);
    void closeStream();
    //! Drops the part of `buffer` before the seek target; true if anything is left.
    bool trimToSeekTarget(Fooyin::AudioBuffer& buffer);

    std::shared_ptr<TidalSession> m_session;
    DecoderFactory m_createDecoder;

    QString m_trackId;
    Fooyin::Track m_track;
    DecoderOptions m_options{None};
    TidalStream m_stream;
    //! Since m_stream was fetched: its segment URLs are signed and expire.
    QElapsedTimer m_streamAge;
    bool m_started{false};
    //! Decoded audio before this position (ms) is dropped after a seek.
    std::optional<uint64_t> m_seekTarget;

    // The decoder reads from the device, so it's declared after it and destroyed first.
    std::shared_ptr<SegmentStreamDevice> m_device;
    std::unique_ptr<Fooyin::AudioDecoder> m_decoder;
    //! The device to abort; interruptRead() can run on another thread while a stream is (re)opened.
    std::atomic<std::shared_ptr<SegmentStreamDevice>> m_abortTarget;
};
} // namespace FooyinTidal
