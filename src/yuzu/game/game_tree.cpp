// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <QApplication>
#include <QHeaderView>
#include <QPainter>
#include <QPainterPath>
#include <QScroller>
#include <QScrollerProperties>
#include <QStyledItemDelegate>
#include <QTimer>

#include "openpak/account.h"
#include "openpak/qt/nzp_online_count.h"
#include "openpak/qt/online_counts.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "yuzu/game/common.h"
#include "yuzu/game/game_tree.h"

namespace {

// [OpenPak] The Online column with what Citron shows in it: the status, how many are playing
// right now, and a pill beside them when the servers take another version than the installed one.
class OnlineDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        const QString pill_text = PillText(index);
        if (pill_text.isEmpty()) {
            return;
        }

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
        const QRect text_rect =
            style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
        const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, &opt, opt.widget) + 1;

        QFont font = opt.font;
        font.setPointSize(std::max(font.pointSize() - 1, 7));
        font.setBold(true);
        const QRect pill(text_rect.left() + margin +
                             opt.fontMetrics.horizontalAdvance(opt.text) + pill_gap,
                         opt.rect.top() + std::max(0, (opt.rect.height() - pill_height) / 2),
                         QFontMetrics(font).horizontalAdvance(pill_text) + pill_padding,
                         pill_height);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setClipRect(opt.rect);
        const QColor color(0, 190, 255);
        QPainterPath path;
        path.addRoundedRect(pill, pill_height / 2.0, pill_height / 2.0);
        QColor fill = color;
        fill.setAlpha(38);
        painter->fillPath(path, fill);
        painter->setPen(QPen(color, 1.2));
        painter->drawPath(path);
        painter->setFont(font);
        painter->setPen(color);
        painter->drawText(pill, Qt::AlignCenter, pill_text);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        const QString pill_text = PillText(index);
        if (!pill_text.isEmpty()) {
            QFont font = option.font;
            font.setBold(true);
            size.rwidth() +=
                pill_gap + QFontMetrics(font).horizontalAdvance(pill_text) + pill_padding;
        }
        return size;
    }

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (!option->text.isEmpty()) {
            const u64 program_id = index.sibling(index.row(), GameListModel::COLUMN_NAME)
                                       .data(GameListItemPath::ProgramIdRole)
                                       .toULongLong();
            option->text = GameTree::tr("OpenPak %1 \u00B7 %2 online")
                               .arg(option->text)
                               .arg(OpenPak::OnlineCounts::For(program_id));
        } else if (IsNzp(index)) {
            option->text =
                GameTree::tr("OpenPak: %1 online").arg(OpenPak::NzpOnlineCount::Get());
            option->features |= QStyleOptionViewItem::HasDisplay;
        }
    }

private:
    static constexpr int pill_height = 20;
    static constexpr int pill_gap = 6;
    static constexpr int pill_padding = 16;

    // NZP is homebrew: no real title ID, so it is matched by its title instead.
    static bool IsNzp(const QModelIndex& index) {
        return Common::OpenPakAccount::IsLinked() &&
               index.sibling(index.row(), GameListModel::COLUMN_NAME)
                       .data(GameListItemPath::TitleRole)
                       .toString() == QStringLiteral("Nazi Zombies Portable");
    }

    static QString PillText(const QModelIndex& index) {
        const QString required =
            index.data(GameListItemOnline::RequiredVersionRole).toString();
        if (!required.isEmpty()) {
            return GameTree::tr("Requires %1").arg(required);
        }
        if (index.data(Qt::DisplayRole).toString().isEmpty() && IsNzp(index)) {
            return GameTree::tr("Latest");
        }
        return {};
    }
};

} // Anonymous namespace

GameTree::GameTree(QWidget* parent) : QTreeView{parent} {
    setAlternatingRowColors(true);
    setSelectionMode(QHeaderView::SingleSelection);
    setSelectionBehavior(QHeaderView::SelectRows);
    setVerticalScrollMode(QHeaderView::ScrollPerPixel);
    setHorizontalScrollMode(QHeaderView::ScrollPerPixel);
    setSortingEnabled(true);
    setEditTriggers(QHeaderView::NoEditTriggers);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setStyleSheet(QStringLiteral("QTreeView{ border: none; }"));

    connect(this, &QTreeView::expanded, this, &GameTree::OnItemExpanded);
    connect(this, &QTreeView::collapsed, this, &GameTree::OnItemExpanded);

    // [OpenPak] The player counts change under the list: draw it again as often as they are
    // polled.
    setItemDelegateForColumn(GameListModel::COLUMN_ONLINE, new OnlineDelegate(this));
    auto* online_timer = new QTimer(this);
    online_timer->setInterval(5000);
    connect(online_timer, &QTimer::timeout, this, [this] {
        if (isVisible() && !isColumnHidden(GameListModel::COLUMN_ONLINE)) {
            viewport()->update();
        }
    });
    online_timer->start();
}

