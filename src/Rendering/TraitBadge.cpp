// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Rendering/TraitBadge.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QPainter>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

namespace scribe
{
    namespace
    {
        enum Property
        {
            Text = QTextFormat::UserProperty + 20,
            Fill,
            Border,
            TextColor,
            PadTop,
            PadBottom,
            PadX,
            BorderY,
            BorderX,
            FontSize,
            CapWidth,
            CapLeft,
            CapRight
        };

        // A row's narrower space beside the text (see setRowPadX()). Block
        // user data, because changing it must not make Qt lay the document
        // out again: Theme::reflow() sets it on a laid-out page.
        struct RowPadX : QTextBlockUserData
        {
            qreal padX = 0;
        };

        // Layout units per point of the document's paint device.
        qreal unitsPerPoint(const QTextDocument *doc)
        {
            const QPaintDevice *device = doc->documentLayout()->paintDevice();
            return device ? device->logicalDpiY() / 72.0 : 1.0;
        }

        TraitBadge::Metrics metricsOf(const QTextFormat &f)
        {
            TraitBadge::Metrics m;
            m.padTop = f.doubleProperty(PadTop);
            m.padBottom = f.doubleProperty(PadBottom);
            m.padX = f.doubleProperty(PadX);
            m.borderY = f.doubleProperty(BorderY);
            m.borderX = f.doubleProperty(BorderX);
            m.fontSize = f.doubleProperty(FontSize);
            m.capWidth = f.doubleProperty(CapWidth);
            return m;
        }

        // Width of the caps a badge carries, in points.
        qreal capsWidth(const QTextFormat &f, const TraitBadge::Metrics &m)
        {
            return m.capWidth * ((f.boolProperty(CapLeft) ? 1 : 0) +
                                 (f.boolProperty(CapRight) ? 1 : 0));
        }

        qreal widthWith(const QTextDocument *doc, const QTextFormat &format,
                        qreal padX)
        {
            const TraitBadge::Metrics m = metricsOf(format);
            const QFontMetricsF metrics(format.toCharFormat().font(),
                                        doc->documentLayout()->paintDevice());
            return metrics.horizontalAdvance(format.stringProperty(Text)) +
                   (2 * (padX + m.borderX) + capsWidth(format, m)) *
                       unitsPerPoint(doc);
        }
    } // namespace

    void TraitBadge::install(QTextDocument &doc)
    {
        static TraitBadge handler;
        doc.documentLayout()->registerHandler(kObjectType, &handler);
    }

    QString TraitBadge::text(const QTextFormat &format)
    {
        return format.stringProperty(Text);
    }

    qreal TraitBadge::padX(const QTextFormat &format)
    {
        return format.doubleProperty(PadX);
    }

    qreal TraitBadge::naturalWidth(const QTextDocument *doc,
                                   const QTextFormat &format)
    {
        return widthWith(doc, format, padX(format));
    }

    void TraitBadge::setRowPadX(QTextBlock row, qreal padX)
    {
        if (padX < 0)
        {
            row.setUserData(nullptr);
            return;
        }
        auto *data = new RowPadX;
        data->padX = padX;
        row.setUserData(data);
    }

    QTextCharFormat TraitBadge::format(const QString &text, const QFont &font,
                                       const QColor &fill, const QColor &border,
                                       const QColor &textColor,
                                       const Metrics &metrics, bool capLeft,
                                       bool capRight)
    {
        QTextCharFormat f;
        f.setObjectType(kObjectType);
        f.setFont(font);
        f.setProperty(Text, text);
        f.setProperty(Fill, fill);
        f.setProperty(Border, border);
        f.setProperty(TextColor, textColor);
        f.setProperty(PadTop, metrics.padTop);
        f.setProperty(PadBottom, metrics.padBottom);
        f.setProperty(PadX, metrics.padX);
        f.setProperty(BorderY, metrics.borderY);
        f.setProperty(BorderX, metrics.borderX);
        f.setProperty(FontSize, metrics.fontSize);
        f.setProperty(CapWidth, metrics.capWidth);
        f.setProperty(CapLeft, capLeft);
        f.setProperty(CapRight, capRight);
        return f;
    }

    QSizeF TraitBadge::intrinsicSize(QTextDocument *doc, int posInDocument,
                                     const QTextFormat &format)
    {
        qreal padX = metricsOf(format).padX;
        if (const auto *row = dynamic_cast<const RowPadX *>(
                doc->findBlock(posInDocument).userData()))
        {
            padX = row->padX;
        }
        return QSizeF(widthWith(doc, format, padX),
                      metricsOf(format).height() * unitsPerPoint(doc));
    }

    void TraitBadge::drawObject(QPainter *painter, const QRectF &rect,
                                QTextDocument *doc, int,
                                const QTextFormat &format)
    {
        const qreal units = unitsPerPoint(doc);
        const Metrics m = metricsOf(format);
        const QString text = format.stringProperty(Text);
        const QColor border = format.colorProperty(Border);

        painter->save();
        painter->setPen(Qt::NoPen);

        // The caps, then the border as a frame around the fill, like a CSS
        // border. Both are in the border colour, so one fill does.
        painter->fillRect(rect, border);
        const qreal cap = m.capWidth * units;
        const QRectF badge =
            rect.adjusted(format.boolProperty(CapLeft) ? cap : 0, 0,
                          format.boolProperty(CapRight) ? -cap : 0, 0);
        const QRectF inner =
            badge.adjusted(m.borderX * units, m.borderY * units,
                           -m.borderX * units, -m.borderY * units);
        painter->fillRect(inner, format.colorProperty(Fill));

        // The text goes through a QTextLayout on the document's own paint
        // device, like the document's text, so it comes out the same on
        // screen and in a PDF. "line-height: 1", centred on the content box.
        const QFont font = format.toCharFormat().font();
        QTextLayout layout(text, font, doc->documentLayout()->paintDevice());
        layout.beginLayout();
        QTextLine line = layout.createLine();
        layout.endLayout();
        const qreal contentTop = inner.top() + m.padTop * units;
        const qreal lineBox = m.fontSize * units;
        const qreal y =
            contentTop + (lineBox - line.ascent() - line.descent()) / 2;
        const qreal x =
            inner.left() + (inner.width() - line.naturalTextWidth()) / 2;
        painter->setPen(format.colorProperty(TextColor));
        layout.draw(painter, QPointF(x, y));
        painter->restore();
    }
} // namespace scribe
