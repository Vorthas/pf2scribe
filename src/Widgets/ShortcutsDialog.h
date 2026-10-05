// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include <QDialog>
#include <QKeySequence>
#include <QList>
#include <QString>

class QAction;
class QKeySequenceEdit;

// A menu action whose shortcuts can be changed, with its defaults.
struct ShortcutEntry
{
    QString id; // QSettings key under "shortcuts/"
    QAction *action = nullptr;
    QList<QKeySequence> defaults;
};

// Loads saved shortcuts onto the actions (defaults where none were saved).
void applySavedShortcuts(const QList<ShortcutEntry> &entries);

// Settings > Keyboard Shortcuts: a primary and an alternate shortcut for
// every action. Saved in the application's settings on OK.
class ShortcutsDialog : public QDialog
{
    Q_OBJECT
    public:
    explicit ShortcutsDialog(const QList<ShortcutEntry> &entries,
                             QWidget *parent = nullptr);

    void accept() override;

    private:
    struct Row
    {
        ShortcutEntry entry;
        QKeySequenceEdit *primary = nullptr;
        QKeySequenceEdit *alternate = nullptr;
    };
    void setRow(Row &row, const QList<QKeySequence> &keys);

    QList<Row> rows_;
};
