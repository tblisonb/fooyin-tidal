/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include "tidalbrowser.h"

#include <core/engine/inputplugin.h>
#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/guiplugin.h>

#include <QObject>
#include <QPointer>

#include <memory>

class QDialog;

namespace Fooyin {
class ActionManager;
class AudioLoader;
class PlayerController;
class PlaylistInteractor;
class TrackSelectionController;
class SettingsManager;
} // namespace Fooyin

namespace FooyinTidal {
class TidalCache;
class TidalCovers;
class TidalEditor;
class TidalSession;

class TidalPlugin : public QObject,
                    public Fooyin::Plugin,
                    public Fooyin::CorePlugin,
                    public Fooyin::GuiPlugin,
                    public Fooyin::InputPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FOOYIN_PLUGIN_IID FILE "tidal.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin Fooyin::GuiPlugin Fooyin::InputPlugin)

public:
    void initialise(const Fooyin::CorePluginContext& context) override;
    void initialise(const Fooyin::GuiPluginContext& context) override;

    [[nodiscard]] QString inputName() const override;
    [[nodiscard]] Fooyin::InputCreator inputCreator() const override;

private:
    void addTrack();
    void showBrowserWindow();
    //! TIDAL's entries in fooyin's track menu, and the Playback menu's for the playing track.
    void setupEditMenus(const Fooyin::GuiPluginContext& context);
    [[nodiscard]] Fooyin::TrackList playingTrack() const;
    [[nodiscard]] TidalBrowser::Services browserServices() const;

    std::weak_ptr<Fooyin::AudioLoader> m_audioLoader;
    Fooyin::SettingsManager* m_settings{nullptr};
    //! The official API's login.
    std::shared_ptr<TidalSession> m_account;
    //! The playback login.
    std::shared_ptr<TidalSession> m_session;
    //! What the panels have fetched, shared between them.
    std::shared_ptr<TidalCache> m_cache;
    //! Album covers for fooyin (cache/fooyin/tidal/covers).
    std::shared_ptr<TidalCovers> m_covers;
    //! Playlist and favorites changes, for the panels and fooyin's menus.
    TidalEditor* m_editor{nullptr};
    Fooyin::PlayerController* m_playerController{nullptr};
    Fooyin::PlaylistInteractor* m_playlistInteractor{nullptr};
    Fooyin::ActionManager* m_actionManager{nullptr};
    Fooyin::TrackSelectionController* m_trackSelection{nullptr};
    QPointer<QDialog> m_browserWindow;
};
} // namespace FooyinTidal
