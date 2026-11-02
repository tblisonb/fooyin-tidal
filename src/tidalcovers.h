/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#pragma once

#include <core/engine/audioinput.h>

#include <QHash>
#include <QString>

#include <memory>
#include <mutex>
#include <optional>

namespace FooyinTidal {
class TidalSession;

/*!
 * Album covers for TIDAL tracks: the track's cover ID from v1 (remembered for the session), and the
 * image from TIDAL's image server, kept on disk by cover ID so an album's tracks share one file.
 * Blocking, and safe to use from several threads at once.
 */
class TidalCovers
{
public:
    TidalCovers(QString dir, std::shared_ptr<TidalSession> session);

    //! The track's album cover as a JPEG; empty if it has none or TIDAL couldn't be reached.
    QByteArray front(const QString& trackId);

private:
    //! The cover ID ("" for none), or nothing if TIDAL couldn't be asked.
    std::optional<QString> coverId(const QString& trackId);
    QByteArray download(const QString& coverId) const;
    void pruneOnce();

    QString m_dir;
    std::shared_ptr<TidalSession> m_session;
    std::mutex m_mutex;
    QHash<QString, QString> m_coverIds;
    bool m_pruned{false};
};

/*!
 * fooyin's reader for tidal:// tracks. Gives fooyin the album cover (as if it were embedded in the
 * file), and the track's details for tidal:// paths fooyin hasn't got them for.
 */
class TidalReader : public Fooyin::AudioReader
{
public:
    TidalReader(std::shared_ptr<TidalSession> session, std::shared_ptr<TidalCovers> covers);

    [[nodiscard]] QStringList extensions() const override;
    [[nodiscard]] QStringList supportedSchemes() const override;
    [[nodiscard]] bool canReadCover() const override;
    [[nodiscard]] bool canWriteMetaData() const override;

    bool init(const Fooyin::AudioSource& source) override;
    [[nodiscard]] bool readTrack(const Fooyin::AudioSource& source, Fooyin::Track& track) override;
    [[nodiscard]] QByteArray readCover(const Fooyin::AudioSource& source, const Fooyin::Track& track,
                                       Fooyin::Track::Cover cover) override;

private:
    std::shared_ptr<TidalSession> m_session;
    std::shared_ptr<TidalCovers> m_covers;
};
} // namespace FooyinTidal
