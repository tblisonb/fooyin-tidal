/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include <QNetworkRequest>
#include <QUrlQuery>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QThread;

namespace FooyinTidal {
struct HttpResult
{
    int status{0};
    QByteArray body;
    //! Network-level failure (no reply, timeout, cancelled); empty when the server answered.
    QString error;

    [[nodiscard]] bool ok() const
    {
        return error.isEmpty() && status >= 200 && status < 300;
    }
    //! A one-line description of what went wrong, for logs and messages.
    [[nodiscard]] QString describe() const;
};

/*!
 * Synchronous HTTP for threads that mustn't run an event loop of their own, such as fooyin's audio
 * engine thread: a local event loop there would also deliver fooyin's queued engine commands in
 * the middle of loading a track. Requests run on a network thread that each instance owns, and the
 * calling thread just waits. Use an instance from one thread at a time.
 */
class BlockingHttp
{
public:
    using Cancelled = std::function<bool()>;
    using Headers   = QList<std::pair<QByteArray, QByteArray>>;
    //! Called on the network thread with the reply's HTTP status and each chunk of its body.
    using DataHandler = std::function<void(int status, const QByteArray& chunk)>;

    BlockingHttp();
    ~BlockingHttp();
    BlockingHttp(const BlockingHttp&)            = delete;
    BlockingHttp& operator=(const BlockingHttp&) = delete;

    HttpResult get(const QUrl& url, const Headers& headers = {}, const Cancelled& cancelled = {});
    HttpResult postForm(const QUrl& url, const QUrlQuery& form, const Cancelled& cancelled = {});
    //! Any method (POST, PATCH, DELETE…) with a body, which may be empty.
    HttpResult send(const QByteArray& method, const QUrl& url, const Headers& headers, const QByteArray& body,
                    const Cancelled& cancelled = {});
    //! A GET whose body goes to `onData` as it arrives instead of into the result.
    HttpResult getStreamed(const QUrl& url, const DataHandler& onData, const Cancelled& cancelled = {});

private:
    using Send = std::function<QNetworkReply*(QNetworkAccessManager&)>;
    HttpResult run(const Send& send, const Cancelled& cancelled, const DataHandler& onData = {});

    static QNetworkRequest request(const QUrl& url, const Headers& headers = {});

    std::unique_ptr<QThread> m_thread;
    QNetworkAccessManager* m_network;
};
} // namespace FooyinTidal
