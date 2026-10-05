// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Widgets/ShortcutsDialog.h"

#include <QAction>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

namespace
{
    QString settingsKey(const QString &id)
    {
        return QStringLiteral("shortcuts/") + id;
    }

    QList<QKeySequence> savedOrDefault(const ShortcutEntry &entry)
    {
        const QSettings settings;
        if (!settings.contains(settingsKey(entry.id)))
        {
            return entry.defaults;
        }
        QList<QKeySequence> keys;
        for (const QString &text : settings.value(settingsKey(entry.id)).toStringList())
        {
            const QKeySequence key =
                QKeySequence::fromString(text, QKeySequence::PortableText);
            if (!key.isEmpty())
            {
                keys << key;
            }
        }
        return keys;
    }

    // "&Find..." -> "Find"
    QString plainName(const QAction *action)
    {
        QString text = action->text();
        text.remove(QLatin1Char('&'));
        if (text.endsWith(QLatin1String("...")))
        {
            text.chop(3);
        }
        return text;
    }
} // namespace

void applySavedShortcuts(const QList<ShortcutEntry> &entries)
{
    for (const ShortcutEntry &entry : entries)
    {
        entry.action->setShortcuts(savedOrDefault(entry));
    }
}

ShortcutsDialog::ShortcutsDialog(const QList<ShortcutEntry> &entries,
                                 QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Keyboard Shortcuts"));

    auto *table = new QTableWidget(static_cast<int>(entries.size()), 3);
    table->setHorizontalHeaderLabels({tr("Action"), tr("Shortcut"), tr("Alternate")});
    table->verticalHeader()->hide();
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);

    for (int i = 0; i < entries.size(); ++i)
    {
        Row row;
        row.entry = entries.at(i);
        row.primary = new QKeySequenceEdit;
        row.alternate = new QKeySequenceEdit;
        for (QKeySequenceEdit *edit : {row.primary, row.alternate})
        {
            edit->setMaximumSequenceLength(1);
            edit->setClearButtonEnabled(true);
        }
        setRow(row, savedOrDefault(row.entry));
        table->setItem(i, 0, new QTableWidgetItem(plainName(row.entry.action)));
        table->setCellWidget(i, 1, row.primary);
        table->setCellWidget(i, 2, row.alternate);
        rows_ << row;
    }
    table->resizeRowsToContents();

    auto *note = new QLabel(
        tr("Click a field and press the keys. In Vim mode the editor keeps the "
           "keys Vim uses (Ctrl+R, Ctrl+F, ...) while it has focus."));
    note->setWordWrap(true);

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                             QDialogButtonBox::RestoreDefaults);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults),
            &QPushButton::clicked, this, [this] {
                for (Row &row : rows_)
                {
                    setRow(row, row.entry.defaults);
                }
            });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(table);
    layout->addWidget(note);
    layout->addWidget(buttons);
    resize(560, 440);
}

void ShortcutsDialog::setRow(Row &row, const QList<QKeySequence> &keys)
{
    row.primary->setKeySequence(keys.value(0));
    row.alternate->setKeySequence(keys.value(1));
}

void ShortcutsDialog::accept()
{
    // No key may do two things.
    QHash<QString, QString> owners;
    QStringList conflicts;
    for (const Row &row : rows_)
    {
        for (const QKeySequenceEdit *edit : {row.primary, row.alternate})
        {
            const QString key = edit->keySequence().toString(QKeySequence::NativeText);
            if (key.isEmpty())
            {
                continue;
            }
            const QString name = plainName(row.entry.action);
            if (owners.contains(key) && owners.value(key) != name)
            {
                conflicts << tr("%1: %2 and %3").arg(key, owners.value(key), name);
            }
            owners.insert(key, name);
        }
    }
    if (!conflicts.isEmpty())
    {
        QMessageBox::warning(this, windowTitle(),
                             tr("These shortcuts are used twice:\n\n%1")
                                 .arg(conflicts.join(QLatin1Char('\n'))));
        return;
    }

    QSettings settings;
    for (const Row &row : rows_)
    {
        QList<QKeySequence> keys;
        for (const QKeySequenceEdit *edit : {row.primary, row.alternate})
        {
            if (!edit->keySequence().isEmpty())
            {
                keys << edit->keySequence();
            }
        }
        row.entry.action->setShortcuts(keys);
        if (keys == row.entry.defaults)
        {
            settings.remove(settingsKey(row.entry.id));
        }
        else
        {
            QStringList texts;
            for (const QKeySequence &key : keys)
            {
                texts << key.toString(QKeySequence::PortableText);
            }
            // An empty list is kept too: the user removed the shortcut.
            settings.setValue(settingsKey(row.entry.id), texts);
        }
    }
    QDialog::accept();
}
