// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include "Rendering/PageLayout.h"

#include <QRegularExpression>
#include <QWidget>

#include <vector>

class PreviewWidget;
class QLabel;
class QLineEdit;
class QToolButton;

// Find (no replace) in the rendered pages: a bar under the preview with the
// same options as the editor's, every match highlighted and the page of the
// current one shown. Searches the text as copying gives it, so badges match
// their words and icons their tokens (":a:").
class PreviewFindBar : public QWidget
{
    Q_OBJECT
    public:
    explicit PreviewFindBar(PreviewWidget *preview, QWidget *parent = nullptr);

    void openFind();
    void findNext();
    void findPrevious();
    void closeBar();

    protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

    private:
    QRegularExpression pattern() const;
    void search(const scribe::TextPoint &from, bool scroll = true);
    void step(bool forward);
    void layoutChanged();
    void showMatches(bool scroll);

    PreviewWidget *preview_;
    QLineEdit *findEdit_;
    QToolButton *caseButton_;
    QToolButton *wordButton_;
    QToolButton *regexButton_;
    QLabel *status_;

    // The laid-out text, made once per layout.
    std::shared_ptr<const scribe::PageLayout> textOf_;
    scribe::PlainText text_;
    std::vector<scribe::TextSelection> matches_;
    int current_ = -1;
};
