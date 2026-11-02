/*
 * fooyin-tidal
 * Copyright © 2026, Tanner Lisonbee
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 */

#include "tidalbrowser.h"

#include "tidalcache.h"
#include "tidalsession.h"
#include "tidalsettingspage.h"

#include <gui/guiconstants.h>
#include <gui/playlist/playlistinteractor.h>
#include <gui/trackselectioncontroller.h>
#include <gui/widgets/autoheaderview.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/widgetcontext.h>
#include <utils/settings/settingsdialogcontroller.h>
#include <utils/settings/settingsmanager.h>

#include <QComboBox>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QActionGroup>
#include <QLoggingCategory>
#include <QMenu>
#include <QScrollBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>

Q_DECLARE_LOGGING_CATEGORY(TIDAL)

using namespace Qt::StringLiterals;

namespace {
constexpr int ItemIndexRole = Qt::UserRole;
constexpr int SortRole      = Qt::UserRole + 1;
constexpr auto SourceKey    = "Source"_L1;
// Each panel keeps its own columns (shown, order, widths) in fooyin's layout, as playlists do.
constexpr auto ColumnsKey      = "Columns"_L1;
constexpr auto ColumnCountKey  = "ColumnCount"_L1;
// The sort is the same in every list and every panel, and kept across restarts.
constexpr auto SortColumnSetting = "Tidal/BrowserSortColumn";
constexpr auto SortOrderSetting  = "Tidal/BrowserSortOrder";

enum Column : uint8_t
{
    Position = 0,
    Title,
    Artist,
    Album,
    Date,
    Details,
    // Hidden at first; new columns go here, after the ones sort settings may already name.
    AlbumArtist,
    TrackNumber,
    Released,
    ColumnCount,
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

//! A row that sorts by each column's SortRole when it has one (numbers, dates), else by text.
class Row : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        const QVariant mine = data(column, SortRole);
        const QVariant theirs = other.data(column, SortRole);
        if(mine.isValid() || theirs.isValid()) {
            // Rows without a value (no date) come first, in their own order.
            return mine.toLongLong() < theirs.toLongLong() && theirs.isValid();
        }
        return QString::localeAwareCompare(text(column).toCaseFolded(), other.text(column).toCaseFolded()) < 0;
    }
};

//! The date shown for an item: when it was added, or for the playlists list, when it last changed.
QDateTime dateFor(const FooyinTidal::TidalItem& item, bool playlistsList)
{
    return playlistsList ? item.modified : item.added;
}

QTreeWidgetItem* rowFor(const FooyinTidal::TidalItem& item, qsizetype index, qsizetype position, bool playlistsList)
{
    const QDateTime date = dateFor(item, playlistsList);
    const QString dateText
        = date.isValid() ? QLocale{}.toString(date.toLocalTime().date(), QLocale::ShortFormat) : QString{};

    const bool isTrack = item.kind == FooyinTidal::TidalItem::Kind::Track;
    QString trackNumber;
    if(isTrack && !item.track.trackNumber().isEmpty()) {
        trackNumber = item.track.discNumber().toInt() > 1 ? u"%1.%2"_s.arg(item.track.discNumber(), item.track.trackNumber())
                                                          : item.track.trackNumber();
    }

    auto* row = new Row(QStringList{QString::number(position + 1), item.title, item.artist, item.album, dateText,
                                    item.info, isTrack ? item.track.albumArtists().join(u", "_s) : QString{},
                                    trackNumber, isTrack ? item.track.date() : QString{}});
    row->setData(Position, ItemIndexRole, static_cast<qlonglong>(index));
    row->setData(Position, SortRole, static_cast<qlonglong>(position));
    if(date.isValid()) {
        row->setData(Date, SortRole, date.toMSecsSinceEpoch());
        row->setToolTip(Date, QLocale{}.toString(date.toLocalTime(), QLocale::LongFormat));
    }
    if(!item.info.isEmpty()) {
        row->setData(Details, SortRole, item.infoOrder);
    }
    if(!trackNumber.isEmpty()) {
        row->setData(TrackNumber, SortRole,
                     (item.track.discNumber().toLongLong() * 10000) + item.track.trackNumber().toLongLong());
    }
    row->setTextAlignment(Position, Qt::AlignRight | Qt::AlignVCenter);
    row->setTextAlignment(Details, Qt::AlignRight | Qt::AlignVCenter);
    row->setTextAlignment(TrackNumber, Qt::AlignRight | Qt::AlignVCenter);
    if(item.kind != FooyinTidal::TidalItem::Kind::Track) {
        row->setToolTip(Title, QCoreApplication::translate("FooyinTidal::TidalBrowser", "Double-click to open"));
    }
    return row;
}

