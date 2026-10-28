/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include <QIODevice>
#include <QStringList>

#include <condition_variable>
#include <memory>
#include <mutex>

class QThread;

namespace FooyinTidal {
/*!
 * A sequential device over several URLs fetched one after another, as one stream of bytes. A
 * download thread fills it ahead of the reader; reads block until data arrives, the stream ends,
 * a download fails or abort() is called.
 *
 * Used for TIDAL's DASH streams, whose initialization and media segments joined in order are one
 * fragmented MP4 that FFmpeg reads like a file.
 */
class SegmentStreamDevice : public QIODevice
{
public:
    explicit SegmentStreamDevice(QStringList urls);
    ~SegmentStreamDevice() override;

    //! Starts downloading and opens the device for reading.
    bool start();
    //! Stops downloading and wakes any blocked read, which then fails. Safe from any thread.
    void abort();

    [[nodiscard]] QString lastError() const;

    [[nodiscard]] bool isSequential() const override;
    [[nodiscard]] qint64 bytesAvailable() const override;
    [[nodiscard]] bool atEnd() const override;

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char* data, qint64 maxSize) override;

private:
    void download();
    void append(const QByteArray& chunk);
    [[nodiscard]] bool isAborted() const;

    QStringList m_urls;
    std::unique_ptr<QThread> m_thread;

    mutable std::mutex m_mutex;
    std::condition_variable m_dataChanged;
    //! Downloaded bytes not yet read; the front is dropped as it's read.
    QByteArray m_buffer;
    qsizetype m_readPos{0};
    bool m_finished{false};
    bool m_aborted{false};
    QString m_error;
};
} // namespace FooyinTidal
