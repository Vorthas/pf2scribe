// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include <QList>
#include <QPointF>
#include <QString>
#include <QStringList>

namespace scribe
{

    // Starts a paragraph that holds an item's trait list
    // ("uncommon,class,feat"), for Theme to draw as badges. A private-use
    // character, so it can't clash with document text.
    inline constexpr char16_t kTraitsMarker = u'\uE000';

    // A block inside a column.
    enum class BlockKind
    {
        Markdown, // plain flowed markdown
        Info,     // info ( ... )   red box, may contain "|" for inner columns
        Rules,    // rules ( ... )
        Note,     // note ( ... )   tan sticky-style box
        Math,     // math ( ... )
        Item,     // item ( ... )   stat-block style entry (feats, spells, ...)
        SidebarLeft,  // the sidebar of left ( ... )
        SidebarRight, // the sidebar of right ( ... )
        Sticky, // sticky(X Y ... ): a note pinned to the page, outside the flow
    };

    struct Block
    {
        BlockKind kind = BlockKind::Markdown;
        QStringList columns; // markdown per inner column; size 1 except for
                             // boxes that use "|"
        QPointF position{};  // Sticky only: left and top, in mm from the page's
                             // corner
    };

    struct Column
    {
        QList<Block> blocks;
    };

    // A section is a horizontal band of the page. Its column count is the
    // number of "|" lines in it plus one. Sections end at "/" (or "=", which
    // also starts a new page).
    enum class SectionKind
    {
        Normal,
        Head, // head ( ... ): full-width header block
        Left, // left ( ... ): sidebar (about 1/3) on the left, main text on the
              // right
        Right, // right ( ... ): sidebar on the right
    };

    struct Section
    {
        SectionKind kind = SectionKind::Normal;
        bool pageBreakBefore = false;
        QList<Column> columns; // for Left/Right: [0] = sidebar, [1] = main text
    };

    struct TocEntry
    {
        int level = 0; // number of '+' in the ((+Label)) marker
        QString text;
        int line = -1; // line of its heading in the source, from 0
    };

    // Where the heading of table-of-contents entry N ends up: the parser
    // puts kTocAnchorOpen, N and kTocAnchorClose at the end of the heading
    // line in the markdown it hands on, for PageLayout to find (and remove
    // before the text is laid out).
    constexpr char16_t kTocAnchorOpen = 0xE012;
    constexpr char16_t kTocAnchorClose = 0xE013;

    // `text` without the parser's private table-of-contents tags.
    QString withoutTocTags(QString text);

    struct Document
    {
        QString title; // title ( ... ): banner text at the top of every page
        QString watermark; // watermark ( ... ): italic line above it
        QString css; // css ( ... ): raw user CSS, kept but not interpreted yet
        QStringList
            fonts; // fonts ( ... ): Google Fonts import lines, not used yet
        bool pageNumbers = false; // "pagenumbers" on a line by itself
        bool reset = false;       // "reset" on a line by itself
        QList<Section> sections;
        QList<TocEntry> toc;
        QStringList warnings;
    };

    Document parse(const QString &source);

} // namespace scribe
