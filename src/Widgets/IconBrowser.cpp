// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Widgets/IconBrowser.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace
{
    // Code point ranges worth scanning: ASCII, Latin-1 and the private use
    // area, which is where icon fonts usually put their glyphs.
    struct Range
    {
        uint first;
        uint last;
    };

    constexpr Range kRanges[] = {{0x21, 0x7E}, {0xA1, 0x24F}, {0xE000, 0xF8FF}};

    constexpr int kColumns = 8;
} // namespace

IconBrowser::IconBrowser(const scribe::Theme &theme, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Icon font glyphs"));
    resize(640, 520);

    auto *browser = new QTextBrowser;
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(browser);

    QString family;
    for (const QString &candidate : theme.iconFamilies)
    {
        if (QFontDatabase::hasFamily(candidate))
        {
            family = candidate;
            break;
        }
    }

    if (family.isEmpty())
    {
        browser->setPlainText(
            tr("None of these font families is installed: %1\n\n"
               "Put the icon font in fonts/ and check the log for the "
               "family name it reports on startup.")
                .arg(theme.iconFamilies.join(QStringLiteral(", "))));
        return;
    }

    QFont font(family);
    font.setPointSizeF(24);
    font.setStyleStrategy(QFont::NoFontMerging);
    const QFontMetricsF metrics(font);

    QString html = QStringLiteral("<p>Family: <b>%1</b></p>"
                                  "<table cellpadding=\"6\"><tr>")
                       .arg(family.toHtmlEscaped());

    int count = 0;
    for (const Range &range : kRanges)
    {
        for (uint cp = range.first; cp <= range.last; ++cp)
        {
            if (!metrics.inFontUcs4(cp))
            {
                continue;
            }

            const QString label =
                cp < 0x7F ? QString(QChar(cp)).toHtmlEscaped() : QString();
            html += QStringLiteral(
                        "<td align=\"center\">"
                        "<span style=\"font-family:'%1'; font-size:24pt;\">"
                        "&#%2;</span><br/><small>U+%3<br/>%4</small></td>")
                        .arg(family, QString::number(cp),
                             QString::number(cp, 16).toUpper(), label);

            if (++count % kColumns == 0)
            {
                html += QStringLiteral("</tr><tr>");
            }
        }
    }
    html += QStringLiteral("</tr></table>");

    if (count == 0)
    {
        html = tr("<p>The font %1 loaded, but no glyphs were found in the "
                  "ranges scanned.</p>")
                   .arg(family.toHtmlEscaped());
    }
    browser->setHtml(html);
}
