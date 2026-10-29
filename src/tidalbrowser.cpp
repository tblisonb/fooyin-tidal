/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "tidalbrowser.h"

#include "tidalsession.h"
#include "tidalsettingspage.h"

#include <gui/guiconstants.h>
#include <gui/playlist/playlistinteractor.h>
#include <gui/trackselectioncontroller.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/widgetcontext.h>
#include <utils/settings/settingsdialogcontroller.h>
#include <utils/settings/settingsmanager.h>

#include <QComboBox>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMenu>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>

Q_DECLARE_LOGGING_CATEGORY(TIDAL)

using namespace Qt::StringLiterals;

namespace {
constexpr int ItemIndexRole = Qt::UserRole;
constexpr auto SourceKey    = "Source"_L1;

enum Column : uint8_t
{
    Title = 0,
    Artist,
    Album,
    Details,
};

struct Source
{
    const char* name;
    FooyinTidal::TidalCatalog::List list;
};

// The sources the panel starts from; the first entry is search.
constexpr std::array Sources{
    Source{QT_TRANSLATE_NOOP("FooyinTidal::TidalBrowser", "Search"), FooyinTidal::TidalCatalog::List::FavoriteTracks},
    Source{QT_TRANSLATE_NOOP("FooyinTidal::TidalBrowser", "Favorite Tracks"),
           FooyinTidal::TidalCatalog::List::FavoriteTracks},
    Source{QT_TRANSLATE_NOOP("FooyinTidal::TidalBrowser", "Favorite Albums"),
           FooyinTidal::TidalCatalog::List::FavoriteAlbums},
    Source{QT_TRANSLATE_NOOP("FooyinTidal::TidalBrowser", "Favorite Artists"),
           FooyinTidal::TidalCatalog::List::FavoriteArtists},
    Source{QT_TRANSLATE_NOOP("FooyinTidal::TidalBrowser", "Playlists"), FooyinTidal::TidalCatalog::List::Playlists},
};
constexpr int SearchSource  = 0;
constexpr int DefaultSource = 1;

QTreeWidgetItem* rowFor(const FooyinTidal::TidalItem& item, qsizetype index)
{
    auto* row = new QTreeWidgetItem(QStringList{item.title, item.artist, item.album, item.info});
    row->setData(Title, ItemIndexRole, static_cast<qlonglong>(index));
    row->setTextAlignment(Details, Qt::AlignRight | Qt::AlignVCenter);
    if(item.kind != FooyinTidal::TidalItem::Kind::Track) {
        row->setToolTip(Title, QCoreApplication::translate("FooyinTidal::TidalBrowser", "Double-click to open"));
    }
    return row;
}
} // namespace

namespace FooyinTidal {
TidalBrowser::TidalBrowser(Services services, QWidget* parent)
    : FyWidget{parent}
    , m_services{std::move(services)}
    , m_context{new Fooyin::WidgetContext(
          this,
          Fooyin::Context{Fooyin::IdList{Fooyin::Constants::Context::TrackSelection,
                                         Fooyin::Id{"Tidal.Context.Browser."}.append(id())}},
          this)}
    , m_back{new QToolButton(this)}
    , m_source{new QComboBox(this)}
    , m_search{new QLineEdit(this)}
    , m_refresh{new QToolButton(this)}
    , m_title{new QLabel(this)}
    , m_tree{new QTreeWidget(this)}
    , m_status{new QLabel(this)}
{
    setObjectName(TidalBrowser::name());
    m_services.actionManager->addContextObject(m_context);

    m_back->setIcon(QIcon::fromTheme(u"go-previous"_s));
    m_back->setToolTip(tr("Back"));
    m_back->setAutoRaise(true);
    m_refresh->setIcon(QIcon::fromTheme(u"view-refresh"_s));
    m_refresh->setToolTip(tr("Reload"));
    m_refresh->setAutoRaise(true);

    for(const Source& source : Sources) {
        m_source->addItem(tr(source.name));
    }
    m_source->setCurrentIndex(DefaultSource);
    m_search->setPlaceholderText(tr("Search TIDAL"));
    m_search->setClearButtonEnabled(true);

    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_title->hide();

    m_tree->setHeaderLabels({tr("Title"), tr("Artist"), tr("Album"), tr("Details")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(Title, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(Details, QHeaderView::ResizeToContents);
    m_tree->setColumnWidth(Artist, 200);
    m_tree->setColumnWidth(Album, 200);

    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::RichText);

    auto* bar = new QHBoxLayout;
    bar->addWidget(m_back);
    bar->addWidget(m_source);
    bar->addWidget(m_search, 1);
    bar->addWidget(m_refresh);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins({});
    layout->addLayout(bar);
    layout->addWidget(m_title);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_status);

    QObject::connect(m_back, &QToolButton::clicked, this, &TidalBrowser::back);
    QObject::connect(m_refresh, &QToolButton::clicked, this, &TidalBrowser::refresh);
    QObject::connect(m_source, &QComboBox::activated, this, &TidalBrowser::showSource);
    QObject::connect(m_search, &QLineEdit::returnPressed, this, &TidalBrowser::search);
    QObject::connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) { activate(item); });
    QObject::connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &TidalBrowser::showContextMenu);
    QObject::connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &TidalBrowser::updateSelection);
    QObject::connect(m_status, &QLabel::linkActivated, this, [this]() {
        m_services.settings->settingsDialog()->openAtPage(SettingsPageId);
    });
}

