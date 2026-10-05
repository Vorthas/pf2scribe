// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include "Rendering/PageLayout.h"

#include <QAbstractScrollArea>

#include <memory>
#include <vector>

class QAction;

// Scrollable view that paints laid-out pages one above the other. Text in it
// can be selected with the mouse and copied as plain text.
class PreviewWidget : public QAbstractScrollArea
{
    Q_OBJECT
    public:
    explicit PreviewWidget(QWidget *parent = nullptr);

    void setPageLayout(std::shared_ptr<const scribe::PageLayout> layout);
    std::shared_ptr<const scribe::PageLayout> pageLayout() const
    {
        return layout_;
    }

    // Marked text besides the selection (find matches).
    void setHighlights(std::vector<scribe::Highlight> highlights);
    // Scrolls so that `point` is in view; centred if it was out of view.
    void showPoint(const scribe::TextPoint &point);
    // Scrolls so that `point` is near the top of the view (a heading, with
    // what follows it below).
    void scrollToPoint(const scribe::TextPoint &point);
    // The first page showing in the view.
    int firstVisiblePage() const;
    const scribe::TextSelection &selection() const { return selection_; }

    qreal zoom() const { return zoom_; }
    void setZoom(qreal zoom);

    // Copies the selected text to the clipboard as plain text.
    void copy();
    void selectAll();
    bool hasSelection() const { return !selection_.isEmpty(); }
    // Shown first in the right-click menu (Edit > Copy, Select All).
    void setContextActions(const QList<QAction *> &actions)
    {
        contextActions_ = actions;
    }

    signals:
    void layoutChanged();   // a new layout is shown
    void selectionChanged();
    void findRequested();   // "Find..." in the context menu

    protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

    private:
    qreal pixelsPerPoint() const { return zoom_ * logicalDpiX() / 72.0; }
    void updateScrollBars();
    // Rectangle of page `index` in viewport pixels.
    QRectF pageRect(int index) const;
    // Text position at viewport point `pos`, on the page nearest to it.
    scribe::TextPoint textAt(const QPoint &pos) const;

    std::shared_ptr<const scribe::PageLayout> layout_;
    qreal zoom_ = 1.0;
    scribe::TextSelection selection_;
    std::vector<scribe::Highlight> highlights_;
    bool selecting_ = false;
    QList<QAction *> contextActions_;
};
