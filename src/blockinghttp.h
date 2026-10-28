/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include <QNetworkAccessManager>
#include <QUrlQuery>

#include <functional>

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
 * Synchronous HTTP for fooyin's decoder threads, which have no event loop of their own. Runs a
 * local event loop until the reply finishes. Create and use it on one thread only.
 */
class BlockingHttp
{
public:
    using Cancelled = std::function<bool()>;
    using Headers   = QList<std::pair<QByteArray, QByteArray>>;

    HttpResult get(const QUrl& url, const Headers& headers = {}, const Cancelled& cancelled = {});
    HttpResult postForm(const QUrl& url, const QUrlQuery& form, const Cancelled& cancelled = {});

    //! Called with the reply's HTTP status and each chunk of its body as it arrives.
    using DataHandler = std::function<void(int status, const QByteArray& chunk)>;

    //! Runs `reply` to completion. With `onData`, the body goes there instead of into the result.
    static HttpResult run(QNetworkReply* reply, const Cancelled& cancelled, const DataHandler& onData = {});

    static QNetworkRequest request(const QUrl& url, const Headers& headers = {});

    QNetworkAccessManager& network()
    {
        return m_network;
    }

private:
    QNetworkAccessManager m_network;
};
} // namespace FooyinTidal
