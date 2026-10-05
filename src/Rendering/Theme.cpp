// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Rendering/Theme.h"

#include "Rendering/ScribeParser.h"
#include "Rendering/TraitBadge.h"

#include <QAbstractTextDocumentLayout>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextList>
#include <QTextTable>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace scribe
{
    namespace
    {
        // Block properties.
        // How far to move the block's lines down once laid out, in layout
        // units. See Theme::finishLayout().
        constexpr int kBaselineShift = QTextFormat::UserProperty + 1;
        // Item title: "\t" separates the name from the right-aligned level.
        constexpr int kRightTab = QTextFormat::UserProperty + 2;
        // A row of trait badges.
        constexpr int kTraitsRow = QTextFormat::UserProperty + 3;
        // "#### 1st Level" bar: its height in points.
        constexpr int kFeatBar = QTextFormat::UserProperty + 4;
        // A sidebar list item, taken out of its list (see apply()).
        constexpr int kSidebarItem = QTextFormat::UserProperty + 5;
        // A heading's negative bottom margin in points; see trailingMargin().
        constexpr int kNegativeBottom = QTextFormat::UserProperty + 6;

        // Where list text starts ("ul { padding-left: 2rem }"); the bullet
        // sits in front of it.
        constexpr qreal kListIndent = 2 * kRem;

        struct HeadingStyle
        {
            QStringList families;
            QString styleName; // empty: pick by weight and slant only
            qreal size = kRem;
            QColor color;
            Qt::Alignment align = Qt::AlignLeft;
            bool bold = false;
            QFont::Capitalization caps = QFont::MixedCase;
            qreal lineHeight = 1.0;
            // The site gives these a negative bottom margin, so the paragraph
            // that follows sits right against them.
            bool tightAfter = false;
            qreal marginTop = 0.5 * kRem; // "* + h1"
            // After a paragraph or list; negative: marginTop.
            qreal marginTopAfterText = -1;
            qreal paddingTop = 0; // adds to the gap, never collapses
            qreal marginBottom = 0;
            int stretch = 100;       // horizontal scale, percent
            qreal letterSpacing = 0; // extra space between letters, pt
        };

        struct RoleStyle
        {
            QStringList families;
            qreal pointSize = 0.925 * kRem;
            QColor color;
            Qt::Alignment align = Qt::AlignJustify;
            qreal lineHeight = 1.4;
            qreal paraTop = 0.5 * kRem;
            qreal paraBottom = 0;     // "p { margin: 0 }", "* + p" adds the top
            qreal paraAfterPara = -1; // negative: use paraTop
            bool italic = false;
            bool bold = false;
            HeadingStyle headings[7]; // index 1..6
        };

        HeadingStyle makeHeading(const QStringList &families, qreal rem,
                                 const QColor &color,
                                 Qt::Alignment align = Qt::AlignLeft,
                                 bool bold = false,
                                 QFont::Capitalization caps = QFont::MixedCase)
        {
            HeadingStyle h;
            h.families = families;
            h.size = rem * kRem;
            h.color = color;
            h.align = align;
            h.bold = bold;
            h.caps = caps;
            return h;
        }

        // Line spacing of Paizo's rulebooks, in points.
        constexpr qreal kBookLeading = 12;

        RoleStyle makeStyle(const Theme &t, Role role)
        {
            RoleStyle s;
            s.families = t.bodyFamilies;
            s.color = t.text;
            for (int level = 1; level <= 6; ++level)
            {
                s.headings[level] = makeHeading(t.bodyFamilies, 1.0, t.text);
            }

            const Qt::Alignment left = Qt::AlignLeft;
            const Qt::Alignment center = Qt::AlignHCenter;
            const Qt::Alignment right = Qt::AlignRight;
            const QFont::Capitalization upper = QFont::AllUppercase;

            // Shorthand for "tight after, with this line height".
            auto tight = [&s](int level, qreal lineHeight = 1.0) {
                s.headings[level].tightAfter = true;
                s.headings[level].lineHeight = lineHeight;
            };

            switch (role)
            {
            case Role::Body:
                s.headings[1] =
                    makeHeading(t.tarocaFamilies, 1.75, t.contentH1);
                tight(1);
                if (t.goodProHeadings)
                {
                    s.headings[2] = makeHeading(t.goodFamilies, 1.4,
                                                t.contentH2, left, true);
                    // "transform: scaleX(0.8); letter-spacing: 0.1rem",
                    // the spacing scaled along with the glyphs.
                    s.headings[3] = makeHeading(t.goodFamilies, 1.3,
                                                t.contentH3, left, true);
                    s.headings[3].stretch = 80;
                    s.headings[3].letterSpacing = 0.8 * 0.1 * kRem;
                }
                else
                {
                    s.headings[2] =
                        makeHeading(t.ginFamilies, 1.4, t.contentH2);
                    s.headings[3] =
                        makeHeading(t.ginFamilies, 1.3, t.contentH3, left,
                                    false, QFont::SmallCaps);
                }
                s.headings[4] = makeHeading(t.ginFamilies, 1.1, t.featText);
                s.headings[5] = makeHeading(t.goodCondensedFamilies, 1.4,
                                            t.text, left, true, upper);
                break;
            case Role::Head:
                s.families = t.bodyFamilies;
                s.pointSize = 1.2 * kRem;
                s.color = t.headText;
                s.italic = true;
                // Taroca's lower case is already small caps.
                s.headings[1] =
                    makeHeading(t.tarocaFamilies, 2.25, t.headText, center);
                s.headings[2] =
                    makeHeading(t.tarocaFamilies, 2.5, t.headText, left);
                s.headings[3] =
                    makeHeading(t.tarocaFamilies, 2.25, t.headH3, center);
                s.headings[4] =
                    makeHeading(t.tarocaFamilies, 2.5, t.navy, center);
                s.headings[5] =
                    makeHeading(t.tarocaFamilies, 1.0, t.headText, center);
                s.headings[6] =
                    makeHeading(t.tarocaFamilies, 1.0, t.headText, center);
                for (int level = 1; level <= 6; ++level)
                {
                    tight(level);
                }
                break;
            case Role::Info:
                s.families = t.goodFamilies;
                s.color = Qt::white;
                s.headings[1] =
                    makeHeading(t.tarocaFamilies, 2.5, Qt::white, center);
                tight(1);
                s.headings[2] = makeHeading(t.goodFamilies, 0.925, Qt::white,
                                            left, true, upper);
                tight(2, 1.5);
                s.headings[3] =
                    makeHeading(t.tarocaFamilies, 1.25, t.infoAccent, center);
                tight(3);
                s.headings[4] =
                    makeHeading(t.ginFamilies, 1.4, Qt::white, center);
                s.headings[5] = makeHeading(t.goodFamilies, 1.0, Qt::white,
                                            center, true, upper);
                tight(5, 1.5);
                s.headings[6] =
                    makeHeading(t.ginFamilies, 1.4, Qt::white, left);
                break;
            case Role::Note:
                s.families = t.goodFamilies;
                s.color = t.noteText;
                for (int level = 1; level <= 6; ++level)
                {
                    s.headings[level] =
                        makeHeading(t.goodFamilies, 1.0, t.noteText, center, true,
                                    level == 1 ? upper : QFont::MixedCase);
                    tight(level, 1.5);
                }
                break;
            case Role::Rules:
                s.families = t.goodFamilies;
                s.pointSize = kRem;
                s.headings[1] =
                    makeHeading(t.goodFamilies, 1.0, t.text, left, true);
                tight(1, 1.5);
                s.headings[2] =
                    makeHeading(t.goodFamilies, 1.0, t.text, center, true);
                tight(2, 1.5);
                break;
            case Role::Math:
                s.families = t.goodFamilies;
                s.pointSize = kRem;
                s.align = center;
                s.bold = true;
                break;
            case Role::Item:
                s.families = t.goodFamilies;
                s.pointSize = kRem;
                s.paraTop = 0;
                s.paraBottom = 0;
                s.paraAfterPara = kRem;
                s.headings[1] = makeHeading(t.goodCondensedFamilies, 1.4,
                                            t.text, left, true, upper);
                s.headings[2] = makeHeading(t.goodCondensedFamilies, 1.4,
                                            t.text, right, true, upper);
                s.headings[1].paddingTop = 0.1 * kRem;
                s.headings[3].marginTop = 0.25 * kRem; // ".item h3"
                for (int level = 3; level <= 6; ++level)
                {
                    s.headings[level] = makeHeading(t.goodFamilies, 1.0, t.text,
                                                    left, true, upper);
                    tight(level, 1.5);
                }
                break;
            case Role::Sticky:
                // ".sticky { font-size: 0.8rem }", in the page's font.
                s.families = t.sansFamilies;
                s.pointSize = 0.8 * kRem;
                s.color = t.stickyText;
                break;
            case Role::SidebarLeft:
            case Role::SidebarRight: {
                const bool isLeft = role == Role::SidebarLeft;
                const Qt::Alignment align = isLeft ? right : left;
                s.families = t.goodFamilies;
                s.pointSize = kRem;
                s.color = t.sidebarText;
                s.align = isLeft ? right : Qt::AlignJustify;
                s.lineHeight = 1.2;
                s.paraTop = 0;
                s.paraBottom = 0.15 * kRem;
                s.headings[1] =
                    makeHeading(t.ginFamilies, 1.25, t.sidebarText, align);
                s.headings[1].lineHeight = 1.2;
                s.headings[2] = makeHeading(t.goodFamilies, 1.0, t.sidebarText,
                                            align, true, upper);
                s.headings[3] = makeHeading(t.goodCondensedFamilies, 1.4,
                                            t.sidebarText, align, true, upper);
                // ".left h2, .left * + h2 { margin-top: 0; padding-top:
                // 0.2em }", ".left p + h1, .left p + h3 { margin-top: 1em }",
                // headings "margin-bottom: 0.1rem".
                for (int level = 1; level <= 3; ++level)
                {
                    HeadingStyle &h = s.headings[level];
                    h.marginTop = 0;
                    h.marginTopAfterText = level == 2 ? 0 : h.size;
                    h.paddingTop = level == 1 ? 0 : 0.2 * h.size;
                    h.marginBottom = 0.1 * kRem;
                }
                break;
            }
            }

            for (int level = 1; level <= 6; ++level)
            {
                if (s.headings[level].families == t.ginFamilies)
                {
                    s.headings[level].styleName = t.ginStyle;
                }
            }
            if (t.compactSpacing &&
                (role == Role::Body || role == Role::Item))
            {
                s.lineHeight = kBookLeading / s.pointSize;
            }
            return s;
        }

        // QFontDatabase::hasFamily, remembered: it goes through fontconfig
        // every time, and the fonts don't change once loaded.
        bool hasFamily(const QString &family)
        {
            static QMutex mutex;
            static QHash<QString, bool> known;
            const QMutexLocker lock(&mutex);
            const auto it = known.constFind(family);
            if (it != known.cend())
            {
                return *it;
            }
            return *known.insert(family, QFontDatabase::hasFamily(family));
        }

        // Layout units per point of the document's paint device.
        qreal unitsPerPoint(const QTextDocument &doc)
        {
            const QPaintDevice *device = doc.documentLayout()->paintDevice();
            return device ? device->logicalDpiY() / 72.0 : 1.0;
        }

        // A row of trait badges a little too long for a line of `width`
        // (layout units) has the space beside each badge's text narrowed,
        // by up to half, so it fits; a longer one wraps between badges.
        // Whether that changed the row's badge sizes.
        bool fitTraitRow(QTextBlock row, qreal width)
        {
            const QTextDocument *doc = row.document();
            qreal needed = 0;
            int badges = 0;
            qreal padX = 0;
            for (auto it = row.begin(); !it.atEnd(); ++it)
            {
                const QTextFragment fragment = it.fragment();
                const QTextCharFormat format = fragment.charFormat();
                if (format.objectType() != TraitBadge::kObjectType)
                {
                    continue;
                }
                needed += fragment.length() * TraitBadge::naturalWidth(doc, format);
                badges += fragment.length();
                padX = TraitBadge::padX(format);
            }
            qreal fitted = -1; // as made
            if (badges > 0 && needed > width)
            {
                // Each badge gives up this much on either side, in points,
                // with a little to spare against rounding.
                const qreal units = unitsPerPoint(*doc);
                const qreal cut = (needed - width) / units / (2.0 * badges) + 0.05;
                if (cut <= padX / 2)
                {
                    fitted = padX - cut;
                }
            }
            const QTextBlockUserData *before = row.userData();
            if (fitted < 0 && !before)
            {
                return false;
            }
            TraitBadge::setRowPadX(row, fitted);
            return true;
        }

        // Tables: small Good Pro text, a dark red header row, striped rows
        // and no grid. A row whose text starts with ". " is a footnote: it
        // spans the whole table and loses the marker.
        void styleTables(const Theme &t, QTextDocument &doc,
                         QTextCursor &cursor, qreal k)
        {
            constexpr qreal kSize = 0.9 * kRem;
            constexpr int kFixed =
                static_cast<int>(QTextBlockFormat::FixedHeight);

            std::vector<QTextTable *> tables;
            for (QTextFrame *frame : doc.rootFrame()->childFrames())
            {
                if (auto *table = qobject_cast<QTextTable *>(frame))
                {
                    tables.push_back(table);
                }
            }

            for (QTextTable *table : tables)
            {
                QTextTableFormat tableFormat = table->format();
                tableFormat.setBorder(0);
                tableFormat.setBorderStyle(QTextFrameFormat::BorderStyle_None);
                tableFormat.setCellSpacing(0);
                tableFormat.setCellPadding(0);
                tableFormat.setMargin(0);
                tableFormat.setWidth(
                    QTextLength(QTextLength::PercentageLength, 100));
                table->setFormat(tableFormat);

                for (int row = 1; row < table->rows(); ++row)
                {
                    QTextTableCell first = table->cellAt(row, 0);
                    QTextCursor start = first.firstCursorPosition();
                    start.movePosition(QTextCursor::NextCharacter,
                                       QTextCursor::KeepAnchor, 2);
                    if (start.selectedText() != QLatin1String(". "))
                    {
                        continue;
                    }
                    bool restEmpty = true;
                    for (int col = 1; col < table->columns(); ++col)
                    {
                        QTextTableCell cell = table->cellAt(row, col);
                        // Qt may already span the first cell across the
                        // row (it does with three columns or more).
                        if (cell.column() == 0)
                        {
                            continue;
                        }
                        if (cell.firstPosition() != cell.lastPosition())
                        {
                            restEmpty = false;
                        }
                    }
                    if (!restEmpty)
                    {
                        continue;
                    }
                    start.removeSelectedText();
                    table->mergeCells(row, 0, 1, table->columns());
                }

                for (int row = 0; row < table->rows(); ++row)
                {
                    const bool header = row == 0;
                    const bool foot = table->cellAt(row, 0).columnSpan() ==
                                          table->columns() &&
                                      table->columns() > 1;
                    QColor fill = header           ? t.tableHeader
                                  : foot           ? t.tableFoot
                                  : (row % 2 == 1) ? t.tableRowOdd
                                                   : t.tableRowEven;
                    for (int col = 0; col < table->columns(); ++col)
                    {
                        QTextTableCell cell = table->cellAt(row, col);
                        if (cell.row() != row || cell.column() != col)
                        {
                            continue; // covered by a merged cell
                        }

                        QTextTableCellFormat cellFormat;
                        cellFormat.setBackground(fill);
                        cellFormat.setPadding(0.25 * kRem * k);
                        cellFormat.setFontFamilies(t.goodFamilies);
                        cellFormat.setFontPointSize(kSize);
                        cellFormat.setFontWeight(QFont::Normal);
                        cellFormat.setForeground(header ? QColor(Qt::white)
                                                        : t.text);

                        QTextBlockFormat blockFormat;
                        // The markdown importer aligns body cells only; the
                        // header follows its column.
                        if (header && table->rows() > 1)
                        {
                            const QTextBlock below = doc.findBlock(
                                table->cellAt(1, col).firstPosition());
                            blockFormat.setAlignment(
                                below.blockFormat().alignment());
                        }
                        blockFormat.setTopMargin(0);
                        blockFormat.setBottomMargin(0);
                        blockFormat.setLineHeight(kSize * k, kFixed);

                        cursor.setPosition(cell.firstPosition());
                        cursor.setPosition(cell.lastPosition(),
                                           QTextCursor::KeepAnchor);
                        cursor.mergeBlockFormat(blockFormat);
                        // Cell-wide format first (padding, fill), then the
                        // character part over the text so bold/italic spans
                        // keep their slant.
                        cell.setFormat(cellFormat);
                        QTextCharFormat charFormat;
                        charFormat.setFontFamilies(t.goodFamilies);
                        charFormat.setFontPointSize(kSize);
                        charFormat.setFontWeight(QFont::Normal);
                        charFormat.setForeground(cellFormat.foreground());
                        cursor.mergeCharFormat(charFormat);
                    }
                }
            }
        }
    } // namespace

    // Measured rather than computed: Qt's default dpi depends on the
    // platform. It only depends on the paint device's dpi, so it's measured
    // once per dpi.
    qreal lengthScale(const QTextDocument &doc)
    {
        QPaintDevice *device = doc.documentLayout()->paintDevice();
        const int dpi = device ? device->logicalDpiY() : 0;
        static QMutex mutex;
        static QHash<int, qreal> measured;
        const QMutexLocker lock(&mutex);
        if (const auto it = measured.constFind(dpi); it != measured.cend())
        {
            return *it;
        }

        constexpr qreal kProbe = 100;
        QTextDocument probe;
        probe.documentLayout()->setPaintDevice(device);
        probe.setDocumentMargin(0);
        probe.setPlainText(QStringLiteral("x"));
        QTextBlockFormat format;
        format.setLineHeight(kProbe, QTextBlockFormat::FixedHeight);
        QTextCursor(&probe).mergeBlockFormat(format);
        const qreal height = probe.documentLayout()->documentSize().height();
        const qreal scale =
            height > 0 ? kProbe * unitsPerPoint(doc) / height : 1.0;
        measured.insert(dpi, scale);
        return scale;
    }

    QFont makeFont(const QStringList &families, qreal pointSize, bool bold,
                   bool italic)
    {
        QFont font;
        if (!families.isEmpty())
        {
            font.setFamily(families.first());
        }
        font.setFamilies(families);
        font.setPointSizeF(pointSize);
        font.setBold(bold);
        font.setItalic(italic);
        return font;
    }

    BoxStyle Theme::boxStyle(Role role) const
    {
        BoxStyle s;
        const qreal pad = 0.5 * kRem;
        s.padding = QMarginsF(pad, pad, pad, pad);

        switch (role)
        {
        case Role::Info:
            s.fill = infoFill;
            s.radius = infoRadius;
            if (infoBorder.isValid())
            {
                s.edges = Qt::TopEdge | Qt::BottomEdge | Qt::LeftEdge |
                          Qt::RightEdge;
                s.edgeColor = infoBorder;
                s.edgeWidth = infoBorderWidth;
            }
            break;
        case Role::Head:
            // A box only in the Remaster palette; its border has the fill's
            // colour, so it just pads the text.
            s.padding = QMarginsF();
            if (headFill.isValid())
            {
                s.fill = headFill;
                s.padding = QMarginsF(headBorder, headBorder, headBorder,
                                      headBorder);
            }
            break;
        case Role::Note:
            s.fill = noteBg;
            s.radius = 3;
            break;
        case Role::Rules:
            s.fill = rulesBg;
            s.radius = 3;
            break;
        case Role::Math:
            s.fill = mathBg;
            s.edges = Qt::TopEdge | Qt::BottomEdge;
            s.edgeColor = mathEdge;
            s.edgeWidth = 1.5;
            break;
        case Role::Item:
            s.padding = QMarginsF();
            break;
        case Role::SidebarLeft:
            s.edges = Qt::RightEdge;
            s.edgeColor = sidebarRule;
            s.edgeWidth = 0.75;
            s.padding = QMarginsF(0, 0, pad, 0);
            break;
        case Role::SidebarRight:
            s.edges = Qt::LeftEdge;
            s.edgeColor = sidebarRule;
            s.edgeWidth = 0.75;
            s.padding = QMarginsF(pad, 0, 0, 0);
            break;
        case Role::Sticky:
            // "padding: 0.25rem; border-radius: 0.25rem"
            s.fill = stickyFill;
            s.radius = 0.25 * kRem;
            s.padding = QMarginsF(1, 1, 1, 1) * (0.25 * kRem);
            break;
        default:
            s.padding = QMarginsF();
            break;
        }
        return s;
    }

    QColor Theme::textColor(Role role) const
    {
        return makeStyle(*this, role).color;
    }

    QColor Theme::ruleColor(Role role, bool lower) const
    {
        // "hr { border-top: 1px solid #000d; border-bottom: 1px solid #0002;
        // opacity: 0.75 }"; head and info boxes recolour both borders and
        // drop the opacity.
        switch (role)
        {
        case Role::Head:
            return headRule;
        case Role::Info:
            return infoRule;
        default:
            return lower ? QColor(0, 0, 0, 25) : QColor(0, 0, 0, 166);
        }
    }

    void Theme::apply(QTextDocument &doc, Role role) const
    {
        const RoleStyle style = makeStyle(*this, role);
        doc.setDefaultFont(makeFont(style.families, style.pointSize));
        // Vertical block lengths, tab stops and list indents below are
        // multiplied by k (see lengthScale()); left margins and text indents,
        // which Qt doesn't scale, by `units` (layout units per point).
        const qreal k = lengthScale(doc);
        doc.setIndentWidth(kListIndent * k);

        // Bullet style. Qt alternates disc/circle/square by depth; the site
        // uses discs, except in sidebars, which have no markers at all.
        const bool sidebar =
            role == Role::SidebarLeft || role == Role::SidebarRight;
        QSet<QTextList *> seen;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
        {
            QTextList *list = b.textList();
            if (!list)
            {
                continue;
            }
            if (sidebar)
            {
                // Qt positions list text itself and ignores the block's own
                // indents, which the hanging indent below needs.
                list->remove(b);
                QTextBlockFormat item;
                item.setIndent(0);
                item.setProperty(kSidebarItem, true);
                QTextCursor(b).mergeBlockFormat(item);
            }
            else if (!seen.contains(list))
            {
                seen.insert(list);
                QTextListFormat listFormat = list->format();
                listFormat.setStyle(QTextListFormat::ListDisc);
                list->setFormat(listFormat);
            }
        }

        constexpr int kFixed = static_cast<int>(QTextBlockFormat::FixedHeight);

        QTextCursor cursor(&doc);
        cursor.beginEditBlock();

        // setMarkdown() gives headings a relative size (+3 for h1, ...) that
        // takes precedence over any point size, even when set to 0, so it has
        // to be removed. Collect first; changing formats invalidates
        // fragment iterators.
        std::vector<std::tuple<int, int, QTextCharFormat>> sized;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
        {
            for (auto it = b.begin(); !it.atEnd(); ++it)
            {
                const QTextFragment fragment = it.fragment();
                QTextCharFormat format = fragment.charFormat();
                if (format.hasProperty(QTextFormat::FontSizeAdjustment))
                {
                    format.clearProperty(QTextFormat::FontSizeAdjustment);
                    sized.emplace_back(fragment.position(), fragment.length(),
                                       format);
                }
            }
        }
        for (const auto &[position, length, format] : sized)
        {
            cursor.setPosition(position);
            cursor.setPosition(position + length, QTextCursor::KeepAnchor);
            cursor.setCharFormat(format);
        }

        if (role == Role::Item)
        {
            // "# Name" + "## Feat 3": the site floats the level to the right
            // of the name, on the same line. Join them with a tab, which
            // finishLayout() turns into a right-aligned stop.
            for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
            {
                const QTextBlock next = b.next();
                if (b.blockFormat().headingLevel() != 1 || !next.isValid() ||
                    next.blockFormat().headingLevel() != 2)
                {
                    continue;
                }
                QTextCursor join(&doc);
                join.setPosition(b.position() + b.length() - 1);
                join.insertText(QStringLiteral("\t"));
                join.deleteChar(); // the block break: pulls the level in
                QTextBlockFormat tab;
                tab.setProperty(kRightTab, true);
                join.mergeBlockFormat(tab);
            }
        }

        // Trait lists from the parser become rows of badges.
        QTextCharFormat badgeFont;
        const auto [badgeSize, badgeWeight] =
            fitFont(goodCondensedFamilies, 0.8 * kRem, true);
        badgeFont.setFontFamilies(goodCondensedFamilies);
        badgeFont.setFontPointSize(badgeSize);
        badgeFont.setFontWeight(badgeWeight);
        TraitBadge::Metrics badge;
        badge.padTop = 0.25 * kRem;
        badge.padBottom = 0.1 * kRem;
        badge.padX = 0.5 * kRem;
        badge.borderY = 0.15 * kRem;
        badge.borderX = 0.1 * kRem;
        badge.fontSize = 0.8 * kRem;
        badge.capWidth = 2.25; // 3px
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
        {
            const QString text = b.text();
            if (!text.startsWith(QChar(kTraitsMarker)))
            {
                continue;
            }
            QTextCursor row(b);
            row.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            row.removeSelectedText();

            const QFont font = badgeFont.font();
            const QString object(QChar::ObjectReplacementCharacter);
            const QStringList names =
                text.mid(1).split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (int i = 0; i < names.size(); ++i)
            {
                // The row's end caps go on its first and last badges.
                row.insertText(object,
                               TraitBadge::format(names[i].toUpper(), font,
                                                  traitColor(names[i]),
                                                  traitBorder, Qt::white, badge,
                                                  i == 0, i == names.size() - 1));
            }

            QTextBlockFormat mark;
            mark.setProperty(kTraitsRow, true);
            row.mergeBlockFormat(mark);
        }

        // CSS centres a font's ascent + descent in the line box (half
        // leading); Qt's FixedHeight puts the baseline at 80% of the line.
        // The difference is stored on the block for shiftBaselines().
        // Both in layout units.
        QPaintDevice *device = doc.documentLayout()->paintDevice();
        const qreal units = unitsPerPoint(doc);
        auto baselineShift = [device, units](const QFont &font,
                                             qreal lineHeight) {
            const QFontMetricsF metrics(font, device);
            return (metrics.ascent() - metrics.descent()) / 2 -
                   0.3 * lineHeight * units;
        };

        bool first = true;
        bool prevTight = false;
        bool prevParagraph = false;
        bool prevHeading = false;
        bool prevText = false; // paragraph or list item
        bool prevHang = false; // previous paragraph was a "hang" one
        qreal prevBottom = 0;  // bottom margin of the previous block, in pt
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
        {
            const int level = b.blockFormat().headingLevel();
            QTextBlockFormat blockFormat;
            QTextCharFormat charFormat;
            QFont lineFont;    // main font of the block's lines
            qreal lineBox = 0; // their height; 0: no baseline shift

            // setMarkdown() puts an empty paragraph in front of a document
            // that starts with a rule; it has no counterpart in the HTML.
            if (first && b.length() == 1 && level == 0 && !b.textList() &&
                b.next().isValid())
            {
                blockFormat.setLineHeight(0, kFixed);
                blockFormat.setTopMargin(0);
                blockFormat.setBottomMargin(0);
                QTextCursor(b).mergeBlockFormat(blockFormat);
                continue; // the next block is still the first
            }

            if (level > 0)
            {
                const HeadingStyle &h = style.headings[std::min(level, 6)];
                const auto [fontSize, weight] =
                    fitFont(h.families, h.size, h.bold);
                charFormat.setFontFamilies(h.families);
                charFormat.setFontPointSize(fontSize);
                if (!h.styleName.isEmpty())
                {
                    charFormat.setFontStyleName(h.styleName);
                }
                charFormat.setFontWeight(weight);
                charFormat.setFontItalic(false);
                charFormat.setFontCapitalization(h.caps);
                if (h.stretch != 100)
                {
                    // Left unset otherwise: an explicit 100 makes Qt treat
                    // condensed families as stretched.
                    charFormat.setFontStretch(h.stretch);
                }
                if (h.letterSpacing > 0)
                {
                    charFormat.setFontLetterSpacingType(
                        QFont::AbsoluteSpacing);
                    charFormat.setFontLetterSpacing(h.letterSpacing); // pt
                }
                charFormat.setForeground(h.color);
                blockFormat.setAlignment(h.align);
                // Margins collapse (Qt does the same), padding adds on.
                const qreal marginTop = prevText && h.marginTopAfterText >= 0
                                            ? h.marginTopAfterText
                                            : h.marginTop;
                blockFormat.setTopMargin(
                    first
                        ? 0
                        : (std::max(prevBottom, marginTop) + h.paddingTop) * k);
                blockFormat.setBottomMargin(h.marginBottom * k);
                blockFormat.setLineHeight(h.size * h.lineHeight * k, kFixed);
                lineFont = makeFont(h.families, fontSize);
                lineFont.setWeight(weight);
                if (!h.styleName.isEmpty())
                {
                    lineFont.setStyleName(h.styleName);
                }
                lineBox = h.size * h.lineHeight;

                if (role == Role::Body && level == 4)
                {
                    // "#### 1st Level": a bar heading a feat section, drawn by
                    // PageLayout. "padding: 0.3rem 0 0.25rem 0.5rem",
                    // "letter-spacing: 0.75px", line-height 1.
                    const qreal padTop = 0.3 * kRem;
                    const qreal bar = padTop + h.size + 0.25 * kRem;
                    blockFormat.setProperty(kFeatBar, bar);
                    blockFormat.setTextIndent(0.5 * kRem * units);
                    blockFormat.setLineHeight(bar * k, kFixed);
                    charFormat.setFontLetterSpacingType(QFont::AbsoluteSpacing);
                    charFormat.setFontLetterSpacing(0.5625); // in points
                    // Like baselineShift(), for a line box inside padding.
                    const QFontMetricsF metrics(lineFont, device);
                    blockFormat.setProperty(
                        kBaselineShift,
                        (padTop + h.size / 2) * units +
                            (metrics.ascent() - metrics.descent()) / 2 -
                            0.8 * bar * units);
                    lineBox = 0;
                }
                prevTight = h.tightAfter;
                if (h.tightAfter)
                {
                    blockFormat.setProperty(kNegativeBottom, -0.5 * kRem);
                }
                prevParagraph = false;
                prevHeading = true;
            }
            else if (b.blockFormat().boolProperty(kTraitsRow))
            {
                // ".traits { margin: 0.25rem 0 }"; the badges fill the line.
                // Objects sit on the baseline, which Qt puts at 80% of a
                // fixed line: move them up to the top.
                const qreal height = badge.height();
                blockFormat.setAlignment(Qt::AlignLeft);
                blockFormat.setTopMargin(0.25 * kRem * k);
                blockFormat.setBottomMargin(0.25 * kRem * k);
                blockFormat.setLineHeight(height * k, kFixed);
                blockFormat.setProperty(kBaselineShift, 0.2 * height * units);
                prevTight = false;
                prevParagraph = false;
                prevHeading = false;
            }
            else if (b.blockFormat().hasProperty(
                         QTextFormat::BlockTrailingHorizontalRulerWidth))
            {
                // "* + hr { margin-top: 0.5rem }" (a rule on its own measures
                // the same on the site), "hr { margin-bottom: 0.15rem }" and
                // 1px borders top and bottom. Qt draws the
                // rule across the middle of the line's natural height, so
                // the (empty) line gets a font that is as tall as the rule.
                // ".item hr { margin: 0 }".
                const bool item = role == Role::Item;
                blockFormat.setTopMargin(item ? 0 : 0.5 * kRem * k);
                blockFormat.setBottomMargin(item ? 0 : 0.15 * kRem * k);
                QTextCharFormat ruleFont;
                ruleFont.setFontPointSize(1.25);
                QTextCursor(b).mergeBlockCharFormat(ruleFont);
                prevTight = false;
                prevParagraph = false;
                prevHeading = false;
            }
            else if (QTextCursor(b).currentTable())
            {
                // Styled by styleTables(), which keeps column alignment.
                prevTight = false;
                prevParagraph = false;
                prevHeading = false;
                prevText = false;
                prevBottom = 0;
                continue;
            }
            else
            {
                blockFormat.setAlignment(style.align);
                blockFormat.setLineHeight(
                    style.pointSize * style.lineHeight * k, kFixed);
                lineFont = makeFont(style.families, style.pointSize, style.bold,
                                    style.italic);
                lineBox = style.pointSize * style.lineHeight;
                for (auto it = b.begin(); !it.atEnd(); ++it)
                {
                    if (it.fragment().charFormat().isImageFormat())
                    {
                        // A line holding an image grows to fit it.
                        blockFormat.setLineHeight(
                            style.pointSize * style.lineHeight * k,
                            QTextBlockFormat::MinimumHeight);
                        lineBox = 0;
                        break;
                    }
                }
                const qreal em = style.pointSize;
                const bool sidebarItem =
                    b.blockFormat().boolProperty(kSidebarItem);
                if (b.textList() || sidebarItem)
                {
                    // "h2 + ul" gets the usual gap; a list right after a
                    // paragraph or a tight heading doesn't.
                    const bool firstItem =
                        sidebarItem ? !b.previous().blockFormat().boolProperty(
                                          kSidebarItem)
                                    : b.previous().textList() != b.textList();
                    const bool gap = firstItem && prevHeading && !prevTight;
                    blockFormat.setTopMargin(gap ? style.paraTop * k : 0);
                    blockFormat.setBottomMargin(0);
                    if (sidebarItem)
                    {
                        // "padding-left: 0.5em; text-indent: -0.5em", no
                        // marker.
                        blockFormat.setLeftMargin(0.5 * em * units);
                        blockFormat.setTextIndent(-0.5 * em * units);
                    }
                }
                else
                {
                    const auto start = b.begin();
                    const bool hang =
                        role != Role::Info && !start.atEnd() &&
                        start.fragment().charFormat().fontWeight() >=
                            QFont::Bold;
                    qreal top = style.paraTop;
                    if (prevTight)
                    {
                        top = 0;
                    }
                    else if (prevParagraph && style.paraAfterPara >= 0)
                    {
                        // ".item p.hang + p.hang { margin: 0 }"
                        top = hang && prevHang ? 0 : style.paraAfterPara;
                    }
                    blockFormat.setTopMargin(first ? 0 : top * k);
                    // A list hangs right under the paragraph introducing it.
                    const bool listNext =
                        b.next().isValid() && b.next().textList();
                    blockFormat.setBottomMargin(
                        listNext ? 0 : style.paraBottom * k);

                    // Scribe marks paragraphs that open with bold text
                    // ("**Version 2.4** ...") as "hang": wrapped lines are
                    // indented by 1em. Info boxes turn it off.
                    prevHang = hang;
                    if (hang)
                    {
                        blockFormat.setLeftMargin(em * units);
                        blockFormat.setTextIndent(-em * units);
                    }
                }
                if (style.italic)
                {
                    charFormat.setFontItalic(true);
                }
                if (style.bold)
                {
                    charFormat.setFontWeight(QFont::Bold);
                }
                prevTight = false;
                prevParagraph = !b.textList() && !sidebarItem;
                prevHeading = false;
            }
            prevText = level == 0 &&
                       !blockFormat.hasProperty(
                           QTextFormat::BlockTrailingHorizontalRulerWidth) &&
                       !b.blockFormat().hasProperty(
                           QTextFormat::BlockTrailingHorizontalRulerWidth);
            prevBottom = blockFormat.bottomMargin() / k;
            if (lineBox > 0)
            {
                blockFormat.setProperty(kBaselineShift,
                                        baselineShift(lineFont, lineBox));
            }
            first = false;

            cursor.setPosition(b.position());
            cursor.movePosition(QTextCursor::EndOfBlock,
                                QTextCursor::KeepAnchor);
            cursor.mergeBlockFormat(blockFormat);
            if (b.length() > 1)
            {
                cursor.mergeCharFormat(charFormat);
            }
        }
        styleTables(*this, doc, cursor, k);
        cursor.endEditBlock();

        // Links get the site's link colour. Collect the ranges first, because
        // changing formats invalidates fragment iterators.
        std::vector<std::pair<int, int>> anchors;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
        {
            for (auto it = b.begin(); !it.atEnd(); ++it)
            {
                const QTextFragment fragment = it.fragment();
                if (fragment.charFormat().isAnchor())
                {
                    anchors.emplace_back(fragment.position(),
                                         fragment.length());
                }
            }
        }
        for (const auto &[position, length] : anchors)
        {
            QTextCharFormat linkFormat;
            linkFormat.setForeground(link);
            cursor.setPosition(position);
            cursor.setPosition(position + length, QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(linkFormat);
        }

        // Last, so the icon font isn't overridden by the styling above.
        applyIcons(doc);
    }

    Theme Theme::pathfinder()
    {
        return Theme{};
    }

    Theme Theme::remaster()
    {
        Theme t;
        const QColor green{0x00, 0x2a, 0x16};
        const QColor tan{0xe7, 0xd9, 0xb4};
        const QColor brown{0x4e, 0x1b, 0x0e};
        t.titleFill = green;
        t.headText = green;
        t.headH3 = green;
        t.headRule = QColor(0x17, 0x0f, 0x03);
        t.headFill = tan;
        t.headBorder = 2.25; // "border: solid", medium: 3px
        t.contentH1 = green;
        t.contentH2 = brown;
        t.contentH3 = QColor(0x01, 0x5c, 0x4d);
        t.goodProHeadings = true;
        t.sidebarText = brown;
        t.infoFill = green;
        t.infoBorder = QColor(0xb1, 0x9d, 0x74);
        t.infoBorderWidth = 0.165 * kRem;
        t.infoRadius = 0;
        t.infoDivider = t.infoBorder; // 2px
        t.noteBg = tan;
        t.tableHeader = green;
        return t;
    }

    Theme Theme::starfinder()
    {
        Theme t;
        // Measured in Starfinder 2e Alien Core and Player Core, as a PDF
        // reader shows their CMYK colours.
        const QColor blue{0x2e, 0x49, 0x69};   // CMYK 88/66/39/22
        const QColor cyan{0xa3, 0xe1, 0xe5};   // the rule beside sidebars
        const QColor white{0xff, 0xff, 0xff};

        // Good for everything but headings; Audiowide where Pathfinder uses
        // Gin, Sofachrome where it uses Taroca.
        t.bodyFamilies = t.goodFamilies;
        t.ginFamilies.prepend(QStringLiteral("Audiowide"));
        t.tarocaFamilies.prepend(QStringLiteral("Sofachrome"));
        // Sofachrome is over half as wide again as Taroca: scaled to about
        // the same width. It has no bold.
        t.substitutes.push_back({QStringLiteral("Sofachrome"), 0.65, QFont::Normal});

        t.pageBg = white;
        t.text = QColor(0x23, 0x1f, 0x20);
        t.navy = blue;
        t.tableRowOdd = QColor(0xdd, 0xe3, 0xeb);
        t.tableRowEven = QColor(0xee, 0xf1, 0xf5);
        t.tableFoot = QColor(0xc8, 0xd2, 0xdf);
        // Plain badges: a white border on white pages, a gap between them.
        t.traitFill = blue;
        t.traitUncommon = QColor(0x95, 0x41, 0x32);
        t.traitRare = QColor(0x70, 0x7a, 0x94);
        t.traitUnique = QColor(0x54, 0x31, 0x98);
        t.traitSize = QColor(0x3e, 0x78, 0x4c);
        t.traitAlignment = blue;
        t.traitBorder = white;
        t.noteBg = blue; // like the books' Key Terms box
        t.noteText = white;
        t.rulesBg = QColor(0xe6, 0xeb, 0xf1);
        t.mathBg = QColor(0xeb, 0xee, 0xf2);
        t.mathEdge = QColor(0xc5, 0xd0, 0xdd);

        t.titleFill = blue;
        t.titleEdge = cyan;
        t.titleText = white;
        t.headText = blue;
        t.headH3 = blue;
        t.headRule = blue;
        t.contentH1 = blue;
        t.contentH2 = blue;
        t.contentH3 = blue;
        t.sidebarText = blue;
        t.sidebarRule = cyan;
        t.infoFill = blue;
        t.infoRule = cyan;
        t.infoAccent = cyan;
        t.featText = white;
        t.tableHeader = blue;
        return t;
    }

    const std::vector<Theme::Preset> &Theme::presets()
    {
        static const std::vector<Preset> list{
            {"pathfinder", QT_TRANSLATE_NOOP("Theme", "Pathfinder 2e theme"),
             &Theme::pathfinder},
            {"remaster",
             QT_TRANSLATE_NOOP("Theme", "Pathfinder 2e Remaster theme"),
             &Theme::remaster},
            {"starfinder", QT_TRANSLATE_NOOP("Theme", "Starfinder 2e theme"),
             &Theme::starfinder}};
        return list;
    }

    Theme Theme::preset(const QString &key)
    {
        for (const Preset &p : presets())
        {
            if (key == QLatin1String(p.key))
            {
                return p.make();
            }
        }
        return pathfinder();
    }

    qreal trailingMargin(const QTextDocument &doc)
    {
        return doc.lastBlock().blockFormat().doubleProperty(kNegativeBottom);
    }

    qreal featBarHeight(const QTextBlock &block)
    {
        return block.blockFormat().doubleProperty(kFeatBar);
    }

    std::pair<qreal, QFont::Weight> Theme::fitFont(const QStringList &families,
                                                   qreal pointSize,
                                                   bool bold) const
    {
        const QFont::Weight normal = bold ? QFont::Bold : QFont::Normal;
        for (const QString &family : families)
        {
            if (!hasFamily(family))
            {
                continue;
            }
            // The first installed family is the one Qt will use.
            for (const Substitute &sub : substitutes)
            {
                if (family.compare(sub.family, Qt::CaseInsensitive) == 0)
                {
                    return {pointSize * sub.scale,
                            bold ? sub.boldWeight : QFont::Normal};
                }
            }
            break;
        }
        return {pointSize, normal};
    }

    QColor Theme::traitColor(const QString &name) const
    {
        static const QSet<QString> sizes{"tiny",  "small", "medium",
                                         "large", "huge",  "gargantuan"};
        static const QSet<QString> alignments{"lg", "ng", "cg", "ln", "n",
                                              "cn", "le", "ne", "ce"};
        const QString key = name.trimmed().toLower();
        if (key == QLatin1String("uncommon"))
        {
            return traitUncommon;
        }
        if (key == QLatin1String("rare"))
        {
            return traitRare;
        }
        if (key == QLatin1String("unique"))
        {
            return traitUnique;
        }
        if (sizes.contains(key))
        {
            return traitSize;
        }
        if (alignments.contains(key))
        {
            return traitAlignment;
        }
        return traitFill;
    }

    void Theme::finishLayout(QTextDocument &doc)
    {
        // Qt lays a document out lazily: the first 1000 or so characters
        // at once, the rest on a timer, which never gets to run here. An
        // edit made before then, such as the tab stops below, leaves the rest
        // of the document never laid out (and so never drawn: long stat
        // blocks lost their end). The root frame's bounding rectangle is one
        // of the few calls that make Qt finish.
        doc.documentLayout()->frameBoundingRect(doc.rootFrame());

        // Trait rows too long for the full width (see fitTraitRow()).
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
        {
            QTextLayout *layout = b.layout();
            if (b.blockFormat().boolProperty(kTraitsRow) && layout &&
                layout->lineCount() > 0 &&
                fitTraitRow(b, layout->lineAt(0).width()))
            {
                doc.markContentsDirty(b.position(), b.length());
            }
        }
        doc.documentLayout()->frameBoundingRect(doc.rootFrame()); // see above

        // Right-aligned stops need the final width. Like other block
        // lengths, Qt scales tab positions (see lengthScale()), so the width
        // in layout units is converted back. (Setting them before the
        // layout above would save Qt a second pass over stat blocks, but
        // changes the output: tried 2026-10-04, 213 pages became 209.)
        const qreal toQt = lengthScale(doc) / unitsPerPoint(doc);
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
        {
            if (!b.blockFormat().boolProperty(kRightTab))
            {
                continue;
            }
            QTextBlockFormat tabs;
            tabs.setTabPositions({QTextOption::Tab((doc.textWidth() - 1) * toQt,
                                                   QTextOption::RightTab)});
            QTextCursor(b).mergeBlockFormat(tabs);
        }

        // Qt has no half-leading mode and ignores negative margins, so the
        // lines themselves are moved. Block rectangles stay where they are,
        // and nothing re-lays the document out afterwards.
        doc.documentLayout()->frameBoundingRect(doc.rootFrame()); // see above
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
        {
            const qreal shift = b.blockFormat().doubleProperty(kBaselineShift);
            QTextLayout *layout = b.layout();
            if (shift == 0 || !layout)
            {
                continue;
            }
            for (int i = 0; i < layout->lineCount(); ++i)
            {
                QTextLine line = layout->lineAt(i);
                line.setPosition(line.position() + QPointF(0, shift));
            }
        }
    }

    qreal Theme::reflow(QTextDocument &doc, const LineRoom &room)
    {
        if (!doc.rootFrame()->childFrames().isEmpty())
        {
            return -1;
        }
        const qreal units = unitsPerPoint(doc);
        // Qt's factor for block lengths: line heights in the block format
        // are multiplied by it (see lengthScale()).
        const qreal scale = units / lengthScale(doc);
        // Narrower than this, a line waits until the sidebar ends.
        const qreal minWidth = 4 * kRem * units;
        const qreal step = units; // a point at a time

        qreal moved = 0; // how far the blocks below have moved down
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
        {
            QTextLayout *layout = b.layout();
            if (!layout || layout->lineCount() == 0)
            {
                continue;
            }
            const QTextBlockFormat format = b.blockFormat();
            const qreal shift = format.doubleProperty(kBaselineShift);

            // How far a line moves the layout down, and how far above that
            // point its top goes; as Qt's own layout works them out.
            auto advance = [&](const QTextLine &line) -> std::pair<qreal, qreal> {
                const qreal raw =
                    std::ceil(line.ascent() + line.descent() + line.leading());
                const qreal lh = format.lineHeight(raw, scale);
                switch (format.lineHeightType())
                {
                case QTextBlockFormat::FixedHeight:
                    return {lh, line.ascent() + std::max(line.leading(), 0.0) -
                                    lh * 4 / 5};
                case QTextBlockFormat::MinimumHeight:
                    return {lh, line.height() - lh};
                default:
                    return {lh, 0};
                }
            };

            // Where the lines start and end now, and their indents: the
            // first line's (text-indent) and the others'.
            const QTextLine first = layout->lineAt(0);
            const qreal start = first.y() - shift + advance(first).second;
            qreal oldEnd = start;
            for (int i = 0; i < layout->lineCount(); ++i)
            {
                oldEnd += advance(layout->lineAt(i)).first;
            }
            const qreal firstX = first.x();
            const qreal firstRight = first.x() + first.width();
            qreal restX = firstX - format.textIndent();
            if (layout->lineCount() > 1)
            {
                restX = layout->lineAt(1).x();
            }
            const qreal nominal = format.lineHeightType() ==
                                          QTextBlockFormat::FixedHeight
                                      ? format.lineHeight() * scale
                                      : first.height();

            const QPointF position = layout->position() + QPointF(0, moved);
            layout->setPosition(position);
            // The x span a line at `y` may use: the block's own, cut by the
            // free span (whose left edge also carries the block's indents).
            auto span = [&](qreal y, qreal height, qreal x) {
                const auto [left, right] =
                    room(position.y() + y, position.y() + y + height);
                const qreal lineX = x + left;
                return std::make_pair(lineX, std::min(firstRight, right) - lineX);
            };

            // An item title's level sits at the right edge: its tab stop
            // follows the edge in. Tab positions count from the start of
            // the line (which a sidebar on the left moves right), and Qt
            // multiplies them by `scale`.
            if (format.boolProperty(kRightTab))
            {
                const qreal width = span(start, nominal, firstX).second;
                QTextOption option = layout->textOption();
                option.setTabs({QTextOption::Tab((width - 1) / scale,
                                                 QTextOption::RightTab)});
                layout->setTextOption(option);
            }

            // A trait row fitted to the room beside a sidebar.
            if (format.boolProperty(kTraitsRow))
            {
                fitTraitRow(b, span(start, nominal, firstX).second);
            }

            qreal y = start;
            layout->beginLayout();
            for (int n = 0;; ++n)
            {
                QTextLine line = layout->createLine();
                if (!line.isValid())
                {
                    break;
                }
                line.setLeadingIncluded(true); // as Qt's own layout does
                const qreal x = n == 0 ? firstX : restX;
                const qreal fullWidth = firstRight - x;
                qreal height = nominal;
                std::pair<qreal, qreal> geometry;
                for (int tries = 0; tries < 3; ++tries)
                {
                    geometry = span(y, height, x);
                    for (int skipped = 0;
                         geometry.second < std::min(minWidth, fullWidth) &&
                         skipped < 2000;
                         ++skipped)
                    {
                        y += step;
                        geometry = span(y, height, x);
                    }
                    line.setLineWidth(std::max(geometry.second, 1.0));
                    // A taller line (an inline image) may meet more of the
                    // sidebar: check again with its real height.
                    const qreal real = std::max(advance(line).first, line.height());
                    if (real <= height + 0.01)
                    {
                        break;
                    }
                    height = real;
                }
                const auto [lineAdvance, above] = advance(line);
                line.setPosition(QPointF(geometry.first, y - above + shift));
                y += lineAdvance;
            }
            layout->endLayout();
            moved += y - oldEnd;
        }
        return doc.documentLayout()->documentSize().height() + moved;
    }

    void Theme::applyIcons(QTextDocument &doc) const
    {
        // Without an icon font the glyphs would show as empty boxes; the
        // tokens themselves (":a:") say more.
        if (std::none_of(iconFamilies.begin(), iconFamilies.end(),
                         [](const QString &family) { return hasFamily(family); }))
        {
            return;
        }
        // One search for all tokens. Alternatives are tried in order, and
        // actionGlyphs lists the longest tokens first. Compiled once: one
        // made per document cost a tenth of a build.
        QHash<QString, QString> glyphs;
        QStringList alternatives;
        for (const auto &[token, glyph] : actionGlyphs)
        {
            glyphs.insert(token.toLower(), glyph);
            alternatives << QRegularExpression::escape(token);
        }
        const QString pattern = alternatives.join(QLatin1Char('|'));
        static QMutex mutex;
        static QHash<QString, QRegularExpression> compiled;
        QRegularExpression tokens;
        {
            const QMutexLocker lock(&mutex);
            auto it = compiled.find(pattern);
            if (it == compiled.end())
            {
                it = compiled.insert(
                    pattern, QRegularExpression(
                                 pattern, QRegularExpression::CaseInsensitiveOption));
                it->optimize();
            }
            tokens = *it;
        }
        // Most documents have no icons at all.
        if (!tokens.match(doc.toRawText()).hasMatch())
        {
            return;
        }

        // Block by block, last match first, so the positions still to come
        // don't move. (QTextDocument::find would compile its own copy of the
        // expression on every call.)
        for (QTextBlock b = doc.lastBlock(); b.isValid(); b = b.previous())
        {
            std::vector<std::pair<int, int>> found; // start, length
            auto it = tokens.globalMatch(b.text());
            while (it.hasNext())
            {
                const QRegularExpressionMatch m = it.next();
                found.emplace_back(b.position() + m.capturedStart(), m.capturedLength());
            }
            for (auto match = found.rbegin(); match != found.rend(); ++match)
            {
                QTextCursor cursor(&doc);
                cursor.setPosition(match->first);
                cursor.setPosition(match->first + match->second, QTextCursor::KeepAnchor);
                QTextCharFormat format = cursor.charFormat();
                format.setFontFamilies(iconFamilies);
                format.setFontCapitalization(QFont::MixedCase);
                format.setFontWeight(QFont::Normal);
                format.setFontItalic(false);
                // Matched regardless of case: ":A:" too.
                cursor.insertText(glyphs.value(cursor.selectedText().toLower()), format);
            }
        }
    }
} // namespace scribe
