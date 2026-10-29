/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include <QJsonObject>
#include <QString>

#include <expected>
#include <mutex>

namespace FooyinTidal {
class BlockingHttp;

/*!
 * One TIDAL login, kept in a JSON file in the plugin's config folder; keeps its access token fresh.
 * The plugin has two: the account login (a developer app, for the official API) in account.json,
 * and the playback login (a client of one of TIDAL's own apps, for v1's stream info) in
 * session.json. TidalLogin's functions log in.
 *
 * Thread-safe: decoders on several threads may ask for the token at once.
 */
class TidalSession
{
public:
    //! `refreshScope` is sent when refreshing the token, if not empty.
    TidalSession(QString path, QString refreshScope);

    [[nodiscard]] bool isLoggedIn() const;
    [[nodiscard]] QString countryCode() const;
    [[nodiscard]] QString userId() const;
    [[nodiscard]] QString clientId() const;
    [[nodiscard]] QString clientSecret() const;

    //! Stores a new login from a token reply and saves it.
    void setLogin(const QString& clientId, const QString& clientSecret, const QJsonObject& token,
                  const QString& countryCode);
    //! Forgets the tokens, keeping the client, and saves.
    void logOut();

    //! A valid access token, refreshing it (blocking, through `http`) if it has expired.
    std::expected<QString, QString> accessToken(BlockingHttp& http);

private:
    void load();
    void save() const;

    void storeToken(const QJsonObject& token);

    QString m_path;
    QString m_refreshScope;
    mutable std::mutex m_mutex;
    QString m_clientId;
    QString m_clientSecret;
    QString m_accessToken;
    QString m_refreshToken;
    QString m_countryCode;
    QString m_userId;
    qint64 m_expiresAt{0};
};
} // namespace FooyinTidal
