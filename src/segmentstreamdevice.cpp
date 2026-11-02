/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "segmentstreamdevice.h"

#include "blockinghttp.h"

#include <QLoggingCategory>
#include <QNetworkReply>
#include <QThread>

#include <algorithm>
#include <cstring>

Q_DECLARE_LOGGING_CATEGORY(TIDAL)

using namespace Qt::StringLiterals;

namespace {
// Drop bytes already read once this many have piled up at the front of the buffer.
constexpr qsizetype CompactThreshold = 4 * 1024 * 1024;
} // namespace

namespace FooyinTidal {
SegmentStreamDevice::SegmentStreamDevice(QStringList urls)
    : m_urls{std::move(urls)}
{ }

SegmentStreamDevice::~SegmentStreamDevice()
{
    abort();
    if(m_thread) {
        m_thread->wait();
    }
}

bool SegmentStreamDevice::start()
{
    if(m_urls.isEmpty() || !open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        return false;
    }
    m_thread.reset(QThread::create([this]() { download(); }));
    m_thread->start();
    return true;
}

void SegmentStreamDevice::abort()
{
    {
        const std::scoped_lock lock{m_mutex};
        m_aborted = true;
    }
    m_dataChanged.notify_all();
}

QString SegmentStreamDevice::lastError() const
{
    const std::scoped_lock lock{m_mutex};
    return m_error;
}

bool SegmentStreamDevice::isSequential() const
{
    return true;
}

qint64 SegmentStreamDevice::bytesAvailable() const
{
    const std::scoped_lock lock{m_mutex};
    return (m_buffer.size() - m_readPos) + QIODevice::bytesAvailable();
}

bool SegmentStreamDevice::atEnd() const
{
    const std::scoped_lock lock{m_mutex};
    return m_finished && m_readPos == m_buffer.size() && QIODevice::bytesAvailable() == 0;
}

qint64 SegmentStreamDevice::readData(char* data, qint64 maxSize)
{
    std::unique_lock lock{m_mutex};
    m_dataChanged.wait(lock, [this]() { return m_aborted || m_finished || m_readPos < m_buffer.size(); });

    if(m_aborted) {
        return -1;
    }

    const qint64 available = m_buffer.size() - m_readPos;
    if(available == 0) {
        // Finished: a clean end of stream, or a failed download.
        return m_error.isEmpty() ? 0 : -1;
    }

    const qint64 count = std::min(maxSize, available);
    std::memcpy(data, m_buffer.constData() + m_readPos, static_cast<size_t>(count));
    m_readPos += count;

    if(m_readPos >= CompactThreshold) {
        m_buffer.remove(0, m_readPos);
        m_readPos = 0;
    }
    return count;
}

qint64 SegmentStreamDevice::writeData(const char* /*data*/, qint64 /*maxSize*/)
{
    return -1;
}

void SegmentStreamDevice::append(const QByteArray& chunk)
{
    if(chunk.isEmpty()) {
        return;
    }
    {
        const std::scoped_lock lock{m_mutex};
        m_buffer.append(chunk);
    }
    m_dataChanged.notify_all();
}

bool SegmentStreamDevice::isAborted() const
{
    const std::scoped_lock lock{m_mutex};
    return m_aborted;
}

void SegmentStreamDevice::download()
{
    BlockingHttp http;
    const auto cancelled = [this]() { return isAborted(); };

    QString error;
    for(qsizetype i{0}; i < m_urls.size() && !isAborted(); ++i) {
        const auto onData = [this](int status, const QByteArray& chunk) {
            if(status == 200) {
                append(chunk);
            }
        };
        const HttpResult result = http.getStreamed(QUrl{m_urls.at(i)}, onData, cancelled);
        if(!result.ok()) {
            error = u"Segment %1 of %2: %3"_s.arg(i + 1).arg(m_urls.size()).arg(result.describe());
            break;
        }
    }

    {
        const std::scoped_lock lock{m_mutex};
        m_finished = true;
        if(!m_aborted) {
            m_error = error;
        }
    }
    if(!error.isEmpty() && !isAborted()) {
        qCWarning(TIDAL) << "TIDAL stream download failed:" << error;
    }
    m_dataChanged.notify_all();
}
} // namespace FooyinTidal
