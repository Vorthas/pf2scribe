// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include <QRegularExpression>
#include <QTimer>
#include <QWidget>

class CodeEditor;
class QLabel;
class QLineEdit;
class QToolButton;

// Find, and find and replace, in the markdown editor: a bar under it with
// match case / whole word / regular expression options, a match count, and
// every match highlighted.
class FindBar : public QWidget
{
    Q_OBJECT
    public:
    explicit FindBar(CodeEditor *editor, QWidget *parent = nullptr);

    void openFind();
    void openReplace();
    void findNext();
    void findPrevious();
    void closeBar();

    // The search for `text` with the bar's options, shared with the
    // preview's find bar.
    static QRegularExpression makePattern(QString text, bool matchCase,
                                          bool wholeWords, bool regex);
    // An option button: "Aa", "W" or ".*".
    static QToolButton *toggleButton(const QString &text, const QString &tip);

    protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

    private:
    struct Match
    {
        int start = 0;
        int length = 0;
    };

    void open(bool replace);
    QRegularExpression pattern() const;
    QList<Match> matches() const;
    void find(bool forward, int from, bool inclusive);
    void select(const Match &match);
    void refresh();
    void replaceOne();
    void replaceAll();
    QString replacementFor(const QString &matched) const;

    CodeEditor *editor_;
    QLineEdit *findEdit_;
    QLineEdit *replaceEdit_;
    QToolButton *caseButton_;
    QToolButton *wordButton_;
    QToolButton *regexButton_;
    QLabel *status_;
    QWidget *replaceRow_;
    QTimer refreshTimer_;
    int origin_ = 0;   // where typing in the find field searches from
    Match current_{-1, 0};
};
