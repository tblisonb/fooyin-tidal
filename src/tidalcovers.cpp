/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "tidalcovers.h"

#include "blockinghttp.h"
#include "tidalapi.h"
#include "tidalsession.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>

#include <array>

Q_DECLARE_LOGGING_CATEGORY(TIDAL)

using namespace Qt::StringLiterals;

namespace {
// Covers not shown for this long are deleted.
constexpr auto PruneAfterDays = 60;
// TIDAL's largest square cover, then a smaller one in case an old album lacks it.
constexpr std::array CoverSizes{"1280x1280", "640x640"};

bool validCoverId(const QString& id)
{
    static const QRegularExpression uuid{u"^[0-9a-fA-F-]{8,64}$"_s};
    return uuid.match(id).hasMatch();
}
} // namespace

namespace FooyinTidal {
TidalCovers::TidalCovers(QString dir, std::shared_ptr<TidalSession> session)
    : m_dir{std::move(dir)}
    , m_session{std::move(session)}
{ }

QByteArray TidalCovers::front(const QString& trackId)
{
    if(trackId.isEmpty()) {
        return {};
    }
    pruneOnce();

    const auto id = coverId(trackId);
    if(!id || id->isEmpty()) {
        return {};
    }

    const QString path = m_dir + u"/"_s + *id + u".jpg"_s;
    QFile file{path};
    if(file.open(QIODevice::ReadOnly)) {
        QByteArray data = file.readAll();
        file.close();
        if(!data.isEmpty()) {
            // Shown again: keep it from being pruned.
            if(file.open(QIODevice::Append)) {
                file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
            }
            return data;
        }
    }

    QByteArray data = download(*id);
    if(data.isEmpty()) {
        return {};
    }

    QDir{}.mkpath(m_dir);
    QSaveFile save{path};
    if(save.open(QIODevice::WriteOnly)) {
        save.write(data);
        if(!save.commit()) {
            qCDebug(TIDAL) << "Couldn't keep the cover in" << path;
        }
    }
    return data;
}

std::optional<QString> TidalCovers::coverId(const QString& trackId)
{
    {
        const std::scoped_lock lock{m_mutex};
        if(const auto it = m_coverIds.constFind(trackId); it != m_coverIds.cend()) {
            return it.value();
        }
    }

    if(!m_session->isLoggedIn()) {
        return {};
    }

    TidalApi api{m_session};
    const auto id = api.albumCoverId(trackId);
    if(!id) {
        qCInfo(TIDAL) << "No cover for TIDAL track" << trackId << "-" << id.error();
        return {};
    }

    // TIDAL answered: remember it, including that there's no cover.
    const QString result = validCoverId(*id) ? *id : QString{};
    const std::scoped_lock lock{m_mutex};
    m_coverIds.insert(trackId, result);
    return result;
}

QByteArray TidalCovers::download(const QString& coverId) const
{
    // https://resources.tidal.com/images/<the UUID's parts as folders>/1280x1280.jpg
    const QString base = u"https://resources.tidal.com/images/%1/"_s.arg(QString{coverId}.replace(u'-', u'/'));

    BlockingHttp http;
    for(const char* size : CoverSizes) {
        const HttpResult result = http.get(QUrl{base + QLatin1StringView{size} + u".jpg"_s});
        if(result.ok() && !result.body.isEmpty()) {
            return result.body;
        }
        if(result.status != 404) {
            qCInfo(TIDAL) << "Couldn't download the TIDAL cover" << coverId << "-" << result.describe();
            return {};
        }
    }
    return {};
}

void TidalCovers::pruneOnce()
{
    {
        const std::scoped_lock lock{m_mutex};
        if(m_pruned) {
            return;
        }
        m_pruned = true;
    }

    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-PruneAfterDays);
    QDirIterator files{m_dir, {u"*.jpg"_s}, QDir::Files};
    while(files.hasNext()) {
        const QFileInfo info = files.nextFileInfo();
        if(info.lastModified() < cutoff) {
            QFile::remove(info.absoluteFilePath());
        }
    }
}

TidalReader::TidalReader(std::shared_ptr<TidalSession> session, std::shared_ptr<TidalCovers> covers)
    : m_session{std::move(session)}
    , m_covers{std::move(covers)}
{ }

QStringList TidalReader::extensions() const
{
    return {};
}

QStringList TidalReader::supportedSchemes() const
{
    return {u"tidal"_s};
}

bool TidalReader::canReadCover() const
{
    return true;
}

bool TidalReader::canWriteMetaData() const
{
    return false;
}

bool TidalReader::init(const Fooyin::AudioSource& source)
{
    return !trackIdFrom(source.filepath).isEmpty();
}

bool TidalReader::readTrack(const Fooyin::AudioSource& source, Fooyin::Track& track)
{
    if(!m_session->isLoggedIn()) {
        return false;
    }

    TidalApi api{m_session};
    const auto details = api.track(trackIdFrom(source.filepath));
    if(!details) {
        qCInfo(TIDAL) << "Couldn't read TIDAL track" << source.filepath << "-" << details.error();
        return false;
    }

    track.setTitle(details->title());
    track.setArtists(details->artists());
    track.setAlbumArtists(details->albumArtists());
    track.setAlbum(details->album());
    track.setTrackNumber(details->trackNumber());
    track.setDiscNumber(details->discNumber());
    track.setDuration(details->duration());
    track.setCodec(details->codec());
    return true;
}

QByteArray TidalReader::readCover(const Fooyin::AudioSource& source, const Fooyin::Track& /*track*/,
                                  Fooyin::Track::Cover cover)
{
    if(cover != Fooyin::Track::Cover::Front || !m_covers) {
        return {};
    }
    return m_covers->front(trackIdFrom(source.filepath));
}
} // namespace FooyinTidal
