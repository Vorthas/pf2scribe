// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include <QLabel>
#include <QPlainTextEdit>

class QAction;
class ScribeHighlighter;
class VimEngine;

// The markdown editor: a plain text editor with optional Vim motions, line
// numbers (Vim's 'number' / 'relativenumber') and a block cursor outside
// Insert mode.
class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT
    public:
    explicit CodeEditor(QWidget *parent = nullptr);

    VimEngine *vim() const { return vim_; }
    void setVimEnabled(bool on);

    // Colours Markdown and Scribe's syntax (on by default).
    void setSyntaxHighlighting(bool on);

    // In Vim mode the keys of these actions (Edit > Undo, Copy, ...) go to
    // Vim, whatever they are bound to; the menu items still work.
    void setVimReservedActions(const QList<QAction *> &actions)
    {
        vimReserved_ = actions;
    }

    // Matches shown by the find bar, under Vim's own highlights.
    void setFindHighlights(const QList<QTextEdit::ExtraSelection> &highlights);

    // For the line number area.
    int lineNumberAreaWidth() const;
    void paintLineNumbers(QPaintEvent *event);

    protected:
    bool event(QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    private:
    void updateVimState();
    void rehighlight();
    void updateMargins();
    bool showLineNumbers() const;

    VimEngine *vim_ = nullptr;
    ScribeHighlighter *highlighter_ = nullptr;
    QWidget *lineNumberArea_ = nullptr;
    QList<QTextEdit::ExtraSelection> findHighlights_;
    QList<QAction *> vimReserved_;
};

// The line under the editor in Vim mode: the mode or the command being
// typed on the left, pending keys on the right.
class VimStatusLine : public QWidget
{
    Q_OBJECT
    public:
    explicit VimStatusLine(VimEngine *vim, QWidget *parent = nullptr);

    private:
    void refresh();

    VimEngine *vim_;
    QLabel *left_;
    QLabel *right_;
};
