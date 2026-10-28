/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include <QString>

#include <expected>
#include <mutex>

namespace FooyinTidal {
class BlockingHttp;

/*!
 * The playback login: a client of one of TIDAL's own apps, used only for v1's stream info.
 * Kept in session.json in the plugin's config folder. For now it's written there by
 * tools/tidal-probe.py export-session; the plugin keeps the access token fresh.
 *
 * Thread-safe: decoders on several threads may ask for the token at once.
 */
class TidalSession
{
public:
    explicit TidalSession(QString path);

    [[nodiscard]] bool isLoggedIn() const;
    [[nodiscard]] QString countryCode() const;

    //! A valid access token, refreshing it (blocking, through `http`) if it has expired.
    std::expected<QString, QString> accessToken(BlockingHttp& http);

private:
    void load();
    void save() const;

    QString m_path;
    mutable std::mutex m_mutex;
    QString m_clientId;
    QString m_clientSecret;
    QString m_accessToken;
    QString m_refreshToken;
    QString m_countryCode;
    qint64 m_expiresAt{0};
};
} // namespace FooyinTidal
