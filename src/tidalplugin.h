/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include <core/engine/inputplugin.h>
#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/guiplugin.h>

#include <QObject>

#include <memory>

namespace Fooyin {
class AudioLoader;
class PlaylistInteractor;
class SettingsManager;
} // namespace Fooyin

namespace FooyinTidal {
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

    std::weak_ptr<Fooyin::AudioLoader> m_audioLoader;
    Fooyin::SettingsManager* m_settings{nullptr};
    //! The official API's login.
    std::shared_ptr<TidalSession> m_account;
    //! The playback login.
    std::shared_ptr<TidalSession> m_session;
    Fooyin::PlaylistInteractor* m_playlistInteractor{nullptr};
};
} // namespace FooyinTidal
