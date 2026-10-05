// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Widgets/FileTree.h"

#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QStandardItemModel>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <functional>

namespace
{
    constexpr int kPathRole = Qt::UserRole + 1;
    constexpr int kIsFolderRole = Qt::UserRole + 2;

    bool isMarkdown(const QFileInfo &info)
    {
        return info.isFile() &&
               info.suffix().compare(QLatin1String("md"), Qt::CaseInsensitive) == 0;
    }

} // namespace

FileTree::FileTree(QWidget *parent) : QWidget(parent)
{
    title_ = new QLabel;
    title_->setTextFormat(Qt::PlainText);
    QFont bold = title_->font();
    bold.setBold(true);
    title_->setFont(bold);
    auto *openFolder = new QToolButton;
    openFolder->setIcon(QFileIconProvider().icon(QFileIconProvider::Folder));
    openFolder->setAutoRaise(true);
    openFolder->setToolTip(tr("Open Folder..."));
    connect(openFolder, &QToolButton::clicked, this, &FileTree::openFolderRequested);

    model_ = new QStandardItemModel(this);
    view_ = new QTreeView;
    view_->installEventFilter(this);
    view_->setModel(model_);
    view_->setHeaderHidden(true);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setUniformRowHeights(true);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(6, 4, 2, 2);
    header->addWidget(title_, 1);
    header->addWidget(openFolder);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(view_);

    auto choose = [this](const QModelIndex &index, bool focusEditor) {
        if (!index.isValid() || index.data(kIsFolderRole).toBool())
        {
            return;
        }
        emit fileChosen(index.data(kPathRole).toString(), focusEditor);
    };
    connect(view_, &QTreeView::clicked, this,
            [choose](const QModelIndex &index) { choose(index, false); });
    connect(view_, &QTreeView::activated, this,
            [choose](const QModelIndex &index) { choose(index, true); });
    connect(view_, &QTreeView::expanded, this, [this](const QModelIndex &index) {
        expanded_.insert(index.data(kPathRole).toString());
    });
    connect(view_, &QTreeView::collapsed, this, [this](const QModelIndex &index) {
        expanded_.remove(index.data(kPathRole).toString());
    });

    // Rebuild a moment after files are added, renamed or removed.
    rebuildTimer_.setSingleShot(true);
    rebuildTimer_.setInterval(300);
    connect(&rebuildTimer_, &QTimer::timeout, this, &FileTree::rebuild);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, &rebuildTimer_,
            qOverload<>(&QTimer::start));

    title_->setText(tr("No folder"));
}

void FileTree::setRoot(const QString &folder)
{
    const QString path = QDir(folder).absolutePath();
    if (path == root_)
    {
        return;
    }
    root_ = path;
    expanded_.clear();
    title_->setText(QDir(root_).dirName().isEmpty() ? root_ : QDir(root_).dirName());
    title_->setToolTip(QDir::toNativeSeparators(root_));
    rebuild();
}

void FileTree::rebuild()
{
    if (!watcher_.directories().isEmpty())
    {
        watcher_.removePaths(watcher_.directories());
    }
    model_->clear();
    if (root_.isEmpty() || !QFileInfo(root_).isDir())
    {
        return;
    }
    addFolder(model_->invisibleRootItem(), root_);

    // Restore the open folders; the first time, open the top level.
    const bool first = expanded_.isEmpty();
    const QSet<QString> wanted = expanded_;
    std::function<void(QStandardItem *, bool)> restore = [&](QStandardItem *item,
                                                             bool topLevel) {
        for (int r = 0; r < item->rowCount(); ++r)
        {
            QStandardItem *child = item->child(r);
            if (!child->data(kIsFolderRole).toBool())
            {
                continue;
            }
            if (wanted.contains(child->data(kPathRole).toString()) ||
                (first && topLevel))
            {
                view_->expand(child->index());
            }
            restore(child, false);
        }
    };
    restore(model_->invisibleRootItem(), true);
    if (!current_.isEmpty())
    {
        setCurrentFile(current_);
    }
}

// Adds the .md files and the folders holding some under `folder`; false if
// there are none.
bool FileTree::addFolder(QStandardItem *parent, const QString &folder)
{
    watcher_.addPath(folder);
    const QFileIconProvider icons;
    QDir dir(folder);
    const QFileInfoList folders =
        dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    const QFileInfoList files =
        dir.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase);
    bool any = false;
    for (const QFileInfo &info : folders)
    {
        if (info.fileName().startsWith(QLatin1Char('.')) || info.isSymLink())
        {
            continue; // hidden folders, and links that could loop
        }
        auto *item = new QStandardItem(icons.icon(QFileIconProvider::Folder),
                                       info.fileName());
        item->setData(info.absoluteFilePath(), kPathRole);
        item->setData(true, kIsFolderRole);
        if (addFolder(item, info.absoluteFilePath()))
        {
            parent->appendRow(item);
            any = true;
        }
        else
        {
            delete item;
        }
    }
    for (const QFileInfo &info : files)
    {
        if (!isMarkdown(info))
        {
            continue;
        }
        auto *item = new QStandardItem(icons.icon(QFileIconProvider::File),
                                       info.fileName());
        item->setData(info.absoluteFilePath(), kPathRole);
        item->setData(false, kIsFolderRole);
        item->setToolTip(QDir::toNativeSeparators(info.absoluteFilePath()));
        parent->appendRow(item);
        any = true;
    }
    return any;
}

QStandardItem *FileTree::itemFor(const QString &path) const
{
    std::function<QStandardItem *(QStandardItem *)> find =
        [&](QStandardItem *item) -> QStandardItem * {
        for (int r = 0; r < item->rowCount(); ++r)
        {
            QStandardItem *child = item->child(r);
            if (child->data(kPathRole).toString() == path)
            {
                return child;
            }
            if (QStandardItem *found = find(child))
            {
                return found;
            }
        }
        return nullptr;
    };
    return find(model_->invisibleRootItem());
}

void FileTree::setCurrentFile(const QString &path)
{
    current_ = QFileInfo(path).absoluteFilePath();
    QStandardItem *item = itemFor(current_);
    if (!item)
    {
        view_->clearSelection();
        return;
    }
    for (QStandardItem *p = item->parent(); p; p = p->parent())
    {
        view_->expand(p->index());
    }
    view_->setCurrentIndex(item->index());
    view_->scrollTo(item->index());
}

void FileTree::focusTree()
{
    view_->setFocus();
}

// Enter opens the file and moves to the editor; Esc goes back to it.
bool FileTree::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == view_ && event->type() == QEvent::KeyPress)
    {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            const QModelIndex index = view_->currentIndex();
            if (index.data(kIsFolderRole).toBool())
            {
                view_->setExpanded(index, !view_->isExpanded(index));
            }
            else if (index.isValid())
            {
                emit fileChosen(index.data(kPathRole).toString(), true);
            }
            return true;
        }
        if (key->key() == Qt::Key_Escape)
        {
            emit leaveRequested();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
