/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "tidalsession.h"

#include "blockinghttp.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>

Q_DECLARE_LOGGING_CATEGORY(TIDAL)

using namespace Qt::StringLiterals;

namespace {
constexpr auto TokenUrl = "https://auth.tidal.com/v1/oauth2/token";
// Refresh a minute early so a token doesn't expire between handing it out and using it.
constexpr qint64 ExpiryMarginSecs = 60;
} // namespace

namespace FooyinTidal {
TidalSession::TidalSession(QString path, QString refreshScope)
    : m_path{std::move(path)}
    , m_refreshScope{std::move(refreshScope)}
{
    load();
}

bool TidalSession::isLoggedIn() const
{
    const std::scoped_lock lock{m_mutex};
    return !m_clientId.isEmpty() && !m_refreshToken.isEmpty();
}

QString TidalSession::countryCode() const
{
    const std::scoped_lock lock{m_mutex};
    return m_countryCode.isEmpty() ? u"US"_s : m_countryCode;
}

QString TidalSession::userId() const
{
    const std::scoped_lock lock{m_mutex};
    return m_userId;
}

QString TidalSession::clientId() const
{
    const std::scoped_lock lock{m_mutex};
    return m_clientId;
}

QString TidalSession::clientSecret() const
{
    const std::scoped_lock lock{m_mutex};
    return m_clientSecret;
}

void TidalSession::setLogin(const QString& clientId, const QString& clientSecret, const QJsonObject& token,
                            const QString& countryCode)
{
    const std::scoped_lock lock{m_mutex};
    m_clientId     = clientId;
    m_clientSecret = clientSecret;
    m_countryCode  = countryCode;
    m_refreshToken.clear();
    m_userId.clear();
    storeToken(token);
    save();
}

void TidalSession::logOut()
{
    const std::scoped_lock lock{m_mutex};
    m_accessToken.clear();
    m_refreshToken.clear();
    m_userId.clear();
    m_expiresAt = 0;
    save();
}

void TidalSession::storeToken(const QJsonObject& token)
{
    m_accessToken = token.value("access_token"_L1).toString();
    m_expiresAt   = QDateTime::currentSecsSinceEpoch() + token.value("expires_in"_L1).toInteger(3600);
    if(const QString refresh = token.value("refresh_token"_L1).toString(); !refresh.isEmpty()) {
        m_refreshToken = refresh;
    }
    if(const QJsonValue user = token.value("user_id"_L1); !user.isUndefined() && !user.isNull()) {
        m_userId = user.isString() ? user.toString() : QString::number(user.toInteger());
    }
}

std::expected<QString, QString> TidalSession::accessToken(BlockingHttp& http)
{
    const std::scoped_lock lock{m_mutex};

    if(m_clientId.isEmpty() || m_refreshToken.isEmpty()) {
        return std::unexpected(u"Not logged in to TIDAL for playback"_s);
    }

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if(!m_accessToken.isEmpty() && now < m_expiresAt - ExpiryMarginSecs) {
        return m_accessToken;
    }

    QUrlQuery form;
    form.addQueryItem(u"grant_type"_s, u"refresh_token"_s);
    form.addQueryItem(u"client_id"_s, m_clientId);
    form.addQueryItem(u"refresh_token"_s, m_refreshToken);
    if(!m_refreshScope.isEmpty()) {
        form.addQueryItem(u"scope"_s, m_refreshScope);
    }
    if(!m_clientSecret.isEmpty()) {
        form.addQueryItem(u"client_secret"_s, m_clientSecret);
    }

    const HttpResult result = http.postForm(QUrl{QString::fromLatin1(TokenUrl)}, form);
    if(!result.ok()) {
        return std::unexpected(u"Couldn't refresh the TIDAL login: %1"_s.arg(result.describe()));
    }

    storeToken(QJsonDocument::fromJson(result.body).object());
    if(m_accessToken.isEmpty()) {
        return std::unexpected(u"TIDAL's token reply had no access token"_s);
    }

    save();
    qCInfo(TIDAL) << "Refreshed the TIDAL login in" << QFileInfo{m_path}.fileName();
    return m_accessToken;
}

void TidalSession::load()
{
    QFile file{m_path};
    if(!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    m_clientId             = json.value("client_id"_L1).toString();
    m_clientSecret         = json.value("client_secret"_L1).toString();
    m_accessToken          = json.value("access_token"_L1).toString();
    m_refreshToken         = json.value("refresh_token"_L1).toString();
    m_countryCode          = json.value("country"_L1).toString();
    m_userId               = json.value("user_id"_L1).toString();
    m_expiresAt            = static_cast<qint64>(json.value("expires_at"_L1).toDouble());
}

void TidalSession::save() const
{
    QJsonObject json;
    json["client_id"_L1]     = m_clientId;
    json["client_secret"_L1] = m_clientSecret;
    json["access_token"_L1]  = m_accessToken;
    json["refresh_token"_L1] = m_refreshToken;
    json["country"_L1]       = m_countryCode;
    json["user_id"_L1]       = m_userId;
    json["expires_at"_L1]    = static_cast<double>(m_expiresAt);

    QDir{}.mkpath(QFileInfo{m_path}.path());
    QSaveFile file{m_path};
    if(!file.open(QIODevice::WriteOnly)) {
        qCWarning(TIDAL) << "Couldn't save the TIDAL login to" << m_path;
        return;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument{json}.toJson());
    file.commit();
}
} // namespace FooyinTidal
