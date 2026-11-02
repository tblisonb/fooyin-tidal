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

#include <gui/fywidget.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace Fooyin {
class ActionManager;
class PlaylistInteractor;
class SettingsManager;
class TrackSelectionController;
class WidgetContext;
} // namespace Fooyin

namespace FooyinTidal {
class TidalCache;
class TidalSession;

/*!
 * The TIDAL panel: search, favorites and playlists from the account login. Albums, playlists and
 * artists open in place, with Back to return. Double-clicking a track plays the list it's in, in
 * the order shown, from there. Lists come from the cache when they're there, and are checked with
 * TIDAL in the background.
 */
class TidalBrowser : public Fooyin::FyWidget
{
    Q_OBJECT

public:
    struct Services
    {
        std::shared_ptr<TidalSession> account;
        Fooyin::ActionManager* actionManager{nullptr};
        Fooyin::TrackSelectionController* trackSelection{nullptr};
        Fooyin::PlaylistInteractor* playlistInteractor{nullptr};
        Fooyin::SettingsManager* settings{nullptr};
        std::shared_ptr<TidalCache> cache;
    };

    explicit TidalBrowser(Services services, QWidget* parent = nullptr);
    ~TidalBrowser() override;

    [[nodiscard]] QString name() const override;
    [[nodiscard]] QString layoutName() const override;
    void saveLayoutData(QJsonObject& layout) override;
    void loadLayoutData(const QJsonObject& layout) override;

protected:
    void showEvent(QShowEvent* event) override;

private:
    //! One place in the panel: a search, a list, or something opened from one.
    struct View
    {
        bool isSearch{false};
        QString query;
        TidalCatalog::List list{TidalCatalog::List::FavoriteTracks};
        QString id;
        QString title;

        QList<TidalItem> items;
        //! Search results' groups: how many of `items` are tracks, albums, artists, playlists.
        std::array<qsizetype, 4> groupSizes{};
        QString nextCursor;
        bool loaded{false};
        bool failed{false};

        //! While it's first fetched: the list's version and the pages so far, for the cache.
        QString version;
        QList<QJsonObject> pages;
        //! Being checked with TIDAL, or fetched again, while the cached copy is shown.
        bool updating{false};
    };

    enum class SendAction : uint8_t
    {
        Play,
        AddToCurrent,
        NewPlaylist,
    };

    void showSource(int index);
    void search();
    void open(const TidalItem& item);
    void back();
    void refresh();
    void enter(View view);

    void render();
    void loadMore();
    void checkForChanges();
    void showError(const QString& error);
    void updateStatus();

    [[nodiscard]] View& current();
    [[nodiscard]] const TidalItem* itemAt(QTreeWidgetItem* treeItem) const;
    void activate(QTreeWidgetItem* treeItem);
    void showContextMenu(const QPoint& pos);
    void updateSelection();
    void sendAll(const TidalItem& item, SendAction action);
    void send(const QList<TidalItem>& items, const QString& title, SendAction action);
    void applySort();
    void saveSort(int column, Qt::SortOrder order);
    [[nodiscard]] TidalCatalog::Cancelled cancelled() const;

    Services m_services;
    Fooyin::WidgetContext* m_context;
    QToolButton* m_back;
    QComboBox* m_source;
    QLineEdit* m_search;
    QToolButton* m_refresh;
    QLabel* m_title;
    QTreeWidget* m_tree;
    QLabel* m_status;

    std::vector<View> m_views;
    //! Results that arrive for an older load are dropped.
    quint64 m_generation{0};
    bool m_loading{false};
    bool m_shown{false};
    int m_sortColumn;
    Qt::SortOrder m_sortOrder;
    bool m_applyingSort{false};
    //! Lets background requests stop once the panel is gone.
    std::shared_ptr<std::atomic_bool> m_alive;
};
} // namespace FooyinTidal
