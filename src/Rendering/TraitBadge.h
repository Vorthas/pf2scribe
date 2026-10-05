// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include <QColor>
#include <QObject>
#include <QTextObjectInterface>

class QTextBlock;
class QTextDocument;

namespace scribe
{
    // An item trait ("UNCOMMON", "FEAT", ...) drawn inline as a filled badge
    // with a border; the first and last of a row also carry the plain end
    // caps that close it, so a cap never wraps onto a line alone. The
    // badge's text, font and colours travel in its QTextCharFormat, so one
    // handler serves every document.
    class TraitBadge : public QObject, public QTextObjectInterface
    {
        Q_OBJECT
        Q_INTERFACES(QTextObjectInterface)

        public:
        static constexpr int kObjectType = QTextFormat::UserObject + 1;

        // Sizes, in points (".pf-trait" in the site's stylesheet).
        struct Metrics
        {
            qreal padTop = 0;
            qreal padBottom = 0;
            qreal padX = 0;
            qreal borderY = 0;
            qreal borderX = 0;
            qreal fontSize = 0;
            qreal capWidth = 0; // end caps (".pf-trait-edge")

            qreal height() const
            {
                return 2 * borderY + padTop + fontSize + padBottom;
            }
        };

        // Makes `doc` draw badges; call once per document.
        static void install(QTextDocument &doc);

        // Format for one badge. `font` should carry the family, size and
        // weight; end caps in `border` colour on the sides asked for.
        static QTextCharFormat format(const QString &text, const QFont &font,
                                      const QColor &fill, const QColor &border,
                                      const QColor &textColor,
                                      const Metrics &metrics,
                                      bool capLeft = false,
                                      bool capRight = false);

        // The badge's text.
        static QString text(const QTextFormat &format);
        // The space either side of the text, in points, as made.
        static qreal padX(const QTextFormat &format);
        // Width in layout units with that space.
        static qreal naturalWidth(const QTextDocument *doc,
                                  const QTextFormat &format);
        // Narrower space for every badge in `row`, so it fits its line;
        // negative: as made. Takes effect at its next layout.
        static void setRowPadX(QTextBlock row, qreal padX);

        QSizeF intrinsicSize(QTextDocument *doc, int posInDocument,
                             const QTextFormat &format) override;
        void drawObject(QPainter *painter, const QRectF &rect,
                        QTextDocument *doc, int posInDocument,
                        const QTextFormat &format) override;
    };
} // namespace scribe