void GameTree::SetModel(GameListModel* model) {
    QTreeView::setModel(model);
    LoadInterfaceLayout();
    UpdateColumnVisibility(model);
}

void GameTree::OnItemExpanded(const QModelIndex& item) {
    const auto type = item.data(GameListItem::TypeRole).value<GameListItemType>();
    const bool is_dir = type == GameListItemType::CustomDir || type == GameListItemType::SdmcDir ||
                        type == GameListItemType::UserNandDir ||
                        type == GameListItemType::SysNandDir;
    const bool is_fave = type == GameListItemType::Favorites;
    if (!is_dir && !is_fave) {
        return;
    }
    const bool is_expanded = isExpanded(item);
    if (is_fave) {
        UISettings::values.favorites_expanded = is_expanded;
        return;
    }
    const int item_dir_index = item.data(GameListDir::GameDirRole).toInt();
    UISettings::values.game_dirs[item_dir_index].expanded = is_expanded;
}

void GameTree::SaveInterfaceLayout() {
    UISettings::values.gamelist_header_state = header()->saveState();
}

void GameTree::LoadInterfaceLayout() {
    auto* hdr = header();

    if (hdr->restoreState(UISettings::values.gamelist_header_state))
        return;

    hdr->resizeSection(GameListModel::COLUMN_NAME, 840);
}

void GameTree::UpdateColumnVisibility(GameListModel* model) {
    Q_UNUSED(model)
    setColumnHidden(GameListModel::COLUMN_ADD_ONS, !UISettings::values.show_add_ons);
    setColumnHidden(GameListModel::COLUMN_FILE_TYPE, !UISettings::values.show_types);
    setColumnHidden(GameListModel::COLUMN_SIZE, !UISettings::values.show_size);
    setColumnHidden(GameListModel::COLUMN_PLAY_TIME, !UISettings::values.show_play_time);
    // [OpenPak] Only while OpenPak is on, as Ryujinx shows it.
    setColumnHidden(GameListModel::COLUMN_ONLINE, !Settings::values.enable_openpak.GetValue());
}

QString GameTree::GetLastFilterResultItem() const {
    QString file_path;

    auto* model = qobject_cast<GameListModel*>(QTreeView::model());
    if (!model)
        return {};

    for (int i = 1; i < model->rowCount() - 1; ++i) {
        const QStandardItem* folder = model->item(i, 0);
        const QModelIndex folder_index = folder->index();
        const int children_count = folder->rowCount();

        for (int j = 0; j < children_count; ++j) {
            if (isRowHidden(j, folder_index)) {
                continue;
            }

            const QStandardItem* child = folder->child(j, 0);
            file_path = child->data(GameListItemPath::FullPathRole).toString();
        }
    }

    return file_path;
}

int GameTree::FilterClosedResultCount(GameListModel* model) {
    int children_total = 0;

    auto hide_favorites_row = UISettings::values.favorited_ids.size() == 0;
    setRowHidden(0, model->invisibleRootItem()->index(), hide_favorites_row);

    for (int i = 1; i < model->rowCount() - 1; ++i) {
        auto* folder = model->item(i, 0);
        const QModelIndex folder_index = folder->index();
        const int children_count = folder->rowCount();
        for (int j = 0; j < children_count; ++j) {
            ++children_total;
            setRowHidden(j, folder_index, false);
        }
    }

    return children_total;
}

void GameTree::ApplyFilter(const QString& edit_filter_text, GameListModel* model) {
    int children_total = 0;
    int result_count = 0;

    if (edit_filter_text.isEmpty()) {
        children_total = FilterClosedResultCount(model);
        emit FilterResultReady(children_total, children_total);
        return;
    }

    setRowHidden(0, model->invisibleRootItem()->index(), true);

    for (int i = 1; i < model->rowCount() - 1; ++i) {
        auto* folder = model->item(i, 0);
        const QModelIndex folder_index = folder->index();
        const int children_count = folder->rowCount();

        for (int j = 0; j < children_count; ++j) {
            ++children_total;

            const QStandardItem* child = folder->child(j, 0);

            if (Yuzu::FilterMatches(edit_filter_text, child)) {
                setRowHidden(j, folder_index, false);
                ++result_count;
            } else {
                setRowHidden(j, folder_index, true);
            }
        }
    }

    emit FilterResultReady(result_count, children_total);
}
