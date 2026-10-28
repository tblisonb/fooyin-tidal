/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "tidalplugin.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(TIDAL, "fooyin.tidal")

namespace FooyinTidal {
void TidalPlugin::initialise(const Fooyin::CorePluginContext& /*context*/)
{
    qCInfo(TIDAL) << "TIDAL plugin" << FOOYIN_TIDAL_VERSION << "loaded";
}
} // namespace FooyinTidal