//! What a background check of a cached list found.
struct Sync
{
    //! The cached copy is still TIDAL's.
    bool unchanged{false};
    QString version;
    QList<QJsonObject> pages;
};

/*!
 * Brings a list up to date: with `known` (a cached copy's version), asks TIDAL whether the list
 * changed and fetches it only if it did; without, fetches it.
 */
std::expected<Sync, QString> sync(const std::shared_ptr<FooyinTidal::TidalSession>& account,
                                  FooyinTidal::TidalCatalog::List list, const QString& id,
                                  const std::optional<QString>& known,
                                  const FooyinTidal::TidalCatalog::Cancelled& cancelled)
{
    FooyinTidal::TidalCatalog catalog{account};
    Sync result;
    if(FooyinTidal::TidalCatalog::hasVersion(list)) {
        auto version = catalog.version(list, id, cancelled);
        if(!version) {
            return std::unexpected(version.error());
        }
        result.version = *version;
        if(known && *known == result.version) {
            result.unchanged = true;
            return result;
        }
    }
    auto pages = catalog.allDocuments(list, id, cancelled);
    if(!pages) {
        return std::unexpected(pages.error());
    }
    result.pages = std::move(*pages);
    return result;
}

//! The first page of a list also brings the version it's cached under.
struct FirstPage
{
    QString version;
    QJsonObject page;
};
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
    , m_header{new Fooyin::AutoHeaderView(Qt::Horizontal, m_tree)}
    , m_status{new QLabel(this)}
    , m_sortColumn{std::clamp(m_services.settings->fileValue(SortColumnSetting, int{Position}).toInt(), 0,
                              ColumnCount - 1)}
    , m_sortOrder{m_services.settings->fileValue(SortOrderSetting, int{Qt::AscendingOrder}).toInt()
                          == Qt::DescendingOrder
                      ? Qt::DescendingOrder
                      : Qt::AscendingOrder}
    , m_alive{std::make_shared<std::atomic_bool>(true)}
{
    setObjectName(TidalBrowser::name());
    m_services.actionManager->addContextObject(m_context);

    m_back->setIcon(QIcon::fromTheme(u"go-previous"_s));
    m_back->setToolTip(tr("Back"));
    m_back->setAutoRaise(true);
    m_refresh->setIcon(QIcon::fromTheme(u"view-refresh"_s));
    m_refresh->setToolTip(tr("Reload from TIDAL"));
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

    // fooyin's own header, as in its playlists: columns can be resized, moved, hidden and auto-sized.
    m_tree->setHeader(m_header);
    m_tree->setHeaderLabels({u"#"_s, tr("Title"), tr("Artist"), tr("Album"), tr("Added"), tr("Details"),
                             tr("Album Artist"), tr("Track No."), tr("Release Date")});
    m_tree->headerItem()->setTextAlignment(Position, Qt::AlignRight | Qt::AlignVCenter);
    m_tree->headerItem()->setTextAlignment(TrackNumber, Qt::AlignRight | Qt::AlignVCenter);
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_header->setStretchLastSection(false);
    m_header->setStretchEnabled(true);
    m_header->setSectionsMovable(true);
    m_header->setFirstSectionMovable(false);
    m_header->setSectionsClickable(true);
    m_header->setContextMenuPolicy(Qt::CustomContextMenu);
    resetColumns();

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
    QObject::connect(m_header, &QHeaderView::sortIndicatorChanged, this, &TidalBrowser::saveSort);
    QObject::connect(m_header, &QWidget::customContextMenuRequested, this, &TidalBrowser::showHeaderMenu);
    QObject::connect(m_status, &QLabel::linkActivated, this, [this]() {
        m_services.settings->settingsDialog()->openAtPage(SettingsPageId);
    });
}