QString TidalBrowser::name() const
{
    return tr("TIDAL");
}

QString TidalBrowser::layoutName() const
{
    return u"TidalBrowser"_s;
}

void TidalBrowser::saveLayoutData(QJsonObject& layout)
{
    layout[SourceKey] = m_source->currentIndex();
}

void TidalBrowser::loadLayoutData(const QJsonObject& layout)
{
    const int source = layout.value(SourceKey).toInt(DefaultSource);
    if(source >= 0 && source < m_source->count()) {
        m_source->setCurrentIndex(source);
    }
}

void TidalBrowser::showEvent(QShowEvent* event)
{
    FyWidget::showEvent(event);
    // Nothing is fetched until the panel is first seen.
    if(!m_shown) {
        m_shown = true;
        showSource(m_source->currentIndex());
    }
}

TidalBrowser::View& TidalBrowser::current()
{
    return m_views.back();
}

void TidalBrowser::showSource(int index)
{
    m_views.clear();
    if(index == SearchSource) {
        m_search->setFocus();
        if(!m_search->text().trimmed().isEmpty()) {
            search();
            return;
        }
        enter(View{.isSearch = true, .loaded = true});
        return;
    }
    enter(View{.list = Sources.at(index).list});
}

void TidalBrowser::search()
{
    const QString query = m_search->text().trimmed();
    if(query.isEmpty()) {
        return;
    }
    m_source->setCurrentIndex(SearchSource);
    m_views.clear();
    enter(View{.isSearch = true, .query = query});
}

void TidalBrowser::open(const TidalItem& item)
{
    View view;
    view.id    = item.id;
    view.title = item.artist.isEmpty() || item.kind == TidalItem::Kind::Playlist
                   ? item.title
                   : u"%1 – %2"_s.arg(item.artist, item.title);
    switch(item.kind) {
        case TidalItem::Kind::Album:
            view.list = TidalCatalog::List::AlbumTracks;
            break;
        case TidalItem::Kind::Playlist:
            view.list = TidalCatalog::List::PlaylistTracks;
            break;
        case TidalItem::Kind::Artist:
            view.list = TidalCatalog::List::ArtistAlbums;
            break;
        case TidalItem::Kind::Track:
            return;
    }
    enter(std::move(view));
}

void TidalBrowser::back()
{
    if(m_views.size() > 1) {
        m_views.pop_back();
        ++m_generation;
        m_loading = false;
        render();
    }
}

void TidalBrowser::refresh()
{
    if(m_views.empty()) {
        return;
    }
    View view = current();
    m_views.pop_back();
    view.items.clear();
    view.groupSizes = {};
    view.nextCursor.clear();
    view.loaded = view.isSearch && view.query.isEmpty();
    view.failed = false;
    enter(std::move(view));
}

void TidalBrowser::enter(View view)
{
    ++m_generation;
    m_loading = false;
    m_views.push_back(std::move(view));
    render();
}

