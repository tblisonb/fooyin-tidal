/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "tidallogin.h"

#include "blockinghttp.h"
#include "tidalsession.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QUrlQuery>

using namespace Qt::StringLiterals;

namespace {
constexpr auto AuthorizeUrl     = "https://login.tidal.com/authorize";
constexpr auto TokenUrl         = "https://auth.tidal.com/v1/oauth2/token";
constexpr auto PlaybackRedirect = "https://tidal.com/android/login/auth";

QByteArray randomBytes(qsizetype count)
{
    QByteArray bytes(count, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(bytes.data()), count / 4);
    return bytes;
}

QString base64Url(const QByteArray& bytes)
{
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

FooyinTidal::TidalLogin::Request newRequest(const QString& clientId, const QString& clientSecret,
                                            const QString& redirectUri)
{
    FooyinTidal::TidalLogin::Request request;
    request.clientId     = clientId.trimmed();
    request.clientSecret = clientSecret.trimmed();
    request.redirectUri  = redirectUri;
    request.verifier     = base64Url(randomBytes(64));
    return request;
}

QString challengeFor(const QString& verifier)
{
    return base64Url(QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256));
}

std::expected<QJsonObject, QString> exchangeCode(const FooyinTidal::TidalLogin::Request& request,
                                                 const QString& code, const QString& scope,
                                                 FooyinTidal::BlockingHttp& http)
{
    QUrlQuery form;
    form.addQueryItem(u"grant_type"_s, u"authorization_code"_s);
    form.addQueryItem(u"client_id"_s, request.clientId);
    form.addQueryItem(u"code"_s, code);
    form.addQueryItem(u"redirect_uri"_s, request.redirectUri);
    form.addQueryItem(u"code_verifier"_s, request.verifier);
    if(!scope.isEmpty()) {
        form.addQueryItem(u"scope"_s, scope);
    }
    if(!request.uniqueKey.isEmpty()) {
        form.addQueryItem(u"client_unique_key"_s, request.uniqueKey);
    }

    const FooyinTidal::HttpResult result = http.postForm(QUrl{QString::fromLatin1(TokenUrl)}, form);
    if(!result.ok()) {
        return std::unexpected(u"TIDAL didn't accept the login: %1"_s.arg(result.describe()));
    }
    const QJsonObject token = QJsonDocument::fromJson(result.body).object();
    if(token.value("access_token"_L1).toString().isEmpty()) {
        return std::unexpected(u"TIDAL's token reply had no access token"_s);
    }
    return token;
}

FooyinTidal::BlockingHttp::Headers bearer(const QJsonObject& token, const QByteArray& accept = {})
{
    FooyinTidal::BlockingHttp::Headers headers{
        {"Authorization", "Bearer " + token.value("access_token"_L1).toString().toUtf8()}};
    if(!accept.isEmpty()) {
        headers.emplace_back("Accept", accept);
    }
    return headers;
}
} // namespace

