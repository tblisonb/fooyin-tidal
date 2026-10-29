/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "tidalsettingspage.h"

#include "callbackserver.h"
#include "tidallogin.h"
#include "tidalsession.h"

#include <QDesktopServices>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QPushButton>
#include <QtConcurrentRun>

#include <optional>

Q_DECLARE_LOGGING_CATEGORY(TIDAL)

using namespace Qt::StringLiterals;

namespace FooyinTidal {
namespace {
using LoginResult = std::expected<void, QString>;

//! The widgets and state of one login.
struct LoginBox
{
    std::shared_ptr<TidalSession> session;
    QGroupBox* group{nullptr};
    QLineEdit* clientId{nullptr};
    QLineEdit* clientSecret{nullptr};
    QLabel* status{nullptr};
    QPushButton* logIn{nullptr};
    QPushButton* logOut{nullptr};
    //! The login waiting on the browser, if any.
    std::optional<TidalLogin::Request> pending;
    bool completing{false};
};

QLabel* hint(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setOpenExternalLinks(true);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    return label;
}
} // namespace

class TidalSettingsPageWidget : public Fooyin::SettingsPageWidget
{
    Q_OBJECT

public:
    TidalSettingsPageWidget(std::shared_ptr<TidalSession> account, std::shared_ptr<TidalSession> playback);

    void load() override;
    void apply() override { }
    void reset() override { }

private:
    void setupBox(LoginBox& box, const QString& title, const QString& explanation, bool withSecret);
    void updateBox(LoginBox& box, const QString& message = {});

    void startAccountLogin();
    void startPlaybackLogin();
    void finishPlaybackLogin();
    void complete(LoginBox& box, const QString& code);
    void logOut(LoginBox& box);

