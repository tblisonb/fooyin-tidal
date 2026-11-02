/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include "tidalcatalog.h"

#include <QDateTime>
#include <QJsonObject>

#include <map>
#include <memory>

namespace FooyinTidal {
class TidalSession;

/*!
 * Lists the panel has fetched (favorites, playlists, albums…), kept in memory and on disk so that
 * opening one again doesn't fetch it all again. Each list keeps TIDAL's pages as they came, and
 * the version that `TidalCatalog::version()` gave when they were fetched, to tell whether they're
 * out of date. Kept per account. Use it from the GUI thread only.
 */
class TidalCache
{
public:
    struct Entry
    {
        QString version;
        QList<QJsonObject> pages;
        QDateTime fetched;
        //! When `version` was last found to still be TIDAL's; not kept on disk.
        QDateTime checked;
        //! The pages' items, made when the entry is first used; not kept on disk.
        QList<TidalItem> items;
    };

    //! Files go in `dir`/<the account's user ID>.
    TidalCache(QString dir, std::shared_ptr<TidalSession> account);

    //! The list as last fetched, or nothing. Reads it from disk the first time.
    [[nodiscard]] Entry* find(TidalCatalog::List list, const QString& id);
    //! Keeps a freshly fetched list, replacing what was there.
    Entry* store(TidalCatalog::List list, const QString& id, QString version, QList<QJsonObject> pages);
    //! Forgets a list, on disk too.
    void remove(TidalCatalog::List list, const QString& id);

    //! Whether the entry should be checked against TIDAL before it's trusted any further.
    [[nodiscard]] static bool needsCheck(TidalCatalog::List list, const Entry& entry);
    //! Whether the entry is too old to show at all (only for lists without a version).
    [[nodiscard]] static bool expired(TidalCatalog::List list, const Entry& entry);

private:
    [[nodiscard]] QString accountDir() const;
    [[nodiscard]] static QString key(TidalCatalog::List list, const QString& id);
    void prune(const QString& dir) const;

    QString m_dir;
    std::shared_ptr<TidalSession> m_account;
    QString m_userId;
    std::map<QString, std::unique_ptr<Entry>> m_entries;
};
} // namespace FooyinTidal
