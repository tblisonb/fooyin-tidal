/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "callbackserver.h"

#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>

using namespace Qt::StringLiterals;

namespace {
// A request line and headers; a browser's are well under this.
constexpr qsizetype MaxRequestBytes = 16 * 1024;
} // namespace

namespace FooyinTidal {
CallbackServer::CallbackServer(QObject* parent)
    : QObject{parent}
    , m_server{new QTcpServer(this)}
{
    m_server->setProxy(QNetworkProxy::NoProxy);
    QObject::connect(m_server, &QTcpServer::newConnection, this, [this]() {
        while(QTcpSocket* socket = m_server->nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { handle(socket); });
        }
    });
}

bool CallbackServer::listen(quint16 port, const QString& path)
{
    m_path = path;
    close();
    return m_server->listen(QHostAddress::LocalHost, port);
}

void CallbackServer::close()
{
    m_server->close();
}

QString CallbackServer::errorString() const
{
    return m_server->errorString();
}

void CallbackServer::handle(QTcpSocket* socket)
{
    // Only the request line matters, but wait for the end of the headers before answering.
    const QByteArray data = socket->peek(MaxRequestBytes);
    if(!data.contains("\r\n\r\n") && data.size() < MaxRequestBytes) {
        return;
    }
    const QList<QByteArray> requestLine = data.left(data.indexOf("\r\n")).split(' ');
    socket->readAll();

    const QUrl url{QString::fromLatin1(requestLine.value(1))};
    if(requestLine.value(0) != "GET" || url.path() != m_path) {
        respond(socket, "404 Not Found", tr("Not found."));
        return;
    }

    respond(socket, "200 OK", tr("Back to fooyin: you can close this tab."));
    close();
    Q_EMIT redirected(url);
}

void CallbackServer::respond(QTcpSocket* socket, const QByteArray& status, const QString& message)
{
    const QByteArray body = u"<!doctype html><meta charset=utf-8><title>fooyin TIDAL login</title>"
                            "<p style='font-family:sans-serif;text-align:center;margin-top:3em'>%1</p>"_s.arg(
                                message.toHtmlEscaped())
                                .toUtf8();
    socket->write("HTTP/1.1 " + status + "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
                  + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    socket->disconnectFromHost();
}
} // namespace FooyinTidal
