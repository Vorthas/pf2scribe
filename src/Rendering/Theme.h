// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include <QColor>
#include <QFont>
#include <QMarginsF>
#include <QString>
#include <QStringList>

#include <functional>
#include <utility>
#include <vector>

class QTextBlock;
class QTextDocument;

namespace scribe
{
    // What a chunk of text is inside. Decides fonts, colours and alignment.
    enum class Role
    {
        Body,
        Head,
        Info,
        Note,
        Rules,
        Math,
        Item,
        SidebarLeft,
        SidebarRight,
        Sticky
    };

    // Negative bottom margin, in points, that `doc` ends with: headings the
    // site gives "margin-bottom: -0.5rem". Like CSS, PageLayout lets it eat
    // into the gap before the next block when nothing (padding, border)
    // separates them. 0 when there is none.
    qreal trailingMargin(const QTextDocument &doc);

    // Height in points of the "#### 1st Level" feat bar that `block` is, or
    // 0 if it isn't one. PageLayout paints the bar behind the text.
    qreal featBarHeight(const QTextBlock &block);

    // Qt scales block lengths (margins, line heights, tab stops, list
    // indents, cell padding, image sizes) by the paint device's dpi over its
    // own default, while font sizes follow the dpi exactly. A length in
    // points times this factor comes out as that many points in `doc`.
    qreal lengthScale(const QTextDocument &doc);

    // Builds a QFont with a family fallback list.
    QFont makeFont(const QStringList &families, qreal pointSize,
                   bool bold = false, bool italic = false);

    // Background and borders of a boxed block (info, note, sidebar, ...).
    struct BoxStyle
    {
        QColor fill; // invalid colour: no fill
        qreal radius = 0;
        Qt::Edges edges;
        QColor edgeColor;
        qreal edgeWidth = 0;
        QMarginsF padding;
    };

    // 1rem of the site's stylesheet, in points (":root { font-size: 10.4pt }"
    // in Scribe's HTML export, for screen and print alike).
    constexpr qreal kRem = 10.4;

    // Numbers below come from the stylesheet of Scribe's HTML export, in
    // rem. Font families are tried in order: the first names are what
    // the site loads from Adobe Fonts (not redistributable), the rest are open
    // fallbacks that may be installed. Drop the real files in fonts/ to use
    // them.
    struct Theme
    {
        // Linotype Sabon: body text
        QStringList bodyFamilies{"Sabon LT Pro", "Linotype Sabon", "Sabon",
                                 "EB Garamond",  "Crimson Pro",    "Garamond",
                                 "Noto Serif",   "serif"};
        // FF Good Pro: boxes, items, sidebars, tables
        QStringList goodFamilies{"Good Pro",  "FF Good Pro", "Good OT",
                                 "Fira Sans", "Noto Sans",   "sans-serif"};
        QStringList goodCondensedFamilies{"Good Pro Condensed", "FF Good Pro Condensed",
                                          "FF Good Pro Cond",   "Good Pro Cond",
                                          "Fira Sans Extra Condensed",
                                          "Roboto Condensed",   "Oswald",
                                          "sans-serif"};
        // Taroca: page title, h1 and head text
        QStringList tarocaFamilies{"Taroca", "Nodesto Caps Condensed",
                                   "EB Garamond", "Noto Serif", "serif"};
        // Stand-ins for fonts that aren't installed. When a family list
        // resolves to `family`, font sizes are scaled by `scale` and bold
        // text uses `boldWeight`, so the stand-in keeps the proportions of
        // the font it replaces. Oswald, for Good Pro Condensed Bold: its
        // capitals are 12% taller and its Bold much heavier.
        struct Substitute
        {
            QString family;
            qreal scale = 1;
            QFont::Weight boldWeight = QFont::Bold;
        };
        std::vector<Substitute> substitutes{{"Oswald", 0.88, QFont::Medium}};

        // Gin: h2, h3 and h4 in the body
        QStringList ginFamilies{"Gin", "Gin Test", "EB Garamond", "Noto Serif",
                                "serif"};
        // "Gin Test" ships its Lines, Rough and Round cuts as styles of one
        // family, all at the same weight, so the style is named explicitly.
        QString ginStyle{"Regular"};
        // Open Sans: the page's own font, used by sticky notes
        QStringList sansFamilies{"Open Sans", "Noto Sans", "DejaVu Sans",
                                 "sans-serif"};
        // Watermark and page numbers
        QStringList timesFamilies{"Times New Roman", "Liberation Serif",
                                  "Tinos",           "Nimbus Roman",
                                  "Times",           "serif"};

        // Icon font for action symbols, and what each token in the source
        // turns into. Longest tokens first.
        QStringList iconFamilies{"Pathfinder-Icons", "Pathfinder Icons"};
        std::vector<std::pair<QString, QString>> actionGlyphs{
            {":aaa:", QStringLiteral("\uE900")},
            {":aa:", QStringLiteral("\uE901")},
            {":a:", QStringLiteral("\uE902")},
            {":f:", QStringLiteral("\uE903")},
            {":r:", QStringLiteral("\uE904")}};

