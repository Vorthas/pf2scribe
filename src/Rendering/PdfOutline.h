// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include <QString>

#include <vector>

namespace scribe
{
    // One bookmark: nested under the last entry before it with a lower
    // level, pointing at `y` points below the top of page `page` (from 0;
    // -1 if unknown, in which case it takes the next known entry's place).
    struct OutlineEntry
    {
        QString title;
        int level = 0;
        int page = -1;
        double y = 0;
    };

    // Adds `entries` as the bookmarks (outline) of the PDF QPdfWriter wrote
    // at `path`, collapsed, and has viewers open with the bookmarks shown.
    // The file is extended with an incremental update, so everything Qt
    // wrote stays as it is. False, with `error` set, if the file isn't laid
    // out as expected; it is then left unchanged.
    bool addPdfOutline(const QString &path,
                       const std::vector<OutlineEntry> &entries,
                       QString *error = nullptr);

    // The same bookmarks as Ghostscript pdfmark lines (an "index.info"):
    // `gs -sDEVICE=pdfwrite -sOutputFile=out.pdf index.info -f in.pdf`.
    // `pageHeight` (points) turns positions into PDF coordinates.
    QString pdfmarkOutline(const std::vector<OutlineEntry> &entries,
                           double pageHeight);
} // namespace scribe
