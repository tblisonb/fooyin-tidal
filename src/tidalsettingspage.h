/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include <utils/settings/settingspage.h>

#include <memory>

namespace FooyinTidal {
class TidalSession;

constexpr auto SettingsPageId = "Tidal.Settings";

//! Settings > Integrations > TIDAL: the account and playback logins.
class TidalSettingsPage : public Fooyin::SettingsPage
{
    Q_OBJECT

public:
    TidalSettingsPage(Fooyin::SettingsDialogController* controller, std::shared_ptr<TidalSession> account,
                      std::shared_ptr<TidalSession> playback, QObject* parent = nullptr);
};
} // namespace FooyinTidal