namespace FooyinTidal::TidalLogin {
QString accountRedirectUri()
{
    return u"http://localhost:%1/callback"_s.arg(AccountRedirectPort);
}

QString accountScopes()
{
    return u"user.read collection.read collection.write playlists.read playlists.write search.read playback"_s;
}

QString playbackScopes()
{
    return u"r_usr w_usr w_sub"_s;
}

Request accountRequest(const QString& clientId)
{
    Request request = newRequest(clientId, {}, accountRedirectUri());
    request.state   = base64Url(randomBytes(16));

    QUrlQuery query;
    query.addQueryItem(u"response_type"_s, u"code"_s);
    query.addQueryItem(u"client_id"_s, request.clientId);
    query.addQueryItem(u"redirect_uri"_s, request.redirectUri);
    query.addQueryItem(u"scope"_s, accountScopes());
    query.addQueryItem(u"code_challenge_method"_s, u"S256"_s);
    query.addQueryItem(u"code_challenge"_s, challengeFor(request.verifier));
    query.addQueryItem(u"state"_s, request.state);

    request.authorizeUrl = QUrl{QString::fromLatin1(AuthorizeUrl)};
    request.authorizeUrl.setQuery(query);
    return request;
}

Request playbackRequest(const QString& clientId, const QString& clientSecret)
{
    Request request   = newRequest(clientId, clientSecret, QString::fromLatin1(PlaybackRedirect));
    request.uniqueKey = QString::fromLatin1(randomBytes(8).toHex());

    QUrlQuery query;
    query.addQueryItem(u"response_type"_s, u"code"_s);
    query.addQueryItem(u"redirect_uri"_s, request.redirectUri);
    query.addQueryItem(u"client_id"_s, request.clientId);
    query.addQueryItem(u"lang"_s, u"EN"_s);
    query.addQueryItem(u"appMode"_s, u"android"_s);
    query.addQueryItem(u"client_unique_key"_s, request.uniqueKey);
    query.addQueryItem(u"code_challenge"_s, challengeFor(request.verifier));
    query.addQueryItem(u"code_challenge_method"_s, u"S256"_s);
    query.addQueryItem(u"restrict_signup"_s, u"true"_s);

    request.authorizeUrl = QUrl{QString::fromLatin1(AuthorizeUrl)};
    request.authorizeUrl.setQuery(query);
    return request;
}

std::expected<QString, QString> codeFrom(const Request& request, const QUrl& redirect)
{
    const QUrlQuery query{redirect};
    if(query.hasQueryItem(u"error"_s)) {
        return std::unexpected(u"TIDAL refused the login: %1 %2"_s.arg(
            query.queryItemValue(u"error"_s), query.queryItemValue(u"error_description"_s, QUrl::FullyDecoded)));
    }
    const QString code = query.queryItemValue(u"code"_s, QUrl::FullyDecoded);
    if(code.isEmpty()) {
        return std::unexpected(u"There's no login code in that address"_s);
    }
    if(!request.state.isEmpty() && query.queryItemValue(u"state"_s, QUrl::FullyDecoded) != request.state) {
        return std::unexpected(u"The reply doesn't belong to this login"_s);
    }
    return code;
}

std::expected<void, QString> completeAccount(const Request& request, const QString& code, TidalSession& session)
{
    BlockingHttp http;
    const auto token = exchangeCode(request, code, {}, http);
    if(!token) {
        return std::unexpected(token.error());
    }

    const HttpResult me = http.get(QUrl{u"https://openapi.tidal.com/v2/users/me"_s},
                                   bearer(*token, "application/vnd.api+json"));
    if(!me.ok()) {
        return std::unexpected(u"Couldn't read the account's details: %1"_s.arg(me.describe()));
    }
    const QJsonObject user = QJsonDocument::fromJson(me.body).object().value("data"_L1).toObject();

    QJsonObject stored = *token;
    if(const QString userId = user.value("id"_L1).toString(); !userId.isEmpty()) {
        stored["user_id"_L1] = userId;
    }
    session.setLogin(request.clientId, {}, stored,
                     user.value("attributes"_L1).toObject().value("country"_L1).toString());
    return {};
}

std::expected<void, QString> completePlayback(const Request& request, const QString& code, TidalSession& session)
{
    BlockingHttp http;
    const auto token = exchangeCode(request, code, playbackScopes(), http);
    if(!token) {
        return std::unexpected(token.error());
    }

    const HttpResult sessionInfo = http.get(QUrl{u"https://api.tidal.com/v1/sessions"_s}, bearer(*token));
    if(!sessionInfo.ok()) {
        return std::unexpected(u"Couldn't read the login's details: %1"_s.arg(sessionInfo.describe()));
    }
    const QJsonObject info = QJsonDocument::fromJson(sessionInfo.body).object();

    session.setLogin(request.clientId, request.clientSecret, *token, info.value("countryCode"_L1).toString());
    return {};
}
} // namespace FooyinTidal::TidalLogin
