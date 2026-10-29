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
#include <QUrl>

#include <expected>

namespace FooyinTidal {
class TidalSession;

namespace TidalLogin {
//! The account login's redirect: a developer app must list exactly this.
constexpr auto AccountRedirectPort = 47130;
QString accountRedirectUri();
//! The scopes the account login asks for; the developer app must have all of them enabled.
QString accountScopes();
//! The playback login's scopes, also sent when refreshing it.
QString playbackScopes();

//! One login in progress: PKCE details, and the page to open in the browser.
struct Request
{
    QString clientId;
    QString clientSecret;
    QString redirectUri;
    QString verifier;
    QString state;
    QString uniqueKey;
    QUrl authorizeUrl;
};

//! The official API's login, for a developer app: the browser comes back to accountRedirectUri().
Request accountRequest(const QString& clientId);
//! The private API's login, as TIDAL's Android app does it: the browser ends on an "Oops" page
//! whose address holds the code.
Request playbackRequest(const QString& clientId, const QString& clientSecret);

//! The `code` in a redirect's query, checking `state` if the request has one.
std::expected<QString, QString> codeFrom(const Request& request, const QUrl& redirect);

//! Exchange the code for tokens, find the account's country, and store the login in `session`.
//! Blocking; run off the GUI thread.
std::expected<void, QString> completeAccount(const Request& request, const QString& code, TidalSession& session);
std::expected<void, QString> completePlayback(const Request& request, const QString& code, TidalSession& session);
} // namespace TidalLogin
} // namespace FooyinTidal