TidalBrowser::~TidalBrowser()
{
    m_alive->store(false);
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
    layout[SourceKey]      = m_source->currentIndex();
    layout[ColumnsKey]     = QString::fromLatin1(qCompress(m_header->saveHeaderState(false), 9).toBase64());
    layout[ColumnCountKey] = int{ColumnCount};
}

void TidalBrowser::loadLayoutData(const QJsonObject& layout)
{
    const int source = layout.value(SourceKey).toInt(DefaultSource);
    if(source >= 0 && source < m_source->count()) {
        m_source->setCurrentIndex(source);
    }

    // Columns saved by a version with other columns are left at the defaults.
    const QByteArray columns = qUncompress(QByteArray::fromBase64(layout.value(ColumnsKey).toString().toLatin1()));
    if(!columns.isEmpty() && layout.value(ColumnCountKey).toInt() == ColumnCount) {
        m_header->restoreHeaderState(columns, false);
    }
}

void TidalBrowser::resetColumns()
{
    m_header->resetSectionPositions();
    m_header->setHeaderSectionWidths({{Position, 0.05},
                                      {Title, 0.33},
                                      {Artist, 0.2},
                                      {Album, 0.2},
                                      {Date, 0.11},
                                      {Details, 0.11},
                                      {AlbumArtist, 0.15},
                                      {TrackNumber, 0.06},
                                      {Released, 0.11}});
    for(int column{0}; column < ColumnCount; ++column) {
        m_header->setHeaderSectionHidden(column, column == AlbumArtist || column == TrackNumber || column == Released);
    }
}