void TidalBrowser::render()
{
    const View& view = current();

    m_back->setEnabled(m_views.size() > 1);
    m_title->setVisible(!view.title.isEmpty());
    m_title->setText(view.title);

    m_tree->clear();
    if(view.isSearch) {
        const std::array groupNames{tr("Tracks"), tr("Albums"), tr("Artists"), tr("Playlists")};
        m_tree->setRootIsDecorated(true);
        qsizetype index{0};
        for(size_t group{0}; group < groupNames.size(); ++group) {
            const qsizetype size = view.groupSizes.at(group);
            if(size == 0) {
                index += size;
                continue;
            }
            auto* header = new QTreeWidgetItem(QStringList{u"%1 (%2)"_s.arg(groupNames.at(group)).arg(size)});
            header->setFlags(Qt::ItemIsEnabled);
            QFont font = header->font(Title);
            font.setBold(true);
            header->setFont(Title, font);
            m_tree->addTopLevelItem(header);
            header->setFirstColumnSpanned(true);
            for(qsizetype i{0}; i < size; ++i, ++index) {
                header->addChild(rowFor(view.items.at(index), index));
            }
            header->setExpanded(true);
        }
    }
    else {
        m_tree->setRootIsDecorated(false);
        QList<QTreeWidgetItem*> rows;
        rows.reserve(view.items.size());
        for(qsizetype i{0}; i < view.items.size(); ++i) {
            rows.push_back(rowFor(view.items.at(i), i));
        }
        m_tree->addTopLevelItems(rows);
    }

    updateStatus();
    if(!view.loaded && !view.failed && !m_loading) {
        loadMore();
    }
}

void TidalBrowser::loadMore()
{
    if(!m_services.account->isLoggedIn()) {
        updateStatus();
        return;
    }

    const quint64 generation = m_generation;
    const View& view         = current();
    m_loading                = true;
    updateStatus();

    if(view.isSearch) {
        using Result  = std::expected<TidalSearch, QString>;
        auto* watcher = new QFutureWatcher<Result>(this);
        QObject::connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, generation]() {
            const Result result = watcher->result();
            watcher->deleteLater();
            if(generation != m_generation) {
                return;
            }
            m_loading = false;
            if(!result) {
                showError(result.error());
                return;
            }
            View& view      = current();
            view.items      = result->tracks + result->albums + result->artists + result->playlists;
            view.groupSizes = {result->tracks.size(), result->albums.size(), result->artists.size(),
                               result->playlists.size()};
            view.loaded     = true;
            render();
        });
        watcher->setFuture(QtConcurrent::run([account = m_services.account, query = view.query]() {
            return TidalCatalog{account}.search(query);
        }));
        return;
    }

    using Result  = std::expected<TidalPage, QString>;
    auto* watcher = new QFutureWatcher<Result>(this);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, generation]() {
        const Result result = watcher->result();
        watcher->deleteLater();
        if(generation != m_generation) {
            return;
        }
        m_loading = false;
        if(!result) {
            showError(result.error());
            return;
        }

        View& view            = current();
        const qsizetype first = view.items.size();
        view.items.append(result->items);
        view.nextCursor = result->nextCursor;
        view.loaded     = view.nextCursor.isEmpty();

        QList<QTreeWidgetItem*> rows;
        for(qsizetype i{first}; i < view.items.size(); ++i) {
            rows.push_back(rowFor(view.items.at(i), i));
        }
        m_tree->addTopLevelItems(rows);

        if(view.loaded) {
            updateStatus();
        }
        else {
            loadMore();
        }
    });
    watcher->setFuture(QtConcurrent::run(
        [account = m_services.account, list = view.list, id = view.id, cursor = view.nextCursor]() {
            return TidalCatalog{account}.page(list, id, cursor);
        }));
}

void TidalBrowser::showError(const QString& error)
{
    qCWarning(TIDAL) << "TIDAL panel:" << error;
    current().failed = true;
    m_status->setText(tr("Couldn't load from TIDAL: %1").arg(error.toHtmlEscaped()));
}

void TidalBrowser::updateStatus()
{
    if(!m_services.account->isLoggedIn()) {
        m_status->setText(tr("Not logged in to your TIDAL account. <a href=\"settings\">Log in…</a>"));
        return;
    }

    const View& view = current();
    if(m_loading) {
        m_status->setText(view.items.isEmpty() ? tr("Loading…") : tr("Loading… %1 so far").arg(view.items.size()));
    }
    else if(view.isSearch && view.query.isEmpty()) {
        m_status->setText(tr("Type something to search for and press Enter."));
    }
    else if(view.items.isEmpty()) {
        m_status->setText(view.isSearch ? tr("Nothing found.") : tr("Nothing here."));
    }
    else {
        m_status->setText(tr("%n item(s)", nullptr, static_cast<int>(view.items.size())));
    }
}

