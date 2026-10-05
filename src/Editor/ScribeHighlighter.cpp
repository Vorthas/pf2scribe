// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Editor/ScribeHighlighter.h"

#include <QFont>
#include <QRegularExpression>
#include <QStringList>

namespace
{
    // The parser's block names (ScribeParser.cpp), in a fixed order: a
    // line's state stores the index of the block it is in.
    const QStringList &blockNames()
    {
        static const QStringList names{
            "watermark", "title", "head", "css",  "fonts", "sticky", "info",
            "rules",     "note",  "math", "left", "right", "item"};
        return names;
    }

    int blockIndex(const QString &name)
    {
        return static_cast<int>(blockNames().indexOf(name.toLower())) + 1;
    }

    // "item(", "head (", "sticky(50 218": the name, then what follows "(".
    const QRegularExpression kBlockOpen(
        QStringLiteral(R"(^\s*([A-Za-z][A-Za-z0-9_-]*)\s*\((.*)$)"));
    // "name {" opens a reusable definition, "{{name}}" uses one.
    const QRegularExpression kDefOpen(
        QStringLiteral(R"(^\s*([A-Za-z][A-Za-z0-9_-]*)\s*\{\s*$)"));
    const QRegularExpression kRefLine(
        QStringLiteral(R"(^\s*\{\{\s*([^{}]+?)\s*\}\}\s*$)"));
    // A trailing "((Title))", "((+Info))", "((++Sub))" on a heading.
    const QRegularExpression kTocMarker(
        QStringLiteral(R"(\(\(\s*\+*\s*[^)]*?\s*\)\)\s*$)"));
    const QRegularExpression kHeading(QStringLiteral(R"(^\s{0,3}#{1,6}(\s|$))"));
    const QRegularExpression kRule(
        QStringLiteral(R"(^\s*(-|([-*_]\s*){3,})\s*$)"));
    const QRegularExpression kListMarker(
        QStringLiteral(R"(^\s*([-*+]|\d+[.)])(?=\s))"));
    const QRegularExpression kQuote(QStringLiteral(R"(^\s*>)"));
    const QRegularExpression kIcon(QStringLiteral(R"(:(a{1,3}|r|f):)"));
    const QRegularExpression kCodeSpan(QStringLiteral(R"(`[^`]+`)"));
    const QRegularExpression kBoldItalic(
        QStringLiteral(R"((\*\*\*|___)(?=\S).+?(?<=\S)\1)"));
    const QRegularExpression kBold(QStringLiteral(R"((\*\*|__)(?=\S).+?(?<=\S)\1)"));
    const QRegularExpression kItalic(QStringLiteral(
        R"((?<![*\w])(\*|_)(?=[^\s*_])(.+?)(?<=[^\s*_])\1(?![*\w]))"));
    const QRegularExpression kLink(
        QStringLiteral(R"(!?\[[^\]]*\]\([^)\s]*\))"));
    const QRegularExpression kNumbers(QStringLiteral(R"(-?\d+(\.\d+)?)"));

    bool isFence(const QString &trimmed)
    {
        return trimmed.startsWith(QLatin1String("```")) ||
               trimmed.startsWith(QLatin1String("~~~"));
    }

    QTextCharFormat colour(const QColor &color, bool bold = false,
                           bool italic = false)
    {
        QTextCharFormat format;
        format.setForeground(color);
        if (bold)
        {
            format.setFontWeight(QFont::Bold);
        }
        if (italic)
        {
            format.setFontItalic(true);
        }
        return format;
    }
} // namespace

ScribeHighlighter::ScribeHighlighter(QObject *parent)
    : QSyntaxHighlighter(static_cast<QTextDocument *>(nullptr))
{
    setParent(parent);
    setDark(false);
}

