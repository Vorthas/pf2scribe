// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Editor/FindBar.h"

#include "Editor/CodeEditor.h"
#include "VimEngine/VimEngine.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBlock>
#include <QToolButton>

QToolButton *FindBar::toggleButton(const QString &text, const QString &tip)
{
    auto *button = new QToolButton;
    button->setText(text);
    button->setToolTip(tip);
    button->setCheckable(true);
    button->setAutoRaise(true);
    return button;
}

QRegularExpression FindBar::makePattern(QString text, bool matchCase,
                                        bool wholeWords, bool regex)
{
    if (!regex)
    {
        text = QRegularExpression::escape(text);
    }
    if (wholeWords)
    {
        text = QStringLiteral("\\b(?:") + text + QStringLiteral(")\\b");
    }
    QRegularExpression::PatternOptions options =
        QRegularExpression::MultilineOption |
        QRegularExpression::UseUnicodePropertiesOption;
    if (!matchCase)
    {
        options |= QRegularExpression::CaseInsensitiveOption;
    }
    return QRegularExpression(text, options);
}

FindBar::FindBar(CodeEditor *editor, QWidget *parent)
    : QWidget(parent), editor_(editor)
{
    findEdit_ = new QLineEdit;
    findEdit_->setPlaceholderText(tr("Find"));
    findEdit_->setClearButtonEnabled(true);
    replaceEdit_ = new QLineEdit;
    replaceEdit_->setPlaceholderText(tr("Replace"));
    replaceEdit_->setToolTip(
        tr("With regular expressions, \\1 or $1 inserts a captured group"));
    findEdit_->installEventFilter(this);
    replaceEdit_->installEventFilter(this);

    caseButton_ = toggleButton(QStringLiteral("Aa"), tr("Match case"));
    wordButton_ = toggleButton(QStringLiteral("W"), tr("Whole words"));
    regexButton_ = toggleButton(QStringLiteral(".*"), tr("Regular expression"));

    auto *previous = new QToolButton;
    previous->setArrowType(Qt::UpArrow);
    previous->setAutoRaise(true);
    previous->setToolTip(tr("Previous match (Shift+Enter)"));
    auto *next = new QToolButton;
    next->setArrowType(Qt::DownArrow);
    next->setAutoRaise(true);
    next->setToolTip(tr("Next match (Enter)"));
    auto *close = new QToolButton;
    close->setText(QStringLiteral("✕"));
    close->setAutoRaise(true);
    close->setToolTip(tr("Close (Esc)"));
    status_ = new QLabel;
    status_->setMinimumWidth(status_->fontMetrics().horizontalAdvance(
        QStringLiteral("999 of 999")));

    auto *replaceButton = new QPushButton(tr("Replace"));
    auto *replaceAllButton = new QPushButton(tr("Replace All"));

    auto *layout = new QGridLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setHorizontalSpacing(4);
    layout->setVerticalSpacing(4);
    auto *findRow = new QHBoxLayout;
    findRow->addWidget(caseButton_);
    findRow->addWidget(wordButton_);
    findRow->addWidget(regexButton_);
    findRow->addWidget(previous);
    findRow->addWidget(next);
    findRow->addWidget(status_);
    findRow->addWidget(close);
    layout->addWidget(findEdit_, 0, 0);
    layout->addLayout(findRow, 0, 1);

    replaceRow_ = new QWidget;
    auto *replaceLayout = new QHBoxLayout(replaceRow_);
    replaceLayout->setContentsMargins(0, 0, 0, 0);
    replaceLayout->addWidget(replaceButton);
    replaceLayout->addWidget(replaceAllButton);
    replaceLayout->addStretch();
    layout->addWidget(replaceEdit_, 1, 0);
    layout->addWidget(replaceRow_, 1, 1);
    layout->setColumnStretch(0, 1);

    connect(findEdit_, &QLineEdit::textChanged, this,
            [this] { find(true, origin_, true); });
    for (QToolButton *button : {caseButton_, wordButton_, regexButton_})
    {
        connect(button, &QToolButton::toggled, this,
                [this] { find(true, origin_, true); });
    }
    connect(previous, &QToolButton::clicked, this, &FindBar::findPrevious);
    connect(next, &QToolButton::clicked, this, &FindBar::findNext);
    connect(close, &QToolButton::clicked, this, &FindBar::closeBar);
    connect(replaceButton, &QPushButton::clicked, this, &FindBar::replaceOne);
    connect(replaceAllButton, &QPushButton::clicked, this, &FindBar::replaceAll);

    // Keep the highlights in step with edits.
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(150);
    connect(&refreshTimer_, &QTimer::timeout, this, &FindBar::refresh);
    connect(editor_, &QPlainTextEdit::textChanged, this, [this] {
        if (isVisible())
        {
            refreshTimer_.start();
        }
    });

    hide();
}

