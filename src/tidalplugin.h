/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>

#include <QObject>

namespace FooyinTidal {
class TidalPlugin : public QObject,
                    public Fooyin::Plugin,
                    public Fooyin::CorePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FOOYIN_PLUGIN_IID FILE "tidal.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin)

public:
    void initialise(const Fooyin::CorePluginContext& context) override;
};
} // namespace FooyinTidal
