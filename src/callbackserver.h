/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include <QObject>
#include <QUrl>

class QTcpServer;
class QTcpSocket;

namespace FooyinTidal {
/*!
 * Waits on a localhost port for the browser to come back from a login, and hands over the
 * address it came back to. Answers other requests (such as the browser's favicon) with a 404 and
 * keeps waiting.
 */
class CallbackServer : public QObject
{
    Q_OBJECT

public:
    explicit CallbackServer(QObject* parent = nullptr);

    //! Starts listening; false (see errorString()) if the port is taken.
    bool listen(quint16 port, const QString& path);
    void close();
    [[nodiscard]] QString errorString() const;

Q_SIGNALS:
    void redirected(const QUrl& url);

private:
    void handle(QTcpSocket* socket);
    static void respond(QTcpSocket* socket, const QByteArray& status, const QString& message);

    QTcpServer* m_server;
    QString m_path;
};
} // namespace FooyinTidal