void ScribeHighlighter::setDark(bool dark)
{
    dark_ = dark;
    // Two sets of colours that read well on the platform's light or dark
    // editor background; the text itself keeps the theme's colour.
    auto pick = [dark](const char *light, const char *darkColor) {
        return QColor(QLatin1String(dark ? darkColor : light));
    };
    heading_ = colour(pick("#1f4e9e", "#79b8ff"), true);
    block_ = colour(pick("#7b2fbf", "#c792ea"), true);
    break_ = colour(pick("#c25e00", "#ffab70"), true);
    keyword_ = colour(pick("#7b2fbf", "#c792ea"));
    error_ = colour(pick("#d32f2f", "#ff6b6b"), true);
    error_.setUnderlineStyle(QTextCharFormat::WaveUnderline);
    error_.setUnderlineColor(error_.foreground().color());
    icon_ = colour(pick("#00838f", "#4dd0e1"), true);
    trait_ = colour(pick("#2e7d32", "#8bd88b"));
    toc_ = colour(pick("#a0569b", "#d7a6d0"), false, true);
    rule_ = colour(pick("#888888", "#8c8c8c"), true);
    code_ = colour(pick("#b3261e", "#f97583"));
    link_ = colour(pick("#1565c0", "#82aaff"));
    dim_ = colour(pick("#888888", "#8c8c8c"));
    comment_ = colour(pick("#8a8a8a", "#7a7a7a"), false, true);
    number_ = colour(pick("#00838f", "#4dd0e1"));
    marker_ = colour(pick("#c25e00", "#ffab70"));
    bold_ = QTextCharFormat();
    bold_.setFontWeight(QFont::Bold);
    italic_ = QTextCharFormat();
    italic_.setFontItalic(true);
}

ScribeHighlighter::State ScribeHighlighter::decode(int value)
{
    State s;
    if (value < 0)
    {
        return s;
    }
    s.block = value & 0xff;
    s.fence = value & 0x100;
    s.comment = value & 0x200;
    s.definition = value & 0x400;
    s.hidden = value & 0x800;
    return s;
}

int ScribeHighlighter::encode(const State &s)
{
    return s.block | (s.fence ? 0x100 : 0) | (s.comment ? 0x200 : 0) |
           (s.definition ? 0x400 : 0) | (s.hidden ? 0x800 : 0);
}

// Adds `format` to what the characters already have (bold inside a
// heading stays bold and takes the heading's colour, and so on).
void ScribeHighlighter::merge(int start, int length,
                              const QTextCharFormat &format)
{
    for (int i = start; i < start + length; ++i)
    {
        QTextCharFormat f = this->format(i);
        f.merge(format);
        setFormat(i, 1, f);
    }
}

void ScribeHighlighter::highlightBlock(const QString &text)
{
    if (!active_)
    {
        return; // no colours, so the previous ones go
    }
    State s = decode(previousBlockState());
    const QString t = text.trimmed();
    const int n = static_cast<int>(text.size());
    auto done = [&] { setCurrentBlockState(encode(s)); };

    // In the order the parser reads them: definitions, comments, the
    // hidden part, then code fences and blocks.
    if (s.definition)
    {
        if (t == QLatin1String("}"))
        {
            setFormat(0, n, keyword_);
            s.definition = false;
        }
        else
        {
            markdown(text);
        }
        return done();
    }
    if (t == QLatin1String("<!--") || t == QLatin1String("-->") || s.comment ||
        (t.startsWith(QLatin1String("<!--")) && t.endsWith(QLatin1String("-->"))))
    {
        if (t == QLatin1String("<!--"))
            s.comment = true;
        else if (t == QLatin1String("-->"))
            s.comment = false;
        setFormat(0, n, comment_);
        return done();
    }
    if (s.hidden || t == QLatin1String("%"))
    {
        // "%" hides everything after it from the render.
        s.hidden = true;
        setFormat(0, n, comment_);
        return done();
    }

    if (s.block)
    {
        if (t == QLatin1String(")"))
        {
            // Only a line that is just ")" ends a block.
            setFormat(0, n, block_);
            s.block = 0;
            s.fence = false;
            return done();
        }
        const QString name = blockNames().value(s.block - 1);
        if (isFence(t))
        {
            s.fence = !s.fence;
            setFormat(0, n, code_);
            return done();
        }
        if (s.fence || name == QLatin1String("css"))
        {
            setFormat(0, n, code_);
            return done();
        }
        if (t == QLatin1String("|"))
        {
            setFormat(0, n, break_); // a column break inside a box
            return done();
        }
        if (name == QLatin1String("item") && t.startsWith(QLatin1Char(';')))
        {
            traits(text);
            return done();
        }
        markdown(text);
        return done();
    }

    if (isFence(t))
    {
        s.fence = !s.fence;
        setFormat(0, n, code_);
        return done();
    }
    if (s.fence)
    {
        setFormat(0, n, code_);
        return done();
    }

    if (const auto m = kBlockOpen.match(text); m.hasMatch())
    {
        const int index = blockIndex(m.captured(1));
        const QString rest = m.captured(2).trimmed();
        const bool sticky = m.captured(1).compare(QLatin1String("sticky"),
                                                  Qt::CaseInsensitive) == 0;
        if (index > 0 && (rest.isEmpty() || sticky))
        {
            setFormat(0, static_cast<int>(m.capturedStart(2)), block_);
            // The sticky note's position.
            auto numbers = kNumbers.globalMatch(text, m.capturedStart(2));
            while (numbers.hasNext())
            {
                const auto number = numbers.next();
                setFormat(static_cast<int>(number.capturedStart()),
                          static_cast<int>(number.capturedLength()), number_);
            }
            s.block = index;
            return done();
        }
    }
    if (t == QLatin1String("=") || t == QLatin1String("/") ||
        t == QLatin1String("|"))
    {
        setFormat(0, n, break_);
        return done();
    }
    if (t == QLatin1String("pagenumbers") || t == QLatin1String("reset"))
    {
        setFormat(0, n, keyword_);
        return done();
    }
    if (kDefOpen.match(text).hasMatch())
    {
        setFormat(0, n, keyword_);
        s.definition = true;
        return done();
    }
    if (kRefLine.match(text).hasMatch())
    {
        setFormat(0, n, keyword_);
        return done();
    }
    if (t == QLatin1String(")"))
    {
        // Closes nothing: shown as a mistake.
        setFormat(0, n, error_);
        return done();
    }
    markdown(text);
    done();
}

