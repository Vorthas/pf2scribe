// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

// Colours Markdown and Scribe's own syntax in the editor: blocks such as
// "item(" ... ")", section and page breaks, trait lines, action icons,
// table-of-contents markers and rules, besides headings, emphasis, code,
// links, lists, comments and the hidden part after "%".
//
// It follows the parser's rules, carrying them from line to line: a block
// only ends at a line that is just ")", so a ")" inside a line never closes
// one, and code fences, comments, "name {" definitions and the hidden part
// last until their own end.
class ScribeHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT
    public:
    explicit ScribeHighlighter(QObject *parent = nullptr);

    // Colours for a dark background or a light one, and whether to colour
    // at all. Neither re-colours the document; see CodeEditor.
    void setDark(bool dark);
    void setActive(bool active) { active_ = active; }
    bool isActive() const { return active_; }

    protected:
    void highlightBlock(const QString &text) override;

    private:
    // What the line before left open.
    struct State
    {
        int block = 0; // index in blockNames() + 1; 0 outside blocks
        bool fence = false;
        bool comment = false;
        bool definition = false;
        bool hidden = false;
    };
    static State decode(int value);
    static int encode(const State &state);

    void markdown(const QString &text);
    void inline_(const QString &text, int from);
    void traits(const QString &text);
    void merge(int start, int length, const QTextCharFormat &format);

    bool dark_ = false;
    bool active_ = true;
    QTextCharFormat heading_, block_, break_, keyword_, error_, icon_,
        trait_, toc_, rule_, code_, link_, dim_, comment_, number_,
        marker_, bold_, italic_;
};
