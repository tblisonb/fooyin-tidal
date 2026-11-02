/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "tidalcache.h"

#include "tidalsession.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>

Q_DECLARE_LOGGING_CATEGORY(TIDAL)

using namespace Qt::StringLiterals;

namespace {
constexpr int FileFormat = 1;
// How long a list is trusted before TIDAL is asked whether it changed. Short: it saves only the
// one request when going back and forth between lists.
constexpr qint64 CheckAfterSecs = 60;
// Lists without a version (an artist's albums) are fetched again after this.
constexpr qint64 ExpireAfterSecs = 24 * 60 * 60;
// Files not used for this long are deleted.
constexpr qint64 PruneAfterDays = 30;
} // namespace

namespace FooyinTidal {
TidalCache::TidalCache(QString dir, std::shared_ptr<TidalSession> account)
    : m_dir{std::move(dir)}
    , m_account{std::move(account)}
{ }

QString TidalCache::accountDir() const
{
    const QString userId = m_account->userId();
    return userId.isEmpty() ? QString{} : m_dir + u'/' + userId;
}

QString TidalCache::key(TidalCatalog::List list, const QString& id)
{
    using List = TidalCatalog::List;
    static const QRegularExpression unsafe{u"[^A-Za-z0-9-]"_s};
    const QString safeId = QString{id}.replace(unsafe, u"_"_s);
    switch(list) {
        case List::FavoriteTracks:
            return u"favorite-tracks"_s;
        case List::FavoriteAlbums:
            return u"favorite-albums"_s;
        case List::FavoriteArtists:
            return u"favorite-artists"_s;
        case List::Playlists:
            return u"playlists"_s;
        case List::AlbumTracks:
            return u"album-"_s + safeId;
        case List::PlaylistTracks:
            return u"playlist-"_s + safeId;
        case List::ArtistAlbums:
            return u"artist-"_s + safeId;
    }
    return {};
}

TidalCache::Entry* TidalCache::find(TidalCatalog::List list, const QString& id)
{
    const QString dir = accountDir();
    if(dir.isEmpty()) {
        return nullptr;
    }
    if(m_userId != m_account->userId()) {
        // Another account: what's in memory belongs to the last one.
        m_entries.clear();
        m_userId = m_account->userId();
        prune(dir);
    }

    const QString name = key(list, id);
    if(const auto it = m_entries.find(name); it != m_entries.end()) {
        return it->second.get();
    }

    QFile file{dir + u'/' + name + u".json"_s};
    if(!file.open(QIODevice::ReadOnly)) {
        return nullptr;
    }
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    if(json.value("format"_L1).toInt() != FileFormat) {
        return nullptr;
    }

    auto entry     = std::make_unique<Entry>();
    entry->version = json.value("version"_L1).toString();
    entry->fetched = QDateTime::fromString(json.value("fetched"_L1).toString(), Qt::ISODateWithMs);
    for(const auto& page : json.value("pages"_L1).toArray()) {
        entry->pages.push_back(page.toObject());
    }
    for(const QJsonObject& page : std::as_const(entry->pages)) {
        entry->items.append(TidalCatalog::parsePage(page).items);
    }
    // Used: keep it from being pruned.
    file.close();
    if(file.open(QIODevice::Append)) {
        file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
    }

    return m_entries.insert_or_assign(name, std::move(entry)).first->second.get();
}

TidalCache::Entry* TidalCache::store(TidalCatalog::List list, const QString& id, QString version,
                                     QList<QJsonObject> pages)
{
    auto entry     = std::make_unique<Entry>();
    entry->version = std::move(version);
    entry->pages   = std::move(pages);
    entry->fetched = QDateTime::currentDateTimeUtc();
    entry->checked = entry->fetched;
    for(const QJsonObject& page : std::as_const(entry->pages)) {
        entry->items.append(TidalCatalog::parsePage(page).items);
    }

    const QString name = key(list, id);
    if(const QString dir = accountDir(); !dir.isEmpty() && QDir{}.mkpath(dir)) {
        QJsonArray pagesJson;
        for(const QJsonObject& page : std::as_const(entry->pages)) {
            pagesJson.push_back(page);
        }
        const QJsonObject json{
            {u"format"_s, FileFormat},
            {u"version"_s, entry->version},
            {u"fetched"_s, entry->fetched.toString(Qt::ISODateWithMs)},
            {u"pages"_s, pagesJson},
        };
        QSaveFile file{dir + u'/' + name + u".json"_s};
        if(!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument{json}.toJson(QJsonDocument::Compact)) < 0
           || !file.commit()) {
            qCWarning(TIDAL) << "Couldn't save the TIDAL cache file" << file.fileName() << file.errorString();
        }
    }

    m_userId = m_account->userId();
    return m_entries.insert_or_assign(name, std::move(entry)).first->second.get();
}

void TidalCache::remove(TidalCatalog::List list, const QString& id)
{
    const QString name = key(list, id);
    m_entries.erase(name);
    if(const QString dir = accountDir(); !dir.isEmpty()) {
        QFile::remove(dir + u'/' + name + u".json"_s);
    }
}

bool TidalCache::needsCheck(TidalCatalog::List list, const Entry& entry)
{
    return TidalCatalog::hasVersion(list)
        && (!entry.checked.isValid() || entry.checked.secsTo(QDateTime::currentDateTimeUtc()) > CheckAfterSecs);
}

bool TidalCache::expired(TidalCatalog::List list, const Entry& entry)
{
    return list == TidalCatalog::List::ArtistAlbums
        && (!entry.fetched.isValid() || entry.fetched.secsTo(QDateTime::currentDateTimeUtc()) > ExpireAfterSecs);
}

void TidalCache::prune(const QString& dir) const
{
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-PruneAfterDays);
    QDirIterator files{dir, {u"*.json"_s}, QDir::Files};
    while(files.hasNext()) {
        const QFileInfo file = files.nextFileInfo();
        if(file.lastModified() < cutoff) {
            QFile::remove(file.filePath());
        }
    }
}
} // namespace FooyinTidal
