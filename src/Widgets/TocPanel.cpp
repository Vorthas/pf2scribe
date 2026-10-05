// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Widgets/TocPanel.h"

#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

#include <functional>

namespace
{
    constexpr int kEntryRole = Qt::UserRole + 1; // index into the entries
} // namespace

TocPanel::TocPanel(QWidget *parent) : QWidget(parent)
{
    auto *title = new QLabel(tr("Contents"));
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);

    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter"));
    filter_->setClearButtonEnabled(true);
    filter_->installEventFilter(this);

    model_ = new QStandardItemModel(this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setRecursiveFilteringEnabled(true); // keep the parents of matches
    view_ = new QTreeView;
    view_->setModel(proxy_);
    view_->setHeaderHidden(true);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setUniformRowHeights(true);
    view_->setExpandsOnDoubleClick(false); // double-click goes to the editor
    view_->installEventFilter(this);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto *header = new QVBoxLayout;
    header->setContentsMargins(6, 4, 2, 2);
    header->setSpacing(4);
    header->addWidget(title);
    header->addWidget(filter_);
    layout->addLayout(header);
    layout->addWidget(view_);

    connect(view_, &QTreeView::clicked, this,
            [this](const QModelIndex &index) { choose(index, false); });
    connect(view_, &QTreeView::doubleClicked, this,
            [this](const QModelIndex &index) { choose(index, true); });
    connect(view_, &QTreeView::expanded, this, [this](const QModelIndex &index) {
        if (filter_->text().isEmpty())
        {
            expanded_.insert(pathOf(index));
        }
    });
    connect(view_, &QTreeView::collapsed, this, [this](const QModelIndex &index) {
        if (filter_->text().isEmpty())
        {
            expanded_.remove(pathOf(index));
        }
    });
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString &text) {
        proxy_->setFilterFixedString(text);
        if (!text.isEmpty())
        {
            view_->expandAll(); // show every match
            return;
        }
        // Back to the folders the user had open.
        std::function<void(const QModelIndex &)> restore =
            [&](const QModelIndex &parent) {
                for (int r = 0; r < proxy_->rowCount(parent); ++r)
                {
                    const QModelIndex index = proxy_->index(r, 0, parent);
                    view_->setExpanded(index, expanded_.contains(pathOf(index)));
                    restore(index);
                }
            };
        restore({});
    });
}

QString TocPanel::pathOf(const QModelIndex &index) const
{
    QStringList parts;
    for (QModelIndex i = index; i.isValid(); i = i.parent())
    {
        parts.prepend(i.data().toString());
    }
    return parts.join(QLatin1Char('/'));
}

void TocPanel::setContents(const QList<scribe::TocEntry> &entries,
                           const std::vector<scribe::TextPoint> &points)
{
    points_ = points;
    lines_.clear();
    model_->clear();

    // Each entry goes under the last one with fewer "+" before it.
    std::vector<std::pair<int, QStandardItem *>> parents; // level, item
    for (int i = 0; i < entries.size(); ++i)
    {
        const scribe::TocEntry &entry = entries.at(i);
        lines_ << entry.line;
        auto *item = new QStandardItem(entry.text);
        item->setData(i, kEntryRole);
        while (!parents.empty() && parents.back().first >= entry.level)
        {
            parents.pop_back();
        }
        (parents.empty() ? model_->invisibleRootItem() : parents.back().second)
            ->appendRow(item);
        parents.emplace_back(entry.level, item);
    }

    // The first time, open the top level; after that, what the user had.
    if (!filled_)
    {
        for (int r = 0; r < model_->rowCount(); ++r)
        {
            expanded_.insert(model_->item(r)->text());
        }
        filled_ = !entries.isEmpty();
    }
    if (!filter_->text().isEmpty())
    {
        view_->expandAll();
        return;
    }
    std::function<void(const QModelIndex &)> restore = [&](const QModelIndex &parent) {
        for (int r = 0; r < proxy_->rowCount(parent); ++r)
        {
            const QModelIndex index = proxy_->index(r, 0, parent);
            if (expanded_.contains(pathOf(index)))
            {
                view_->expand(index);
            }
            restore(index);
        }
    };
    restore({});
}

void TocPanel::focusTree()
{
    view_->setFocus();
    if (!view_->currentIndex().isValid() && proxy_->rowCount() > 0)
    {
        view_->setCurrentIndex(proxy_->index(0, 0));
    }
}

void TocPanel::choose(const QModelIndex &index, bool toEditor)
{
    if (!index.isValid())
    {
        return;
    }
    const int entry = index.data(kEntryRole).toInt();
    if (entry >= 0 && entry < static_cast<int>(points_.size()) &&
        points_[static_cast<size_t>(entry)].isValid())
    {
        emit entryChosen(points_[static_cast<size_t>(entry)]);
    }
    if (toEditor && entry >= 0 && entry < lines_.size() && lines_.at(entry) >= 0)
    {
        emit lineChosen(lines_.at(entry));
    }
}

// Enter shows the entry, Esc goes back to the editor; Down in the filter
// moves into the tree.
bool TocPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::KeyPress)
    {
        return QWidget::eventFilter(watched, event);
    }
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Escape)
    {
        if (watched == filter_ && !filter_->text().isEmpty())
        {
            filter_->clear();
        }
        else
        {
            emit leaveRequested();
        }
        return true;
    }
    if (watched == view_ && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter))
    {
        choose(view_->currentIndex(), key->modifiers() & Qt::ShiftModifier);
        return true;
    }
    if (watched == filter_ && (key->key() == Qt::Key_Down ||
                               key->key() == Qt::Key_Return ||
                               key->key() == Qt::Key_Enter))
    {
        focusTree();
        if (key->key() != Qt::Key_Down)
        {
            choose(view_->currentIndex(), false);
        }
        return true;
    }
    return QWidget::eventFilter(watched, event);
}
