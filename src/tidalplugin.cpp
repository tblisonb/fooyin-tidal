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
#include "tidaldecoder.h"
#include "tidallogin.h"
#include "tidalsession.h"
#include "tidalsettingspage.h"

#include <core/engine/audioloader.h>
#include <core/plugins/coreplugincontext.h>
#include <gui/guiconstants.h>
#include <gui/playlist/playlistinteractor.h>
#include <gui/plugins/guiplugincontext.h>
#include <utils/actions/actioncontainer.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/fypaths.h>
#include <utils/settings/settingsdialogcontroller.h>
#include <utils/settings/settingsmanager.h>
#include <utils/utils.h>

#include <QAction>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QMessageBox>
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

    qCInfo(TIDAL) << "TIDAL plugin" << FOOYIN_TIDAL_VERSION << "loaded;"
                  << (m_account->isLoggedIn() ? "account logged in;" : "account not logged in;")
                  << (m_session->isLoggedIn() ? "logged in for playback" : "not logged in for playback");
}

void TidalPlugin::initialise(const Fooyin::GuiPluginContext& context)
{
    m_playlistInteractor = context.playlistInteractor;

    auto* addTrackAction = new QAction(tr("Add &TIDAL track…"), this);
    addTrackAction->setStatusTip(tr("Add a TIDAL track to the current playlist by its link or ID"));
    auto* command = context.actionManager->registerAction(addTrackAction, "Tidal.AddTrack");
    command->setCategories({tr("File")});
    context.actionManager->actionContainer(Fooyin::Constants::Menus::File)->addAction(command);
    QObject::connect(addTrackAction, &QAction::triggered, this, &TidalPlugin::addTrack);

    new TidalSettingsPage(m_settings->settingsDialog(), m_account, m_session, this);
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
