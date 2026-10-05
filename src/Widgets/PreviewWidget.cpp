// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Widgets/PreviewWidget.h"

#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>

namespace
{
    constexpr qreal kPad = 16; // gap around and between pages, in device pixels
}

PreviewWidget::PreviewWidget(QWidget *parent) : QAbstractScrollArea(parent)
{
    viewport()->setBackgroundRole(QPalette::Dark);
    viewport()->setCursor(Qt::IBeamCursor);
    verticalScrollBar()->setSingleStep(40);
    horizontalScrollBar()->setSingleStep(40);
    setFocusPolicy(Qt::ClickFocus);

    // Copy and Select All are the window's Edit actions (see
    // setContextActions()); they act on the preview while it has focus.
}

void PreviewWidget::setPageLayout(
    std::shared_ptr<const scribe::PageLayout> layout)
{
    layout_ = std::move(layout);
    // Text positions belong to the old layout.
    selection_ = {};
    selecting_ = false;
    highlights_.clear();
    updateScrollBars();
    emit layoutChanged();
    emit selectionChanged();
}

void PreviewWidget::setHighlights(std::vector<scribe::Highlight> highlights)
{
    highlights_ = std::move(highlights);
    viewport()->update();
}

void PreviewWidget::showPoint(const scribe::TextPoint &point)
{
    if (!layout_ || !point.isValid())
    {
        return;
    }
    const QRectF caret = layout_->caretRect(point);
    if (caret.isEmpty() && caret.height() <= 0)
    {
        return;
    }
    const qreal s = pixelsPerPoint();
    const QRectF page = pageRect(point.page);
    const QRectF r(page.topLeft() + caret.topLeft() * s,
                   QSizeF(std::max(1.0, caret.width() * s), caret.height() * s));
    const QRectF view(viewport()->rect());
    if (!view.contains(r))
    {
        QScrollBar *v = verticalScrollBar();
        v->setValue(v->value() + static_cast<int>(r.center().y() - view.center().y()));
        if (r.left() < view.left() || r.right() > view.right())
        {
            QScrollBar *h = horizontalScrollBar();
            h->setValue(h->value() +
                        static_cast<int>(r.center().x() - view.center().x()));
        }
    }
    viewport()->update();
}

void PreviewWidget::scrollToPoint(const scribe::TextPoint &point)
{
    if (!layout_ || !point.isValid())
    {
        return;
    }
    const QRectF caret = layout_->caretRect(point);
    if (caret.isNull())
    {
        return;
    }
    const qreal top = pageRect(point.page).top() + caret.top() * pixelsPerPoint();
    QScrollBar *v = verticalScrollBar();
    v->setValue(v->value() + static_cast<int>(top - viewport()->height() / 8.0));
    viewport()->update();
}

int PreviewWidget::firstVisiblePage() const
{
    if (!layout_)
    {
        return 0;
    }
    const int pageCount = static_cast<int>(layout_->pages().size());
    for (int i = 0; i < pageCount; ++i)
    {
        if (pageRect(i).bottom() > 0)
        {
            return i;
        }
    }
    return std::max(0, pageCount - 1);
}

void PreviewWidget::setZoom(qreal zoom)
{
    zoom_ = std::clamp(zoom, 0.25, 4.0);
    updateScrollBars();
}

void PreviewWidget::copy()
{
    if (!layout_ || selection_.isEmpty())
    {
        return;
    }
    QGuiApplication::clipboard()->setText(layout_->selectedText(selection_));
}

void PreviewWidget::selectAll()
{
    if (!layout_)
    {
        return;
    }
    selection_ = layout_->selectAll();
    emit selectionChanged();
    viewport()->update();
}

void PreviewWidget::updateScrollBars()
{
    if (!layout_)
    {
        verticalScrollBar()->setRange(0, 0);
        horizontalScrollBar()->setRange(0, 0);
        viewport()->update();
        return;
    }

    const qreal s = pixelsPerPoint();
    const auto &spec = layout_->spec();
    const qreal pageW = spec.width * s;
    const qreal pageH = spec.height * s;
    const int pageCount = static_cast<int>(layout_->pages().size());

    const qreal contentW = pageW + 2 * kPad;
    const qreal contentH = kPad + pageCount * (pageH + kPad);

    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setRange(
        0, std::max(0, static_cast<int>(contentH) - viewport()->height()));
    horizontalScrollBar()->setPageStep(viewport()->width());
    horizontalScrollBar()->setRange(
        0, std::max(0, static_cast<int>(contentW) - viewport()->width()));
    viewport()->update();
}

