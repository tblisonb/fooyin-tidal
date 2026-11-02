/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "blockinghttp.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QThread>

#include <chrono>
#include <condition_variable>
#include <mutex>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {
constexpr int TransferTimeoutMs = 20000;
constexpr auto CancelPoll       = 100ms;

//! One request in flight, shared between the waiting thread and the network thread.
struct Pending
{
    std::mutex mutex;
    std::condition_variable done;
    bool finished{false};
    FooyinTidal::HttpResult result;
    QPointer<QNetworkReply> reply;
};
} // namespace

namespace FooyinTidal {
QString HttpResult::describe() const
{
    if(!error.isEmpty()) {
        return error;
    }
    return u"HTTP %1: %2"_s.arg(status).arg(QString::fromUtf8(body.left(300)));
}

BlockingHttp::BlockingHttp()
    : m_thread{std::make_unique<QThread>()}
    , m_network{new QNetworkAccessManager}
{
    m_thread->setObjectName(u"TIDAL network"_s);
    m_network->moveToThread(m_thread.get());
    m_thread->start();
}

BlockingHttp::~BlockingHttp()
{
    // Deleted on its own thread, with any replies still parented to it.
    QMetaObject::invokeMethod(m_network, [network = m_network]() { delete network; }, Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();
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
    const QNetworkRequest req = request(url, headers);
    return run([req](QNetworkAccessManager& network) { return network.get(req); }, cancelled);
}

HttpResult BlockingHttp::postForm(const QUrl& url, const QUrlQuery& form, const Cancelled& cancelled)
{
    QNetworkRequest req = request(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-www-form-urlencoded"_s);
    const QByteArray body = form.toString(QUrl::FullyEncoded).toUtf8();
    return run([req, body](QNetworkAccessManager& network) { return network.post(req, body); }, cancelled);
}

HttpResult BlockingHttp::send(const QByteArray& method, const QUrl& url, const Headers& headers,
                              const QByteArray& body, const Cancelled& cancelled)
{
    const QNetworkRequest req = request(url, headers);
    return run([req, method, body](QNetworkAccessManager& network) {
        return network.sendCustomRequest(req, method, body);
    }, cancelled);
}

HttpResult BlockingHttp::getStreamed(const QUrl& url, const DataHandler& onData, const Cancelled& cancelled)
{
    const QNetworkRequest req = request(url);
    return run([req](QNetworkAccessManager& network) { return network.get(req); }, cancelled, onData);
}

HttpResult BlockingHttp::run(const Send& send, const Cancelled& cancelled, const DataHandler& onData)
{
    auto pending = std::make_shared<Pending>();

    // `send` and `onData` are used only until `finished` is set, while this thread still waits.
    QMetaObject::invokeMethod(
        m_network,
        [network = m_network, pending, &send, &onData]() {
            QNetworkReply* reply = send(*network);
            {
                const std::scoped_lock lock{pending->mutex};
                pending->reply = reply;
            }
            const auto status = [reply]() {
                return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            };
            if(onData) {
                QObject::connect(reply, &QNetworkReply::readyRead, reply,
                                 [reply, &onData, status]() { onData(status(), reply->readAll()); });
            }
            QObject::connect(reply, &QNetworkReply::finished, reply, [reply, pending, &onData, status]() {
                HttpResult result;
                result.status = status();
                if(onData) {
                    onData(result.status, reply->readAll());
                }
                else {
                    result.body = reply->readAll();
                }
                if(result.status == 0 && reply->error() != QNetworkReply::NoError) {
                    result.error = reply->errorString();
                }
                reply->deleteLater();
                {
                    const std::scoped_lock lock{pending->mutex};
                    pending->result   = std::move(result);
                    pending->finished = true;
                }
                pending->done.notify_all();
            });
        },
        Qt::QueuedConnection);

    bool wasCancelled{false};
    std::unique_lock lock{pending->mutex};
    while(!pending->finished) {
        pending->done.wait_for(lock, CancelPoll);
        if(!pending->finished && !wasCancelled && cancelled && cancelled()) {
            wasCancelled = true;
            QMetaObject::invokeMethod(
                m_network,
                [pending]() {
                    // Still on the network thread's queue: the reply is set before this runs.
                    if(QNetworkReply* reply = pending->reply) {
                        reply->abort();
                    }
                },
                Qt::QueuedConnection);
        }
    }

    HttpResult result = std::move(pending->result);
    if(wasCancelled) {
        result.error = u"Cancelled"_s;
    }
    return result;
}
} // namespace FooyinTidal