    LoginBox m_account;
    LoginBox m_playback;
    CallbackServer* m_callback;
    QLineEdit* m_redirected;
    QPushButton* m_finish;
};

TidalSettingsPageWidget::TidalSettingsPageWidget(std::shared_ptr<TidalSession> account,
                                                 std::shared_ptr<TidalSession> playback)
    : m_callback{new CallbackServer(this)}
    , m_redirected{new QLineEdit(this)}
    , m_finish{new QPushButton(tr("&Finish Login"), this)}
{
    m_account.session  = std::move(account);
    m_playback.session = std::move(playback);

    setupBox(m_account, tr("TIDAL Account"),
             tr("For searching TIDAL and for your favorites and playlists. Create an app on "
                "<a href=\"https://developer.tidal.com/dashboard\">TIDAL's developer dashboard</a>, give it the "
                "redirect URI <code>%1</code> and enable these scopes: <code>%2</code>. Then enter its client "
                "ID here and log in in the browser.")
                 .arg(TidalLogin::accountRedirectUri(), TidalLogin::accountScopes()),
             false);
    setupBox(m_playback, tr("Playback"),
             tr("TIDAL's official API plays only 30-second previews to developer apps, so full tracks come "
                "through TIDAL's private API, logged in as one of TIDAL's own apps. Enter that app's client ID "
                "(and secret, if it has one) and log in. The browser ends on an \"Oops\" page: copy that "
                "page's whole address into the box below. This is unofficial, and TIDAL may stop it working."),
             true);

    m_redirected->setPlaceholderText(tr("Address of the \"Oops\" page"));
    auto* playbackLayout = qobject_cast<QGridLayout*>(m_playback.group->layout());
    const int row        = playbackLayout->rowCount();
    playbackLayout->addWidget(m_redirected, row, 0, 1, 3);
    playbackLayout->addWidget(m_finish, row, 3);

    auto* layout = new QGridLayout(this);
    layout->addWidget(m_account.group, 0, 0);
    layout->addWidget(m_playback.group, 1, 0);
    layout->setRowStretch(2, 1);

    QObject::connect(m_account.logIn, &QPushButton::clicked, this, &TidalSettingsPageWidget::startAccountLogin);
    QObject::connect(m_account.logOut, &QPushButton::clicked, this, [this]() { logOut(m_account); });
    QObject::connect(m_playback.logIn, &QPushButton::clicked, this, &TidalSettingsPageWidget::startPlaybackLogin);
    QObject::connect(m_playback.logOut, &QPushButton::clicked, this, [this]() { logOut(m_playback); });
    QObject::connect(m_finish, &QPushButton::clicked, this, &TidalSettingsPageWidget::finishPlaybackLogin);
    QObject::connect(m_redirected, &QLineEdit::returnPressed, this, &TidalSettingsPageWidget::finishPlaybackLogin);

    QObject::connect(m_callback, &CallbackServer::redirected, this, [this](const QUrl& url) {
        if(!m_account.pending) {
            return;
        }
        const auto code = TidalLogin::codeFrom(*m_account.pending, url);
        if(!code) {
            m_account.pending.reset();
            updateBox(m_account, code.error());
            return;
        }
        complete(m_account, *code);
    });
}

void TidalSettingsPageWidget::setupBox(LoginBox& box, const QString& title, const QString& explanation,
                                       bool withSecret)
{
    box.group    = new QGroupBox(title, this);
    box.clientId = new QLineEdit(box.group);
    box.status   = new QLabel(box.group);
    box.logIn    = new QPushButton(tr("Log &In"), box.group);
    box.logOut   = new QPushButton(tr("Log &Out"), box.group);
    box.status->setWordWrap(true);

    auto* layout = new QGridLayout(box.group);
    int row{0};
    layout->addWidget(hint(explanation, box.group), row++, 0, 1, 4);
    layout->addWidget(new QLabel(tr("Client ID:"), box.group), row, 0);
    layout->addWidget(box.clientId, row++, 1, 1, 3);
    if(withSecret) {
        box.clientSecret = new QLineEdit(box.group);
        box.clientSecret->setEchoMode(QLineEdit::Password);
        box.clientSecret->setPlaceholderText(tr("None"));
        layout->addWidget(new QLabel(tr("Client secret:"), box.group), row, 0);
        layout->addWidget(box.clientSecret, row++, 1, 1, 3);
    }
    layout->addWidget(box.status, row, 0, 1, 2);
    layout->addWidget(box.logIn, row, 2);
    layout->addWidget(box.logOut, row++, 3);
    layout->setColumnStretch(1, 1);
}

void TidalSettingsPageWidget::load()
{
    for(LoginBox* box : {&m_account, &m_playback}) {
        box->clientId->setText(box->session->clientId());
        if(box->clientSecret) {
            box->clientSecret->setText(box->session->clientSecret());
        }
        updateBox(*box);
    }
}

void TidalSettingsPageWidget::updateBox(LoginBox& box, const QString& message)
{
    const bool loggedIn = box.session->isLoggedIn();
    const bool busy     = box.pending || box.completing;

    QString status = message;
    if(status.isEmpty()) {
        if(box.completing) {
            status = tr("Logging in…");
        }
        else if(box.pending) {
            status = tr("Waiting for the login in the browser…");
        }
        else if(loggedIn) {
            status = tr("Logged in (country %1).").arg(box.session->countryCode());
        }
        else {
            status = tr("Not logged in.");
        }
    }
    box.status->setText(status);

    box.clientId->setEnabled(!busy);
    if(box.clientSecret) {
        box.clientSecret->setEnabled(!busy);
    }
    box.logIn->setEnabled(!box.completing);
    box.logIn->setText(box.pending ? tr("Log &In Again") : tr("Log &In"));
    box.logOut->setEnabled(loggedIn && !busy);

    if(&box == &m_playback) {
        m_redirected->setEnabled(box.pending && !box.completing);
        m_finish->setEnabled(box.pending && !box.completing);
    }
}

void TidalSettingsPageWidget::startAccountLogin()
{
    const QString clientId = m_account.clientId->text().trimmed();
    if(clientId.isEmpty()) {
        updateBox(m_account, tr("Enter your app's client ID first."));
        return;
    }
    if(!m_callback->listen(TidalLogin::AccountRedirectPort, u"/callback"_s)) {
        updateBox(m_account, tr("Couldn't wait for the browser on port %1: %2")
                                 .arg(TidalLogin::AccountRedirectPort)
                                 .arg(m_callback->errorString()));
        return;
    }

    m_account.pending = TidalLogin::accountRequest(clientId);
    QDesktopServices::openUrl(m_account.pending->authorizeUrl);
    updateBox(m_account);
}

void TidalSettingsPageWidget::startPlaybackLogin()
{
    const QString clientId = m_playback.clientId->text().trimmed();
    if(clientId.isEmpty()) {
        updateBox(m_playback, tr("Enter the client ID first."));
        return;
    }

    m_playback.pending = TidalLogin::playbackRequest(clientId, m_playback.clientSecret->text());
    m_redirected->clear();
    QDesktopServices::openUrl(m_playback.pending->authorizeUrl);
    updateBox(m_playback);
    m_redirected->setFocus();
}

void TidalSettingsPageWidget::finishPlaybackLogin()
{
    if(!m_playback.pending || m_playback.completing) {
        return;
    }
    const auto code = TidalLogin::codeFrom(*m_playback.pending, QUrl::fromUserInput(m_redirected->text().trimmed()));
    if(!code) {
        // Leave the login waiting, so the right address can still be pasted.
        m_playback.status->setText(code.error());
        return;
    }
    complete(m_playback, *code);
}

void TidalSettingsPageWidget::complete(LoginBox& box, const QString& code)
{
    box.completing = true;
    updateBox(box);

    const TidalLogin::Request request = *box.pending;
    const bool isAccount              = &box == &m_account;
    auto* watcher                     = new QFutureWatcher<LoginResult>(this);
    QObject::connect(watcher, &QFutureWatcher<LoginResult>::finished, this, [this, watcher, &box]() {
        const LoginResult result = watcher->result();
        watcher->deleteLater();
        box.completing = false;
        box.pending.reset();
        if(&box == &m_playback) {
            m_redirected->clear();
        }
        if(!result) {
            qCWarning(TIDAL) << "TIDAL login failed:" << result.error();
            updateBox(box, result.error());
            return;
        }
        qCInfo(TIDAL) << (&box == &m_account ? "Logged in to the TIDAL account" : "Logged in to TIDAL for playback");
        updateBox(box);
    });
    watcher->setFuture(QtConcurrent::run([session = box.session, request, code, isAccount]() -> LoginResult {
        return isAccount ? TidalLogin::completeAccount(request, code, *session)
                         : TidalLogin::completePlayback(request, code, *session);
    }));
}

void TidalSettingsPageWidget::logOut(LoginBox& box)
{
    box.session->logOut();
    updateBox(box);
}

TidalSettingsPage::TidalSettingsPage(Fooyin::SettingsDialogController* controller,
                                     std::shared_ptr<TidalSession> account, std::shared_ptr<TidalSession> playback,
                                     QObject* parent)
    : Fooyin::SettingsPage{controller, parent}
{
    setId(SettingsPageId);
    setName(tr("TIDAL"));
    setCategory({tr("Integrations"), tr("TIDAL")});
    setWidgetCreator([account, playback]() { return new TidalSettingsPageWidget(account, playback); });
}
} // namespace FooyinTidal

#include "moc_tidalsettingspage.cpp"
#include "tidalsettingspage.moc"