void TidalBrowser::showHeaderMenu(const QPoint& pos)
{
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto* columns = menu->addMenu(tr("Columns"));
    const int shown = m_header->count() - m_header->hiddenSectionCount();
    for(int section{0}; section < m_header->count(); ++section) {
        const int column = m_header->logicalIndex(section);
        auto* action     = columns->addAction(m_tree->headerItem()->text(column));
        action->setCheckable(true);
        action->setChecked(!m_header->isSectionHidden(column));
        // # stays: search results' group rows (and their arrows) are drawn in it. So do one other.
        action->setEnabled(column != Position && (!action->isChecked() || shown > 2));
        QObject::connect(action, &QAction::toggled, this,
                         [this, column](bool checked) { m_header->setHeaderSectionHidden(column, !checked); });
    }

    menu->addSeparator();
    auto* autoSize = menu->addAction(tr("Auto-size columns"));
    autoSize->setStatusTip(tr("Size columns to fill the panel's width"));
    autoSize->setCheckable(true);
    autoSize->setChecked(m_header->isStretchEnabled());
    QObject::connect(autoSize, &QAction::toggled, this, [this](bool checked) { m_header->setStretchEnabled(checked); });
    QObject::connect(menu->addAction(tr("Reset columns to default")), &QAction::triggered, this,
                     &TidalBrowser::resetColumns);

    menu->popup(m_header->mapToGlobal(pos));
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

TidalCatalog::Cancelled TidalBrowser::cancelled() const
{
    return [alive = m_alive]() {
        return !alive->load();
    };
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
    if(!view.isSearch) {
        m_services.cache->remove(view.list, view.id);
    }
    view.items.clear();
    view.groupSizes = {};
    view.nextCursor.clear();
    view.pages.clear();
    view.version.clear();
    view.loaded   = view.isSearch && view.query.isEmpty();
    view.failed   = false;
    view.updating = false;
    enter(std::move(view));
}

void TidalBrowser::enter(View view)
{
    ++m_generation;
    m_loading = false;

    bool check{false};
    if(!view.isSearch && !view.loaded && view.items.isEmpty()) {
        if(const TidalCache::Entry* entry = m_services.cache->find(view.list, view.id);
           entry && !TidalCache::expired(view.list, *entry)) {
            view.items   = entry->items;
            view.version = entry->version;
            view.loaded  = true;
            check        = TidalCache::needsCheck(view.list, *entry);
        }
    }

    m_views.push_back(std::move(view));
    render();
    if(check) {
        checkForChanges();
    }
}

void TidalBrowser::render()
{
    const View& view          = current();
    const bool playlistsList  = !view.isSearch && view.list == TidalCatalog::List::Playlists;
    const int scrollPosition  = m_tree->verticalScrollBar()->value();
    const bool sameViewRedraw = m_tree->topLevelItemCount() > 0 && view.updating;

    m_back->setEnabled(m_views.size() > 1);
    m_title->setVisible(!view.title.isEmpty());
    m_title->setText(view.title);
    m_tree->headerItem()->setText(Date, playlistsList ? tr("Modified") : tr("Added"));

    m_tree->setSortingEnabled(false);
    m_tree->clear();
    if(view.isSearch) {
        const std::array groupNames{tr("Tracks"), tr("Albums"), tr("Artists"), tr("Playlists")};
        m_tree->setRootIsDecorated(true);
        qsizetype index{0};
        for(size_t group{0}; group < groupNames.size(); ++group) {
            const qsizetype size = view.groupSizes.at(group);
            if(size == 0) {
                continue;
            }
            auto* header = new QTreeWidgetItem(QStringList{u"%1 (%2)"_s.arg(groupNames.at(group)).arg(size)});
            header->setFlags(Qt::ItemIsEnabled);
            QFont font = header->font(Position);
            font.setBold(true);
            header->setFont(Position, font);
            m_tree->addTopLevelItem(header);
            header->setFirstColumnSpanned(true);
            for(qsizetype i{0}; i < size; ++i, ++index) {
                header->addChild(rowFor(view.items.at(index), index, i, false));
            }
            header->setExpanded(true);
        }
        // Search results stay in TIDAL's order (best match first), grouped by kind.
        m_header->setSortIndicatorShown(false);
    }
    else {
        m_tree->setRootIsDecorated(false);
        QList<QTreeWidgetItem*> rows;
        rows.reserve(view.items.size());
        for(qsizetype i{0}; i < view.items.size(); ++i) {
            rows.push_back(rowFor(view.items.at(i), i, i, playlistsList));
        }
        m_tree->addTopLevelItems(rows);
        applySort();
    }

    if(sameViewRedraw) {
        m_tree->verticalScrollBar()->setValue(scrollPosition);
    }

    updateStatus();
    if(!view.loaded && !view.failed && !m_loading) {
        loadMore();
    }
}

void TidalBrowser::applySort()
{
    m_applyingSort = true;
    m_header->setSortIndicatorShown(true);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(m_sortColumn, m_sortOrder);
    m_applyingSort = false;
}

void TidalBrowser::saveSort(int column, Qt::SortOrder order)
{
    if(m_applyingSort || !m_tree->isSortingEnabled()) {
        return;
    }
    m_sortColumn = column;
    m_sortOrder  = order;
    m_services.settings->fileSet(SortColumnSetting, column);
    m_services.settings->fileSet(SortOrderSetting, static_cast<int>(order));
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

    // Page by page, each shown as it comes; the whole list goes in the cache at the end.
    using Result  = std::expected<FirstPage, QString>;
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

        View& view = current();
        if(view.pages.isEmpty()) {
            view.version = result->version;
        }
        view.pages.push_back(result->page);
        const TidalPage page    = TidalCatalog::parsePage(result->page);
        const qsizetype first   = view.items.size();
        const bool playlistList = view.list == TidalCatalog::List::Playlists;
        view.items.append(page.items);
        view.nextCursor = page.nextCursor;
        view.loaded     = view.nextCursor.isEmpty();

        QList<QTreeWidgetItem*> rows;
        for(qsizetype i{first}; i < view.items.size(); ++i) {
            rows.push_back(rowFor(view.items.at(i), i, i, playlistList));
        }
        m_tree->addTopLevelItems(rows);

        if(view.loaded) {
            m_services.cache->store(view.list, view.id, view.version, std::exchange(view.pages, {}));
            updateStatus();
        }
        else {
            loadMore();
        }
    });
    watcher->setFuture(QtConcurrent::run([account = m_services.account, list = view.list, id = view.id,
                                          cursor = view.nextCursor, cancelled = cancelled()]() -> Result {
        TidalCatalog catalog{account};
        FirstPage result;
        if(cursor.isEmpty() && TidalCatalog::hasVersion(list)) {
            // Before the pages: a change while they're fetched shows up as a new version later.
            auto version = catalog.version(list, id, cancelled);
            if(!version) {
                return std::unexpected(version.error());
            }
            result.version = *version;
        }
        auto page = catalog.pageDocument(list, id, cursor, cancelled);
        if(!page) {
            return std::unexpected(page.error());
        }
        result.page = std::move(*page);
        return result;
    }));
}

