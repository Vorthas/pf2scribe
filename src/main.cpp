// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "MainWindow.h"
#include "Rendering/Fonts.h"

#include <QApplication>
#include <QSettings>

namespace
{
    // Settings saved under the program's earlier name are carried over once,
    // the first time it runs as pf2scribe. The old file is left in place.
    void migrateSettings()
    {
        QSettings settings;
        if (!settings.allKeys().isEmpty())
        {
            return;
        }
        const QSettings old(QStringLiteral("PF2eScribeNative"),
                            QStringLiteral("Pathfinder 2e Scribe (Native)"));
        for (const QString &key : old.allKeys())
        {
            settings.setValue(key, old.value(key));
        }
    }
} // namespace

int main(int argc, char *argv[])
{
    // Before the application: Qt reads the installed fonts when it starts.
    scribe::hideBrokenInstalledFonts();
    QApplication app(argc, argv);
    // Where QSettings keeps options (~/.config/pf2scribe/pf2scribe.conf on
    // Linux), and the name shown in window titles.
    QApplication::setOrganizationName(QStringLiteral("pf2scribe"));
    QApplication::setApplicationName(QStringLiteral("pf2scribe"));
    QApplication::setApplicationDisplayName(
        QStringLiteral("Scribe for Pathfinder 2e"));
    migrateSettings();

    scribe::loadFonts();

    MainWindow window;
    if (argc > 1)
    {
        window.openFile(QString::fromLocal8Bit(argv[1]));
    }
    window.show();

    return app.exec();
}
