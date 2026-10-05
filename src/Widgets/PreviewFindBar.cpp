// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Widgets/PreviewFindBar.h"

#include "Editor/FindBar.h"
#include "Widgets/PreviewWidget.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

namespace
{
    // At most this many matches are highlighted (all are counted).
    constexpr int kMaxHighlights = 5000;
} // namespace

PreviewFindBar::PreviewFindBar(PreviewWidget *preview, QWidget *parent)
    : QWidget(parent), preview_(preview)
{
    findEdit_ = new QLineEdit;
    findEdit_->setPlaceholderText(tr("Find in pages"));
    findEdit_->setClearButtonEnabled(true);
    findEdit_->installEventFilter(this);

    caseButton_ = FindBar::toggleButton(QStringLiteral("Aa"), tr("Match case"));
    wordButton_ = FindBar::toggleButton(QStringLiteral("W"), tr("Whole words"));
    regexButton_ =
        FindBar::toggleButton(QStringLiteral(".*"), tr("Regular expression"));

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
        QStringLiteral("999 of 999, page 999")));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(4);
    layout->addWidget(findEdit_, 1);
    layout->addWidget(caseButton_);
    layout->addWidget(wordButton_);
    layout->addWidget(regexButton_);
    layout->addWidget(previous);
    layout->addWidget(next);
    layout->addWidget(status_);
    layout->addWidget(close);

    // Typing searches from the top of the page in view.
    auto fromView = [this] {
        search({preview_->firstVisiblePage(), 0, 0});
    };
    connect(findEdit_, &QLineEdit::textChanged, this, fromView);
    for (QToolButton *button : {caseButton_, wordButton_, regexButton_})
    {
        connect(button, &QToolButton::toggled, this, fromView);
    }
    connect(previous, &QToolButton::clicked, this, &PreviewFindBar::findPrevious);
    connect(next, &QToolButton::clicked, this, &PreviewFindBar::findNext);
    connect(close, &QToolButton::clicked, this, &PreviewFindBar::closeBar);
    connect(preview_, &PreviewWidget::layoutChanged, this,
            &PreviewFindBar::layoutChanged);

    hide();
}

void PreviewFindBar::openFind()
{
    // A one-line selection in the preview becomes the search text.
    if (const auto layout = preview_->pageLayout();
        layout && !preview_->selection().isEmpty())
    {
        const QString selected = layout->selectedText(preview_->selection());
        if (!selected.contains(QLatin1Char('\n')))
        {
            findEdit_->setText(selected);
        }
    }
    show();
    findEdit_->setFocus();
    findEdit_->selectAll();
    search({preview_->firstVisiblePage(), 0, 0});
}

void PreviewFindBar::closeBar()
{
    hide();
    matches_.clear();
    current_ = -1;
    preview_->setHighlights({});
    preview_->setFocus();
}

void PreviewFindBar::findNext()
{
    if (!isVisible() || findEdit_->text().isEmpty())
    {
        openFind();
        return;
    }
    step(true);
}

void PreviewFindBar::findPrevious()
{
    if (!isVisible() || findEdit_->text().isEmpty())
    {
        openFind();
        return;
    }
    step(false);
}

bool PreviewFindBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == findEdit_ && event->type() == QEvent::KeyPress)
    {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape)
        {
            closeBar();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            if (key->modifiers() & Qt::ShiftModifier)
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

QRegularExpression PreviewFindBar::pattern() const
{
    return FindBar::makePattern(findEdit_->text(), caseButton_->isChecked(),
                                wordButton_->isChecked(),
                                regexButton_->isChecked());
}

// Finds every match, making the first one at or after `from` current, and
// scrolls to it if `scroll`.
void PreviewFindBar::search(const scribe::TextPoint &from, bool scroll)
{
    matches_.clear();
    current_ = -1;
    const auto layout = preview_->pageLayout();
    const QRegularExpression re = pattern();
    if (layout && !findEdit_->text().isEmpty() && re.isValid())
    {
        if (textOf_ != layout)
        {
            text_ = layout->plainText();
            textOf_ = layout;
        }
        auto it = re.globalMatch(text_.text);
        while (it.hasNext())
        {
            const QRegularExpressionMatch m = it.next();
            if (m.capturedLength() <= 0)
            {
                continue; // "x*" and the like: skip empty matches
            }
            const auto first = static_cast<size_t>(m.capturedStart());
            const auto last = static_cast<size_t>(m.capturedEnd() - 1);
            scribe::TextPoint end = text_.points[last];
            end.pos += 1; // after the last character
            matches_.push_back({text_.points[first], end});
        }
        for (size_t i = 0; i < matches_.size(); ++i)
        {
            if (!(matches_[i].anchor < from))
            {
                current_ = static_cast<int>(i);
                break;
            }
        }
        if (current_ < 0 && !matches_.empty())
        {
            current_ = 0; // wrap around
        }
    }
    showMatches(scroll);
}

void PreviewFindBar::step(bool forward)
{
    if (matches_.empty())
    {
        search({preview_->firstVisiblePage(), 0, 0});
        return;
    }
    const int count = static_cast<int>(matches_.size());
    current_ = current_ < 0 ? 0 : (current_ + (forward ? 1 : count - 1)) % count;
    showMatches(true);
}

// A new layout after an edit: search it again, keeping the current match
// where it was without scrolling away from what is being read.
void PreviewFindBar::layoutChanged()
{
    textOf_.reset();
    text_ = {};
    if (!isVisible())
    {
        return;
    }
    const scribe::TextPoint was = current_ >= 0
                                      ? matches_[static_cast<size_t>(current_)].anchor
                                      : scribe::TextPoint{preview_->firstVisiblePage(), 0, 0};
    matches_.clear();
    search(was, false);
}

void PreviewFindBar::showMatches(bool scroll)
{
    const bool invalid = !findEdit_->text().isEmpty() && !pattern().isValid();
    QString text;
    if (invalid)
    {
        text = tr("Invalid pattern");
    }
    else if (findEdit_->text().isEmpty())
    {
        text.clear();
    }
    else if (matches_.empty())
    {
        text = tr("No results");
    }
    else
    {
        const auto &match = matches_[static_cast<size_t>(current_)];
        text = tr("%1 of %2, page %3")
                   .arg(current_ + 1)
                   .arg(matches_.size())
                   .arg(match.anchor.page + 1);
    }
    status_->setText(text);
    findEdit_->setStyleSheet(
        invalid || (!findEdit_->text().isEmpty() && matches_.empty())
            ? QStringLiteral("QLineEdit { color: #d03030; }")
            : QString());

    // The same colours as the editor's find bar.
    std::vector<scribe::Highlight> highlights;
    const QColor all(0xff, 0xdc, 0x5a);
    const QColor currentColor(0xff, 0x96, 0x32);
    for (size_t i = 0; i < matches_.size() && i < kMaxHighlights; ++i)
    {
        if (static_cast<int>(i) != current_)
        {
            highlights.push_back({matches_[i], all, Qt::black});
        }
    }
    if (current_ >= 0)
    {
        // Last, so it's drawn over its neighbours.
        highlights.push_back(
            {matches_[static_cast<size_t>(current_)], currentColor, Qt::black});
    }
    preview_->setHighlights(std::move(highlights));
    if (scroll && current_ >= 0)
    {
        preview_->showPoint(matches_[static_cast<size_t>(current_)].anchor);
    }
}
