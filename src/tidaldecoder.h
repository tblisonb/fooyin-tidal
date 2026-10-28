/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include <core/engine/audioinput.h>

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
    std::shared_ptr<TidalSession> m_session;
    DecoderFactory m_createDecoder;
    // The decoder reads from the device, so it's declared after it and destroyed first.
    std::unique_ptr<SegmentStreamDevice> m_device;
    std::unique_ptr<Fooyin::AudioDecoder> m_decoder;
    //! The device to abort; interruptRead() can run on another thread while init() sets up.
    std::atomic<SegmentStreamDevice*> m_abortTarget{nullptr};
};
} // namespace FooyinTidal
