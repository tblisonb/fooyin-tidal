/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "blockinghttp.h"

#include <QEventLoop>
#include <QNetworkReply>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace {
constexpr int TransferTimeoutMs = 20000;
constexpr int CancelPollMs      = 100;
} // namespace

namespace FooyinTidal {
QString HttpResult::describe() const
{
    if(!error.isEmpty()) {
        return error;
    }
    return u"HTTP %1: %2"_s.arg(status).arg(QString::fromUtf8(body.left(300)));
}

QNetworkRequest BlockingHttp::request(const QUrl& url, const Headers& headers)
{
    QNetworkRequest req{url};
    req.setTransferTimeout(TransferTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    for(const auto& [name, value] : headers) {
        req.setRawHeader(name, value);
    }
    return req;
}

HttpResult BlockingHttp::get(const QUrl& url, const Headers& headers, const Cancelled& cancelled)
{
    return run(m_network.get(request(url, headers)), cancelled);
}

HttpResult BlockingHttp::postForm(const QUrl& url, const QUrlQuery& form, const Cancelled& cancelled)
{
    QNetworkRequest req = request(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-www-form-urlencoded"_s);
    return run(m_network.post(req, form.toString(QUrl::FullyEncoded).toUtf8()), cancelled);
}

HttpResult BlockingHttp::run(QNetworkReply* reply, const Cancelled& cancelled, const DataHandler& onData)
{
    const auto status = [reply]() {
        return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    };

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if(onData) {
        QObject::connect(reply, &QNetworkReply::readyRead, &loop,
                         [reply, &onData, &status]() { onData(status(), reply->readAll()); });
    }

    QTimer cancelPoll;
    bool wasCancelled{false};
    if(cancelled) {
        cancelPoll.setInterval(CancelPollMs);
        QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&]() {
            if(cancelled()) {
                wasCancelled = true;
                reply->abort();
            }
        });
        cancelPoll.start();
    }

    if(!reply->isFinished()) {
        loop.exec();
    }

    HttpResult result;
    result.status = status();
    if(onData) {
        onData(result.status, reply->readAll());
    }
    else {
        result.body = reply->readAll();
    }
    if(wasCancelled) {
        result.error = u"Cancelled"_s;
    }
    else if(result.status == 0 && reply->error() != QNetworkReply::NoError) {
        result.error = reply->errorString();
    }
    reply->deleteLater();
    return result;
}
} // namespace FooyinTidal