void FindBar::openFind()
{
    open(false);
}

void FindBar::openReplace()
{
    open(true);
}

void FindBar::open(bool replace)
{
    const QTextCursor cursor = editor_->textCursor();
    origin_ = cursor.selectionStart();
    // A one-line selection becomes the search text.
    const QString selected = cursor.selectedText();
    if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator))
    {
        findEdit_->setText(selected);
    }
    replaceEdit_->setVisible(replace);
    replaceRow_->setVisible(replace);
    show();
    findEdit_->setFocus();
    findEdit_->selectAll();
    find(true, origin_, true);
}

void FindBar::closeBar()
{
    hide();
    current_ = {-1, 0};
    editor_->setFindHighlights({});
    editor_->setFocus();
}

bool FindBar::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress)
    {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape)
        {
            closeBar();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            if (watched == replaceEdit_)
            {
                replaceOne();
            }
            else if (key->modifiers() & Qt::ShiftModifier)
            {
                findPrevious();
            }
            else
            {
                findNext();
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

QRegularExpression FindBar::pattern() const
{
    return makePattern(findEdit_->text(), caseButton_->isChecked(),
                       wordButton_->isChecked(), regexButton_->isChecked());
}

QList<FindBar::Match> FindBar::matches() const
{
    QList<Match> list;
    const QRegularExpression re = pattern();
    if (findEdit_->text().isEmpty() || !re.isValid())
    {
        return list;
    }
    auto it = re.globalMatch(editor_->toPlainText());
    while (it.hasNext())
    {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedLength() > 0) // "x*" and the like: skip empty matches
        {
            list.append({static_cast<int>(m.capturedStart()),
                         static_cast<int>(m.capturedLength())});
        }
    }
    return list;
}

void FindBar::findNext()
{
    if (!isVisible() || findEdit_->text().isEmpty())
    {
        openFind();
        return;
    }
    const int from = current_.start >= 0
                         ? current_.start + std::max(1, current_.length)
                         : editor_->textCursor().position();
    find(true, from, true);
    if (current_.start >= 0)
    {
        origin_ = current_.start;
    }
}

void FindBar::findPrevious()
{
    if (!isVisible() || findEdit_->text().isEmpty())
    {
        openFind();
        return;
    }
    const int from = current_.start >= 0 ? current_.start
                                          : editor_->textCursor().position();
    find(false, from, false);
    if (current_.start >= 0)
    {
        origin_ = current_.start;
    }
}

// The first match at or after `from` (or the last one before it), wrapping
// around the document.
void FindBar::find(bool forward, int from, bool inclusive)
{
    const QList<Match> list = matches();
    current_ = {-1, 0};
    if (!list.isEmpty())
    {
        if (forward)
        {
            current_ = list.first();
            for (const Match &m : list)
            {
                if (inclusive ? m.start >= from : m.start > from)
                {
                    current_ = m;
                    break;
                }
            }
        }
        else
        {
            current_ = list.last();
            for (auto it = list.rbegin(); it != list.rend(); ++it)
            {
                if (it->start < from)
                {
                    current_ = *it;
                    break;
                }
            }
        }
        select(current_);
    }
    refresh();
}

void FindBar::select(const Match &match)
{
    QTextCursor cursor = editor_->textCursor();
    cursor.setPosition(match.start);
    // In Vim mode a selection would start Visual mode: put the cursor on
    // the match instead (the highlight shows it).
    if (!editor_->vim()->isEnabled())
    {
        cursor.setPosition(match.start + match.length, QTextCursor::KeepAnchor);
    }
    editor_->setTextCursor(cursor);
    editor_->ensureCursorVisible();
}

// The match count and the highlights.
void FindBar::refresh()
{
    const QList<Match> list = matches();
    const bool invalid = !findEdit_->text().isEmpty() && !pattern().isValid();

    // The current match may have moved or gone after an edit.
    int index = -1;
    for (int i = 0; i < list.size(); ++i)
    {
        if (list.at(i).start == current_.start && list.at(i).length == current_.length)
        {
            index = i;
            break;
        }
    }
    if (index < 0)
    {
        current_ = {-1, 0};
    }

    QString text;
    if (invalid)
    {
        text = tr("Invalid pattern");
    }
    else if (findEdit_->text().isEmpty())
    {
        text.clear();
    }
    else if (list.isEmpty())
    {
        text = tr("No results");
    }
    else if (index >= 0)
    {
        text = tr("%1 of %2").arg(index + 1).arg(list.size());
    }
    else
    {
        text = tr("%n match(es)", nullptr, static_cast<int>(list.size()));
    }
    status_->setText(text);
    findEdit_->setStyleSheet(invalid || (!findEdit_->text().isEmpty() && list.isEmpty())
                                 ? QStringLiteral("QLineEdit { color: #d03030; }")
                                 : QString());

    QList<QTextEdit::ExtraSelection> highlights;
    QTextCharFormat all;
    all.setBackground(QColor(0xff, 0xdc, 0x5a));
    all.setForeground(Qt::black);
    QTextCharFormat currentFormat;
    currentFormat.setBackground(QColor(0xff, 0x96, 0x32));
    currentFormat.setForeground(Qt::black);
    for (int i = 0; i < list.size() && i < 5000; ++i)
    {
        QTextEdit::ExtraSelection s;
        s.cursor = QTextCursor(editor_->document());
        s.cursor.setPosition(list.at(i).start);
        s.cursor.setPosition(list.at(i).start + list.at(i).length,
                             QTextCursor::KeepAnchor);
        s.format = i == index ? currentFormat : all;
        highlights << s;
    }
    editor_->setFindHighlights(highlights);
}

// "$1" / "\\1" a group (regular expressions only), "\\n" a line break.
QString FindBar::replacementFor(const QString &matched) const
{
    const QString replacement = replaceEdit_->text();
    if (!regexButton_->isChecked())
    {
        return replacement;
    }
    const QRegularExpressionMatch m = pattern().match(
        matched, 0, QRegularExpression::NormalMatch,
        QRegularExpression::AnchorAtOffsetMatchOption);
    QString out;
    for (int i = 0; i < replacement.size(); ++i)
    {
        const QChar c = replacement.at(i);
        const QChar n = i + 1 < replacement.size() ? replacement.at(i + 1) : QChar();
        if ((c == QLatin1Char('$') || c == QLatin1Char('\\')) && n.isDigit())
        {
            out += m.captured(n.digitValue());
            ++i;
        }
        else if (c == QLatin1Char('$') && n == QLatin1Char('$'))
        {
            out += QLatin1Char('$');
            ++i;
        }
        else if (c == QLatin1Char('\\') && n == QLatin1Char('n'))
        {
            out += QLatin1Char('\n');
            ++i;
        }
        else if (c == QLatin1Char('\\') && n == QLatin1Char('t'))
        {
            out += QLatin1Char('\t');
            ++i;
        }
        else if (c == QLatin1Char('\\') && n == QLatin1Char('\\'))
        {
            out += QLatin1Char('\\');
            ++i;
        }
        else
        {
            out += c;
        }
    }
    return out;
}

void FindBar::replaceOne()
{
    if (current_.start < 0)
    {
        findNext();
        return;
    }
    QTextCursor cursor(editor_->document());
    cursor.setPosition(current_.start);
    cursor.setPosition(current_.start + current_.length, QTextCursor::KeepAnchor);
    const QString matched =
        cursor.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    const QString text = replacementFor(matched);
    cursor.insertText(text);
    // On to the next match after the replaced text.
    const int after = current_.start + static_cast<int>(text.size());
    current_ = {-1, 0};
    find(true, after, true);
    if (current_.start >= 0)
    {
        origin_ = current_.start;
    }
}

void FindBar::replaceAll()
{
    const QList<Match> list = matches();
    if (list.isEmpty())
    {
        refresh();
        return;
    }
    // From the end, so positions stay valid; one undo step.
    QTextCursor cursor(editor_->document());
    cursor.beginEditBlock();
    for (auto it = list.rbegin(); it != list.rend(); ++it)
    {
        cursor.setPosition(it->start);
        cursor.setPosition(it->start + it->length, QTextCursor::KeepAnchor);
        const QString matched =
            cursor.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        cursor.insertText(replacementFor(matched));
    }
    cursor.endEditBlock();
    current_ = {-1, 0};
    refresh();
    status_->setText(tr("Replaced %n", nullptr, static_cast<int>(list.size())));
}