void TidalBrowser::checkForChanges()
{
    View& view = current();
    if(!m_services.account->isLoggedIn() || view.isSearch || view.updating) {
        return;
    }
    view.updating = true;
    updateStatus();

    const auto list          = view.list;
    const QString id         = view.id;

    using Result  = std::expected<Sync, QString>;
    auto* watcher = new QFutureWatcher<Result>(this);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, list, id]() {
        const Result result = watcher->result();
        watcher->deleteLater();

        // Kept even when the panel has moved on to another list.
        const TidalCache::Entry* entry{nullptr};
        if(!result) {
            // The cached copy stays; it's checked again next time.
            qCWarning(TIDAL) << "TIDAL panel: couldn't check for changes:" << result.error();
        }
        else if(result->unchanged) {
            if(TidalCache::Entry* cached = m_services.cache->find(list, id)) {
                cached->checked = QDateTime::currentDateTimeUtc();
            }
        }
        else {
            entry = m_services.cache->store(list, id, result->version, result->pages);
        }

        // The list may be the current view, or one Back leads to.
        for(size_t i{0}; i < m_views.size(); ++i) {
            View& view = m_views.at(i);
            if(view.isSearch || view.list != list || view.id != id || !view.updating) {
                continue;
            }
            if(entry) {
                view.items   = entry->items;
                view.version = entry->version;
            }
            if(i + 1 == m_views.size()) {
                if(entry) {
                    render();
                }
                view.updating = false;
                updateStatus();
            }
            else {
                view.updating = false;
            }
        }
    });
    watcher->setFuture(QtConcurrent::run(
        [account = m_services.account, list, id, known = view.version, cancelled = cancelled()]() {
            return sync(account, list, id, known, cancelled);
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
    else if(view.updating) {
        m_status->setText(tr("%n item(s) · checking TIDAL for changes…", nullptr, static_cast<int>(view.items.size())));
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
    const QVariant index = treeItem->data(Position, ItemIndexRole);
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

    // Play the list the track is in (its search group, or the whole view) as sorted, from this track.
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

    // A cached copy that's known to be current is used as it is; otherwise TIDAL is asked.
    std::optional<QString> known;
    if(const TidalCache::Entry* entry = m_services.cache->find(list, item.id)) {
        if(!TidalCache::needsCheck(list, *entry)) {
            send(entry->items, item.title, action);
            return;
        }
        known = entry->version;
    }

    m_status->setText(tr("Loading %1…").arg(item.title.toHtmlEscaped()));

    using Result  = std::expected<Sync, QString>;
    auto* watcher = new QFutureWatcher<Result>(this);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, this,
                     [this, watcher, action, list, id = item.id, title = item.title]() {
                         const Result result = watcher->result();
                         watcher->deleteLater();
                         updateStatus();
                         if(!result) {
                             qCWarning(TIDAL) << "TIDAL panel:" << result.error();
                             m_status->setText(tr("Couldn't load %1: %2")
                                                   .arg(title.toHtmlEscaped(), result.error().toHtmlEscaped()));
                             return;
                         }
                         TidalCache::Entry* entry = m_services.cache->find(list, id);
                         if(result->unchanged && entry) {
                             entry->checked = QDateTime::currentDateTimeUtc();
                         }
                         else {
                             entry = m_services.cache->store(list, id, result->version, result->pages);
                         }
                         send(entry->items, title, action);
                     });
    watcher->setFuture(QtConcurrent::run([account = m_services.account, list, id = item.id, known,
                                          cancelled = cancelled()]() { return sync(account, list, id, known, cancelled); }));
}

void TidalBrowser::send(const QList<TidalItem>& items, const QString& title, SendAction action)
{
    Fooyin::TrackList tracks;
    for(const TidalItem& entry : items) {
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
}
} // namespace FooyinTidal

#include "moc_tidalbrowser.cpp"
