/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "tidalplugin.h"

#include "tidalapi.h"
#include "tidalbrowser.h"
#include "tidalcache.h"
#include "tidaldecoder.h"
#include "tidallogin.h"
#include "tidalsession.h"
#include "tidalsettingspage.h"

#include <core/engine/audioloader.h>
#include <core/plugins/coreplugincontext.h>
#include <gui/guiconstants.h>
#include <gui/playlist/playlistinteractor.h>
#include <gui/plugins/guiplugincontext.h>
#include <gui/widgetprovider.h>
#include <utils/actions/actioncontainer.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/fypaths.h>
#include <utils/settings/settingsdialogcontroller.h>
#include <utils/settings/settingsmanager.h>
#include <utils/utils.h>

#include <QAction>
#include <QDialog>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QtConcurrentRun>

Q_LOGGING_CATEGORY(TIDAL, "fooyin.tidal")

using namespace Qt::StringLiterals;

namespace FooyinTidal {
void TidalPlugin::initialise(const Fooyin::CorePluginContext& context)
{
    m_audioLoader = context.audioLoader;
    m_settings    = context.settingsManager;

    // In the Flatpak this is ~/.var/app/org.fooyin.fooyin/config/fooyin/tidal.
    const QString dir = Fooyin::Utils::configPath(u"tidal"_s);
    m_account         = std::make_shared<TidalSession>(dir + u"/account.json"_s, QString{});
    m_session         = std::make_shared<TidalSession>(dir + u"/session.json"_s, TidalLogin::playbackScopes());
    // In the Flatpak: ~/.var/app/org.fooyin.fooyin/cache/fooyin/tidal.
    m_cache = std::make_shared<TidalCache>(Fooyin::Utils::cachePath(u"tidal"_s), m_account);

    qCInfo(TIDAL) << "TIDAL plugin" << FOOYIN_TIDAL_VERSION << "loaded;"
                  << (m_account->isLoggedIn() ? "account logged in;" : "account not logged in;")
                  << (m_session->isLoggedIn() ? "logged in for playback" : "not logged in for playback");
}

void TidalPlugin::initialise(const Fooyin::GuiPluginContext& context)
{
    m_playlistInteractor = context.playlistInteractor;
    m_actionManager      = context.actionManager;
    m_trackSelection     = context.trackSelection;

    auto* addTrackAction = new QAction(tr("Add &TIDAL track…"), this);
    addTrackAction->setStatusTip(tr("Add a TIDAL track to the current playlist by its link or ID"));
    auto* command = context.actionManager->registerAction(addTrackAction, "Tidal.AddTrack");
    command->setCategories({tr("File")});
    context.actionManager->actionContainer(Fooyin::Constants::Menus::File)->addAction(command);
    QObject::connect(addTrackAction, &QAction::triggered, this, &TidalPlugin::addTrack);

    auto* showBrowserAction = new QAction(tr("&TIDAL"), this);
    showBrowserAction->setStatusTip(tr("Open the TIDAL window: search, favorites and playlists"));
    auto* showBrowserCommand = context.actionManager->registerAction(showBrowserAction, "Tidal.ShowBrowser");
    showBrowserCommand->setCategories({tr("View")});
    context.actionManager->actionContainer(Fooyin::Constants::Menus::View)->addAction(showBrowserCommand);
    QObject::connect(showBrowserAction, &QAction::triggered, this, &TidalPlugin::showBrowserWindow);

    context.widgetProvider->registerWidget(
        u"TidalBrowser"_s, [this]() { return new TidalBrowser(browserServices()); }, tr("TIDAL"));
    context.widgetProvider->setSubMenus(u"TidalBrowser"_s, {tr("Internet")});

    new TidalSettingsPage(m_settings->settingsDialog(), m_account, m_session, this);
}

TidalBrowser::Services TidalPlugin::browserServices() const
{
    return {.account            = m_account,
            .actionManager      = m_actionManager,
            .trackSelection     = m_trackSelection,
            .playlistInteractor = m_playlistInteractor,
            .settings           = m_settings,
            .cache              = m_cache};
}

void TidalPlugin::showBrowserWindow()
{
    if(m_browserWindow) {
        m_browserWindow->show();
        m_browserWindow->raise();
        m_browserWindow->activateWindow();
        return;
    }

    // Also available as a layout widget (Internet > TIDAL), for keeping it in the main window.
    m_browserWindow = new QDialog(Fooyin::Utils::getMainWindow());
    m_browserWindow->setAttribute(Qt::WA_DeleteOnClose);
    m_browserWindow->setWindowTitle(tr("TIDAL"));
    auto* layout = new QVBoxLayout(m_browserWindow);
    layout->addWidget(new TidalBrowser(browserServices(), m_browserWindow));
    m_browserWindow->resize(1000, 650);
    m_browserWindow->show();
}

QString TidalPlugin::inputName() const
{
    return u"TIDAL"_s;
}

Fooyin::InputCreator TidalPlugin::inputCreator() const
{
    Fooyin::InputCreator creator;
    creator.decoder = [session = m_session, weakLoader = m_audioLoader]() {
        // TIDAL's audio is FLAC (or AAC) in MP4, which fooyin's own FFmpeg decoder handles.
        auto createFFmpeg = [weakLoader]() -> std::unique_ptr<Fooyin::AudioDecoder> {
            const auto loader = weakLoader.lock();
            if(!loader) {
                return nullptr;
            }
            for(const auto& entry : loader->decoders()) {
                if(entry.name == "FFmpeg"_L1 && entry.creator) {
                    return entry.creator();
                }
            }
            return nullptr;
        };
        return std::make_unique<TidalDecoder>(session, createFFmpeg);
    };
    return creator;
}

void TidalPlugin::addTrack()
{
    auto* window = Fooyin::Utils::getMainWindow();

    if(!m_session->isLoggedIn()) {
        const auto answer = QMessageBox::question(
            window, tr("TIDAL"), tr("Not logged in to TIDAL for playback yet. Open the TIDAL settings to log in?"));
        if(answer == QMessageBox::Yes) {
            m_settings->settingsDialog()->openAtPage(SettingsPageId);
        }
        return;
    }

    bool ok{false};
    const QString text = QInputDialog::getText(window, tr("Add TIDAL Track"), tr("TIDAL track link or ID:"),
                                               QLineEdit::Normal, {}, &ok);
    if(!ok) {
        return;
    }

    const QString trackId = trackIdFrom(text);
    if(trackId.isEmpty()) {
        QMessageBox::warning(window, tr("TIDAL"), tr("That isn't a TIDAL track link or ID."));
        return;
    }

    using Result  = std::expected<Fooyin::Track, QString>;
    auto* watcher = new QFutureWatcher<Result>(this);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, window]() {
        const Result result = watcher->result();
        watcher->deleteLater();
        if(!result) {
            QMessageBox::warning(window, tr("TIDAL"), tr("Couldn't get that track: %1").arg(result.error()));
            return;
        }
        m_playlistInteractor->tracksToCurrentPlaylist({*result});
    });
    watcher->setFuture(QtConcurrent::run([session = m_session, trackId]() {
        TidalApi api{session};
        return api.track(trackId);
    }));
}
} // namespace FooyinTidal

#include "moc_tidalplugin.cpp"
