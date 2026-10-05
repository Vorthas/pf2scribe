// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include "Rendering/PageLayout.h"
#include "Rendering/ScribeParser.h"

#include <QSet>
#include <QWidget>

#include <vector>

class QLineEdit;
class QModelIndex;
class QSortFilterProxyModel;
class QStandardItem;
class QStandardItemModel;
class QTreeView;

// The document's table of contents, from its ((...)) heading markers, as a
// tree nested by the number of "+": click an entry to show its heading in
// the preview, double-click to also go to it in the editor. Part of the
// window only; the exported PDF gets bookmarks instead.
class TocPanel : public QWidget
{
    Q_OBJECT
    public:
    explicit TocPanel(QWidget *parent = nullptr);

    // The entries of the render being shown and where their headings are.
    void setContents(const QList<scribe::TocEntry> &entries,
                     const std::vector<scribe::TextPoint> &points);
    void focusTree();
    // Another document: forget which entries were open.
    void forgetExpanded() { expanded_.clear(); filled_ = false; }

    signals:
    void entryChosen(const scribe::TextPoint &point); // click, Enter
    void lineChosen(int line);                         // double-click
    void leaveRequested();                             // Esc

    protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

    private:
    QString pathOf(const QModelIndex &index) const;
    void choose(const QModelIndex &index, bool toEditor);

    QLineEdit *filter_;
    QTreeView *view_;
    QStandardItemModel *model_;
    QSortFilterProxyModel *proxy_;
    std::vector<scribe::TextPoint> points_;
    QList<int> lines_;
    QSet<QString> expanded_; // entry paths ("A/Ache"), kept across renders
    bool filled_ = false;
};