// "; Uncommon, N, Small": the traits of an item.
void ScribeHighlighter::traits(const QString &text)
{
    setFormat(0, static_cast<int>(text.size()), trait_);
    for (int i = 0; i < text.size(); ++i)
    {
        if (text.at(i) == QLatin1Char(';') || text.at(i) == QLatin1Char(','))
        {
            setFormat(i, 1, dim_);
        }
    }
}

// A line of Markdown, in a block or not.
void ScribeHighlighter::markdown(const QString &text)
{
    const int n = static_cast<int>(text.size());
    if (kRule.match(text).hasMatch())
    {
        setFormat(0, n, rule_);
        return;
    }
    int from = 0;
    if (const auto m = kHeading.match(text); m.hasMatch())
    {
        setFormat(0, n, heading_);
        if (const auto toc = kTocMarker.match(text); toc.hasMatch())
        {
            setFormat(static_cast<int>(toc.capturedStart()),
                      static_cast<int>(toc.capturedLength()), toc_);
        }
    }
    else if (const auto m = kListMarker.match(text); m.hasMatch())
    {
        setFormat(static_cast<int>(m.capturedStart(1)),
                  static_cast<int>(m.capturedLength(1)), marker_);
        from = static_cast<int>(m.capturedEnd());
    }
    else if (const auto m = kQuote.match(text); m.hasMatch())
    {
        setFormat(static_cast<int>(m.capturedEnd()) - 1, 1, marker_);
    }
    // Table rows: the column bars.
    if (text.contains(QLatin1Char('|')))
    {
        for (int i = 0; i < n; ++i)
        {
            if (text.at(i) == QLatin1Char('|'))
            {
                setFormat(i, 1, dim_);
            }
        }
    }
    inline_(text, from);
}

// Emphasis, code, links and icons within a line, from `from` on.
void ScribeHighlighter::inline_(const QString &text, int from)
{
    // Code spans first: nothing inside them is formatted.
    QList<std::pair<int, int>> code;
    auto spans = kCodeSpan.globalMatch(text, from);
    while (spans.hasNext())
    {
        const auto m = spans.next();
        const int start = static_cast<int>(m.capturedStart());
        const int length = static_cast<int>(m.capturedLength());
        code.append({start, start + length});
        merge(start, length, code_);
    }
    auto inCode = [&code](qsizetype start, qsizetype end) {
        for (const auto &[a, b] : code)
        {
            if (start < b && end > a)
            {
                return true;
            }
        }
        return false;
    };
    auto apply = [&](const QRegularExpression &re, const QTextCharFormat &format) {
        auto it = re.globalMatch(text, from);
        while (it.hasNext())
        {
            const auto m = it.next();
            if (!inCode(m.capturedStart(), m.capturedEnd()))
            {
                merge(static_cast<int>(m.capturedStart()),
                      static_cast<int>(m.capturedLength()), format);
            }
        }
    };
    QTextCharFormat boldItalic = bold_;
    boldItalic.merge(italic_);
    apply(kBoldItalic, boldItalic);
    apply(kBold, bold_);
    apply(kItalic, italic_);
    apply(kLink, link_);
    apply(kIcon, icon_);
}
