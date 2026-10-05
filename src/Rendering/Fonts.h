// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include <QByteArray>

namespace scribe
{
    // Registers every .ttf/.otf found in:
    //   :/fonts            fonts baked into the binary (see CMakeLists.txt)
    //   <app dir>/fonts    next to the executable
    //   <cwd>/fonts        handy when running from the project root
    // with their style metadata fixed (see fixStyleMetadata()), and fixed
    // copies of the installed fonts hideBrokenInstalledFonts() set aside.
    void loadFonts();

    // Installed fonts of the families Theme uses can have the same broken
    // style metadata (FF Good Pro installed in ~/.fonts, say). Qt would then
    // pick any of their styles, Ultra for Regular. Call before creating the
    // application: this program's fontconfig configuration leaves those
    // files out, and loadFonts() registers fixed copies in their place.
    // Does nothing without fontconfig.
    void hideBrokenInstalledFonts();

    // Some font families ship every style with the same OS/2 weight (400) and
    // no italic flag, e.g. FF Good Pro, where Black, Ultra, Bold and Regular
    // all claim to be "Regular". Qt then picks any of them for "FF Good Pro".
    // This derives weight and slant from the style name ("Bold", "Black
    // Italic", ...) and writes them into a copy of the font data. Fonts whose
    // metadata already agrees with their style name are returned unchanged.
    QByteArray fixStyleMetadata(const QByteArray &fontData);
} // namespace scribe
