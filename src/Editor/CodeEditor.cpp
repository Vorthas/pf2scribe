// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Editor/CodeEditor.h"

#include "Editor/ScribeHighlighter.h"
#include "VimEngine/VimEngine.h"

#include <QAction>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QTextBlock>

namespace
{
    // Paints the line numbers to the left of the editor.
    class LineNumberArea : public QWidget
    {
        public:
        explicit LineNumberArea(CodeEditor *editor)
            : QWidget(editor), editor_(editor)
        {
        }

        QSize sizeHint() const override
        {
            return QSize(editor_->lineNumberAreaWidth(), 0);
        }

        protected:
        void paintEvent(QPaintEvent *event) override
        {
            editor_->paintLineNumbers(event);
        }

        private:
        CodeEditor *editor_;
    };
} // namespace

CodeEditor::CodeEditor(QWidget *parent) : QPlainTextEdit(parent)
{
    vim_ = new VimEngine(this);
    lineNumberArea_ = new LineNumberArea(this);

    connect(vim_, &VimEngine::stateChanged, this, &CodeEditor::updateVimState);
    connect(vim_, &VimEngine::optionsChanged, this, [this] {
        updateMargins();
        updateVimState();
    });
    connect(this, &QPlainTextEdit::blockCountChanged, this,
            &CodeEditor::updateMargins);
    connect(this, &QPlainTextEdit::updateRequest, this,
            [this](const QRect &rect, int dy) {
                if (dy != 0)
                {
                    lineNumberArea_->scroll(0, dy);
                }
                else
                {
                    lineNumberArea_->update(0, rect.y(), lineNumberArea_->width(),
                                            rect.height());
                }
            });
    // Relative numbers change with the cursor; the highlights follow it.
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (vim_->isEnabled())
        {
            lineNumberArea_->update();
            updateVimState();
        }
    });
    updateMargins();

    // Attached while the document is still empty, and for good: attached to
    // a document holding text, Qt colours it on the next event loop turn,
    // reported as a text change (see rehighlight()).
    highlighter_ = new ScribeHighlighter(this);
    highlighter_->setDark(palette().color(QPalette::Base).lightness() < 128);
    highlighter_->setDocument(document());
}

void CodeEditor::setSyntaxHighlighting(bool on)
{
    if (on != highlighter_->isActive())
    {
        highlighter_->setActive(on);
        rehighlight();
    }
}

// Colours the whole document again. Qt does it inside an edit block, which
// the document reports as a change of its text (though nothing changed and
// nothing is added to the undo history); with its signals blocked, the
// window doesn't render again, the find bar doesn't search again and Vim
// doesn't count it as an edit. The view repaints all the same.
void CodeEditor::rehighlight()
{
    const QSignalBlocker blocker(document());
    highlighter_->rehighlight();
    viewport()->update();
}

void CodeEditor::changeEvent(QEvent *event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::StyleChange)
    {
        const bool dark = palette().color(QPalette::Base).lightness() < 128;
        highlighter_->setDark(dark);
        if (highlighter_->isActive())
        {
            rehighlight();
        }
    }
}

void CodeEditor::setVimEnabled(bool on)
{
    vim_->setEnabled(on);
    updateMargins();
    updateVimState();
}

bool CodeEditor::showLineNumbers() const
{
    return vim_->isEnabled() &&
           (vim_->options().number || vim_->options().relativeNumber);
}

int CodeEditor::lineNumberAreaWidth() const
{
    if (!showLineNumbers())
    {
        return 0;
    }
    int digits = 1;
    for (int max = std::max(1, blockCount()); max >= 10; max /= 10)
    {
        ++digits;
    }
    // Vim's 'numberwidth' default: at least 3 digits, then a space.
    digits = std::max(3, digits);
    return 8 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * (digits + 1);
}

void CodeEditor::updateMargins()
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
    lineNumberArea_->setVisible(showLineNumbers());
    const QRect r = contentsRect();
    lineNumberArea_->setGeometry(r.left(), r.top(), lineNumberAreaWidth(), r.height());
    // Tabs as wide as Vim's 'tabstop' in Vim mode, else 4 spaces.
    const int tabStop = vim_->isEnabled() ? vim_->options().tabStop : 4;
    setTabStopDistance(tabStop * fontMetrics().horizontalAdvance(QLatin1Char(' ')));
}

void CodeEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect r = contentsRect();
    lineNumberArea_->setGeometry(r.left(), r.top(), lineNumberAreaWidth(), r.height());
}

void CodeEditor::paintLineNumbers(QPaintEvent *event)
{
    QPainter painter(lineNumberArea_);
    const QPalette pal = palette();
    painter.fillRect(event->rect(), pal.color(QPalette::AlternateBase));
    painter.setFont(font());

    const bool relative = vim_->options().relativeNumber;
    const bool absolute = vim_->options().number;
    const int current = textCursor().blockNumber();
    const int width = lineNumberArea_->width() - 4 -
                      fontMetrics().horizontalAdvance(QLatin1Char(' '));

    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    while (block.isValid() && top <= event->rect().bottom())
    {
        if (block.isVisible() && bottom >= event->rect().top())
        {
            int shown = number + 1;
            Qt::Alignment align = Qt::AlignRight;
            if (relative && number != current)
            {
                shown = std::abs(number - current);
            }
            else if (relative && !absolute)
            {
                shown = 0;
            }
            else if (relative && absolute)
            {
                align = Qt::AlignLeft; // Vim: the current line's number on the left
            }
            painter.setPen(number == current ? pal.color(QPalette::Text)
                                             : pal.color(QPalette::PlaceholderText));
            painter.drawText(4, top, width, fontMetrics().height(),
                             align | Qt::AlignTop, QString::number(shown));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

bool CodeEditor::event(QEvent *e)
{
    // Keys such as Ctrl+R or Ctrl+V go to Vim, not to menu shortcuts.
    if (e->type() == QEvent::ShortcutOverride && vim_->isEnabled())
    {
        auto *key = static_cast<QKeyEvent *>(e);
        if (vim_->wantsShortcut(key))
        {
            e->accept();
            return true;
        }
        const QKeySequence pressed(key->keyCombination());
        for (const QAction *action : std::as_const(vimReserved_))
        {
            if (action->shortcuts().contains(pressed))
            {
                e->accept();
                return true;
            }
        }
    }
    return QPlainTextEdit::event(e);
}

void CodeEditor::keyPressEvent(QKeyEvent *event)
{
    if (vim_->isEnabled() && vim_->handleKey(event))
    {
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

void CodeEditor::setFindHighlights(
    const QList<QTextEdit::ExtraSelection> &highlights)
{
    findHighlights_ = highlights;
    updateVimState();
}

void CodeEditor::updateVimState()
{
    if (!vim_->isEnabled())
    {
        setCursorWidth(1);
        setExtraSelections(findHighlights_);
        return;
    }
    // A block cursor outside Insert mode, as in Vim.
    const VimEngine::Mode mode = vim_->mode();
    const bool insert = mode == VimEngine::Mode::Insert;
    setCursorWidth(insert ? 1 : fontMetrics().horizontalAdvance(QLatin1Char('x')));
    setExtraSelections(findHighlights_ + vim_->extraSelections());
}

VimStatusLine::VimStatusLine(VimEngine *vim, QWidget *parent)
    : QWidget(parent), vim_(vim)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 2);
    left_ = new QLabel;
    right_ = new QLabel;
    left_->setTextFormat(Qt::PlainText);
    right_->setTextFormat(Qt::PlainText);
    layout->addWidget(left_, 1);
    layout->addWidget(right_);
    connect(vim_, &VimEngine::stateChanged, this, &VimStatusLine::refresh);
    connect(vim_, &VimEngine::optionsChanged, this, &VimStatusLine::refresh);
    refresh();
}

void VimStatusLine::refresh()
{
    setVisible(vim_->isEnabled());
    QString text = vim_->commandLineText();
    bool error = false;
    if (text.isEmpty())
    {
        text = vim_->message();
        error = vim_->messageIsError();
    }
    if (text.isEmpty())
    {
        text = vim_->modeText();
    }
    else if (vim_->commandLineText().isEmpty() && !vim_->modeText().isEmpty())
    {
        text = vim_->modeText() + QStringLiteral("  ") + text;
    }
    left_->setText(text);
    QFont font = left_->font();
    font.setBold(!vim_->modeText().isEmpty() && vim_->commandLineText().isEmpty() &&
                 vim_->message().isEmpty());
    left_->setFont(font);
    left_->setStyleSheet(error ? QStringLiteral("color: #d03030;") : QString());
    right_->setText(vim_->pendingKeys());
}
