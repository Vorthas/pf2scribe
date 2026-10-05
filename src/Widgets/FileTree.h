// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include <QFileSystemWatcher>
#include <QSet>
#include <QTimer>
#include <QWidget>

class QLabel;
class QStandardItem;
class QStandardItemModel;
class QTreeView;

// The markdown files of a folder, as a tree: only .md files, and only the
// subfolders that contain some. Follows changes on disk.
class FileTree : public QWidget
{
    Q_OBJECT
    public:
    explicit FileTree(QWidget *parent = nullptr);

    QString root() const { return root_; }
    void setRoot(const QString &folder);
    // Highlights the open file (and opens the folders above it).
    void setCurrentFile(const QString &path);
    void focusTree();

    signals:
    // A file was clicked (focusEditor: false) or activated with Enter /
    // double-click (true).
    void fileChosen(const QString &path, bool focusEditor);
    void openFolderRequested();
    void leaveRequested(); // Esc: back to the editor

    protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

    private:
    void rebuild();
    bool addFolder(QStandardItem *parent, const QString &folder);
    QStandardItem *itemFor(const QString &path) const;

    QTreeView *view_;
    QStandardItemModel *model_;
    QLabel *title_;
    QFileSystemWatcher watcher_;
    QTimer rebuildTimer_;
    QString root_;
    QString current_;
    QSet<QString> expanded_; // folder paths, kept across rebuilds
};