        QColor pageBg{0xef, 0xec, 0xe6};
        QColor text{0x22, 0x22, 0x22};
        QColor navy{0x00, 0x25, 0x64};
        QColor tableRowOdd{0xed, 0xe3, 0xc7};
        QColor tableRowEven{0xf4, 0xee, 0xe0};
        QColor tableFoot{0xe6, 0xd8, 0xb0};
        // ".sticky { background: #fffa; color: #444;
        // box-shadow: 1px 2px 2px #0004 }"
        QColor stickyFill{255, 255, 255, 0xaa};
        QColor stickyText{0x44, 0x44, 0x44};
        QColor stickyShadow{0, 0, 0, 0x44};
        // Item trait badges: fill by kind of trait, gold borders and caps
        // (Pathfinder).
        QColor traitFill{0x5d, 0x00, 0x00};
        QColor traitUncommon{0x98, 0x50, 0x3c};
        QColor traitRare{0x00, 0x25, 0x64};
        QColor traitUnique{0x54, 0x16, 0x6d};
        QColor traitSize{0x3a, 0x7a, 0x58};
        QColor traitAlignment{0x56, 0x61, 0x93};
        QColor traitBorder{0xd8, 0xc3, 0x84}; // and the row's end caps
        QColor link{0xac, 0x0d, 0x4a};
        QColor noteBg{0xd1, 0xc7, 0xb1};
        QColor noteText{0x22, 0x22, 0x22}; // and its headings
        QColor rulesBg{0xf0, 0xe8, 0xd3};
        QColor mathBg{0xea, 0xe3, 0xd8};
        QColor mathEdge{0xe0, 0xc9, 0xc0};
        QColor divider{0xf1, 0xf0, 0xeb};

        // Colours the other palettes change; Pathfinder's values here. See
        // remaster() and starfinder().
        QColor titleFill{0x5d, 0x00, 0x00};       // ".title h1"
        QColor titleEdge{0xd8, 0xc3, 0x84};       // its border
        QColor titleText{0xd8, 0xc3, 0x84};
        QColor headText{0x5d, 0x00, 0x00};        // ".head", head h1/h2
        QColor headH3{0x00, 0x25, 0x64};          // ".head h3"
        QColor headRule{0x5d, 0x00, 0x00};        // ".head hr"
        QColor headFill;                          // invalid: no box
        qreal headBorder = 0;                     // around the head box, pt
        QColor contentH1{0x00, 0x25, 0x64};       // "#" in body text
        QColor contentH2{0x5d, 0x00, 0x00};       // "##"
        QColor contentH3{0xa7, 0x66, 0x52};       // "###"
        // "##" and "###" in Good Pro Bold instead of Gin; "###" without
        // small caps, squeezed to 80% width and letter-spaced.
        bool goodProHeadings = false;
        QColor sidebarText{0x5d, 0x00, 0x00};     // ".left", ".right"
        QColor sidebarRule{0x5d, 0x00, 0x00};     // the line beside them
        QColor infoFill{0x5d, 0x00, 0x00};        // ".info"
        QColor infoBorder;                        // invalid: none
        qreal infoBorderWidth = 0;
        qreal infoRadius = 3;
        QColor infoDivider;                       // between info columns
        QColor infoRule{0x98, 0x00, 0x00};        // "---" in an info box
        QColor infoAccent{0xd8, 0xc3, 0x84};      // its "###"
        QColor featText{0xed, 0xe3, 0xc7};        // "####" on its navy bar
        QColor tableHeader{0x5d, 0x00, 0x00};     // "thead th"

        // Native-only option: body text and stat blocks set with the line
        // spacing of Paizo's rulebooks (12pt) instead of the site's, which
        // is about 1.4 lines (13.5pt and 14.6pt).
        bool compactSpacing = false;

        // The palettes the Themes menu offers. Pathfinder 2e: the Scribe
        // website's own colours, as above.
        static Theme pathfinder();
        // Pathfinder 2e Remaster colours: the user stylesheet in
        // RemasterCSS (greens and browns instead of red and navy).
        static Theme remaster();
        // Starfinder 2e: the blues of the Starfinder 2e rulebooks on white
        // pages, Good for body text, Audiowide headings and a Sofachrome
        // title when they are installed.
        static Theme starfinder();

        struct Preset
        {
            const char *key;  // in the settings
            const char *name; // in the Themes menu, untranslated
            Theme (*make)();
        };
        static const std::vector<Preset> &presets();
        // The preset saved as `key`; Pathfinder for an unknown key.
        static Theme preset(const QString &key);
        QColor watermark{0, 0, 0, 85};
        QColor pageNumber{0, 0, 0, 128};

        BoxStyle boxStyle(Role role) const;
        QColor textColor(Role role) const;
        // Colour of a horizontal rule's upper or lower 1px border.
        QColor ruleColor(Role role, bool lower = false) const;

        // Applies fonts, colours and paragraph styling to a document made by
        // setMarkdown(), then swaps action tokens for icon glyphs.
        void apply(QTextDocument &doc, Role role) const;

        // Point size and weight for text meant to be set in `families` at
        // `pointSize`, after any stand-in adjustment (see substitutes).
        std::pair<qreal, QFont::Weight>
        fitFont(const QStringList &families, qreal pointSize, bool bold) const;

        // Background colour of the badge for trait `name`.
        QColor traitColor(const QString &name) const;

        // Last touches once the document has its final width: right-aligned
        // tab stops, then moves laid-out lines so their baselines sit where a
        // browser puts them. Any later relayout undoes the latter.
        static void finishLayout(QTextDocument &doc);

        // Free span [left, right] for a line between `top` and `bottom`, all
        // in layout units of the document.
        using LineRoom =
            std::function<std::pair<qreal, qreal>(qreal top, qreal bottom)>;

        // Breaks the lines of a finished document (see finishLayout()) again
        // so each fits the span `room` gives at its height, moving the blocks
        // below up or down to suit. Text beside a sidebar flows around it
        // this way. Lines too narrowed for some words move down. Returns the
        // new height in layout units, or -1 if the document holds tables,
        // which can't be re-broken and are left alone.
        static qreal reflow(QTextDocument &doc, const LineRoom &room);

        // Replaces tokens such as ":a:" with glyphs set in the icon font,
        // if one of iconFamilies is installed; otherwise they stay as text.
        void applyIcons(QTextDocument &doc) const;
    };
} // namespace scribe