const TidalItem* TidalBrowser::itemAt(QTreeWidgetItem* treeItem) const
{
    if(!treeItem || m_views.empty()) {
        return nullptr;
    }
    const QVariant index = treeItem->data(Title, ItemIndexRole);
    const auto& items    = m_views.back().items;
    if(!index.isValid() || index.toLongLong() < 0 || index.toLongLong() >= items.size()) {
        return nullptr;
    }
    return &items.at(index.toLongLong());
}

void TidalBrowser::activate(QTreeWidgetItem* treeItem)
{
    const TidalItem* item = itemAt(treeItem);
    if(!item) {
        return;
    }
    if(item->kind != TidalItem::Kind::Track) {
        open(*item);
        return;
    }

    // Play the list the track is in (its search group, or the whole view), from this track.
    QTreeWidgetItem* parent = treeItem->parent();
    const int count         = parent ? parent->childCount() : m_tree->topLevelItemCount();
    Fooyin::TrackList tracks;
    int start{0};
    for(int i{0}; i < count; ++i) {
        QTreeWidgetItem* sibling = parent ? parent->child(i) : m_tree->topLevelItem(i);
        const TidalItem* other   = itemAt(sibling);
        if(other && other->kind == TidalItem::Kind::Track) {
            if(sibling == treeItem) {
                start = static_cast<int>(tracks.size());
            }
            tracks.push_back(other->track);
        }
    }
    m_services.trackSelection->startPlayback(tracks, start);
}

void TidalBrowser::updateSelection()
{
    Fooyin::TrackSelection selection;
    for(QTreeWidgetItem* treeItem : m_tree->selectedItems()) {
        if(const TidalItem* item = itemAt(treeItem); item && item->kind == TidalItem::Kind::Track) {
            selection.tracks.push_back(item->track);
        }
    }
    m_services.trackSelection->changeSelectedTracks(m_context, selection);
}

void TidalBrowser::showContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* treeItem = m_tree->itemAt(pos);
    const TidalItem* item     = itemAt(treeItem);
    if(!item) {
        return;
    }

    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    if(item->kind == TidalItem::Kind::Track) {
        updateSelection();
        m_services.trackSelection->addTrackContextMenu(menu, m_context);
    }
    else {
        const TidalItem target = *item;
        menu->addAction(tr("&Open"), this, [this, target]() { open(target); });
        if(target.kind != TidalItem::Kind::Artist) {
            menu->addSeparator();
            menu->addAction(tr("&Play"), this, [this, target]() { sendAll(target, SendAction::Play); });
            menu->addAction(tr("&Add to Current Playlist"), this,
                            [this, target]() { sendAll(target, SendAction::AddToCurrent); });
            menu->addAction(tr("Send to &New Playlist"), this,
                            [this, target]() { sendAll(target, SendAction::NewPlaylist); });
        }
    }

    menu->popup(m_tree->viewport()->mapToGlobal(pos));
}

void TidalBrowser::sendAll(const TidalItem& item, SendAction action)
{
    const auto list
        = item.kind == TidalItem::Kind::Album ? TidalCatalog::List::AlbumTracks : TidalCatalog::List::PlaylistTracks;

    m_status->setText(tr("Loading %1…").arg(item.title.toHtmlEscaped()));

    using Result  = std::expected<QList<TidalItem>, QString>;
    auto* watcher = new QFutureWatcher<Result>(this);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, action, title = item.title]() {
        const Result result = watcher->result();
        watcher->deleteLater();
        updateStatus();
        if(!result) {
            qCWarning(TIDAL) << "TIDAL panel:" << result.error();
            m_status->setText(tr("Couldn't load %1: %2").arg(title.toHtmlEscaped(), result.error().toHtmlEscaped()));
            return;
        }

        Fooyin::TrackList tracks;
        for(const TidalItem& entry : *result) {
            if(entry.kind == TidalItem::Kind::Track) {
                tracks.push_back(entry.track);
            }
        }
        if(tracks.empty()) {
            return;
        }
        switch(action) {
            case SendAction::Play:
                m_services.trackSelection->startPlayback(tracks, 0);
                break;
            case SendAction::AddToCurrent:
                m_services.playlistInteractor->tracksToCurrentPlaylist(tracks);
                break;
            case SendAction::NewPlaylist:
                m_services.playlistInteractor->tracksToNewPlaylist(title, tracks);
                break;
        }
    });
    watcher->setFuture(QtConcurrent::run(
        [account = m_services.account, list, id = item.id]() { return TidalCatalog{account}.all(list, id); }));
}
} // namespace FooyinTidal

#include "moc_tidalbrowser.cpp"