QRectF PreviewWidget::pageRect(int index) const
{
    const qreal s = pixelsPerPoint();
    const auto &spec = layout_->spec();
    const qreal pageW = spec.width * s;
    const qreal pageH = spec.height * s;
    const qreal contentW = pageW + 2 * kPad;

    // Centre pages when they're narrower than the viewport, else honour the
    // scroll offset.
    const qreal x0 = (viewport()->width() >= contentW)
                         ? (viewport()->width() - pageW) / 2
                         : kPad - horizontalScrollBar()->value();
    const qreal y =
        kPad + index * (pageH + kPad) - verticalScrollBar()->value();
    return QRectF(x0, y, pageW, pageH);
}

scribe::TextPoint PreviewWidget::textAt(const QPoint &pos) const
{
    if (!layout_ || layout_->pages().empty())
    {
        return {};
    }
    // The page under the point, or the nearest one when it's in a gap or
    // outside the view (dragging past an edge).
    const int pageCount = static_cast<int>(layout_->pages().size());
    int page = 0;
    qreal bestDistance = -1;
    for (int i = 0; i < pageCount; ++i)
    {
        const QRectF r = pageRect(i);
        const qreal dy =
            std::max({r.top() - pos.y(), 0.0, pos.y() - r.bottom()});
        if (bestDistance < 0 || dy < bestDistance)
        {
            page = i;
            bestDistance = dy;
        }
    }
    const QRectF r = pageRect(page);
    const QPointF point = (QPointF(pos) - r.topLeft()) / pixelsPerPoint();
    return layout_->hitTest(page, point);
}

void PreviewWidget::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars();
}

void PreviewWidget::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier)
    {
        const int dy = event->angleDelta().y();
        if (dy != 0)
        {
            setZoom(zoom_ * (dy > 0 ? 1.1 : 1.0 / 1.1));
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void PreviewWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    const scribe::TextPoint point = textAt(event->position().toPoint());
    if (event->modifiers() & Qt::ShiftModifier && selection_.anchor.isValid())
    {
        selection_.focus = point; // extend the selection
    }
    else
    {
        selection_.anchor = point;
        selection_.focus = point;
    }
    selecting_ = point.isValid();
    emit selectionChanged();
    viewport()->update();
}

void PreviewWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!selecting_ || !(event->buttons() & Qt::LeftButton))
    {
        QAbstractScrollArea::mouseMoveEvent(event);
        return;
    }
    // Scroll when dragging past the top or bottom edge.
    const int y = event->position().toPoint().y();
    if (y < 0)
    {
        verticalScrollBar()->setValue(verticalScrollBar()->value() + y);
    }
    else if (y > viewport()->height())
    {
        verticalScrollBar()->setValue(verticalScrollBar()->value() + y -
                                      viewport()->height());
    }
    const scribe::TextPoint point = textAt(event->position().toPoint());
    if (point.isValid())
    {
        selection_.focus = point;
        emit selectionChanged();
    }
    viewport()->update();
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        selecting_ = false;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void PreviewWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !layout_)
    {
        QAbstractScrollArea::mouseDoubleClickEvent(event);
        return;
    }
    const scribe::TextPoint point = textAt(event->position().toPoint());
    if (point.isValid())
    {
        selection_ = layout_->wordAt(point);
        emit selectionChanged();
        viewport()->update();
    }
}

void PreviewWidget::contextMenuEvent(QContextMenuEvent *event)
{
    setFocus(); // the Edit actions act on the focused side
    QMenu menu(this);
    menu.addActions(contextActions_);
    menu.addSeparator();
    // No shortcut of its own: Edit > Find (Ctrl+F) already searches the
    // preview while it has focus.
    menu.addAction(tr("&Find..."), this, &PreviewWidget::findRequested);
    menu.exec(event->globalPos());
}

void PreviewWidget::paintEvent(QPaintEvent *)
{
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), palette().color(QPalette::Dark));
    if (!layout_)
    {
        return;
    }

    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    const qreal s = pixelsPerPoint();
    const int pageCount = static_cast<int>(layout_->pages().size());
    for (int i = 0; i < pageCount; ++i)
    {
        const QRectF rect = pageRect(i);
        if (!rect.intersects(QRectF(viewport()->rect())))
        {
            continue;
        }

        p.fillRect(rect.translated(3, 3),
                   QColor(0, 0, 0, 70)); // drop shadow

        p.save();
        p.translate(rect.topLeft());
        p.scale(s, s);
        layout_->paintPage(p, i, &selection_, &highlights_);
        p.restore();
    }
}
