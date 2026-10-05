FONTS
=====

Put .ttf or .otf font files in this folder.

How fonts are found
-------------------
- Fonts here are loaded when the app starts, as long as this folder is next
  to the executable or in the current directory. Fonts installed on your
  system (~/.fonts, ~/.local/share/fonts, ...) work too.
- Fonts in free/ are also built into the executable when you run cmake.
  To build in every font here, commercial ones too, configure with
  -DSCRIBE_EMBED_ALL_FONTS=ON (e.g. ./build.sh -DSCRIBE_EMBED_ALL_FONTS=ON).
  Such a build is for your own use only: don't share it.
- Fonts in Unused/ are ignored. Move one back up to use it again.

Which fonts are used
--------------------
From Adobe Fonts (https://fonts.adobe.com, with a Creative Cloud subscription):
  - Sabon                  body text
  - FF Good Pro            boxes, stat blocks, tables
  - FF Good Pro Condensed  stat block titles, headings
  - Gin                    smaller headings

Free downloads:
  - Open Sans              sticky notes       https://fonts.google.com/specimen/Open+Sans
  - Audiowide              Starfinder theme headings
                                              https://fonts.google.com/specimen/Audiowide
  - Pathfinder-Icons       action icons       part of the free Pathfinder Infinite templates:
      https://www.drivethrurpg.com/en/product/371033/pathfinder-infinite-creator-resource-adventure-templates

Free for personal use only (can't be shared):
  - Sofachrome             Starfinder theme title and main headings
      A Typodermic font: https://typodermicfonts.com

From the website:
  - Taroca                 page title, main headings
      Save a page from https://scribe.pf2.tools as HTML, then run
      tools/scripts/extract_embedded_fonts.py on it.

Comes with your system:
  - Times New Roman        watermark, page numbers
      Included with Windows and macOS; on Arch, the ttf-ms-fonts package (AUR).

The Starfinder 2e theme (Themes menu) uses FF Good Pro for body text too,
Audiowide instead of Gin and Sofachrome instead of Taroca. Without them it
falls back to the fonts the other themes use.

Included in free/, under the SIL Open Font License (each with its license
file): EB Garamond, Open Sans, Audiowide.

Free fallbacks, used when the fonts above are missing:
  - EB Garamond (for Sabon, Gin, Taroca)           https://fonts.google.com/specimen/EB+Garamond
  - Fira Sans (for FF Good Pro)                    https://fonts.google.com/specimen/Fira+Sans
  - Fira Sans Extra Condensed (for Good Pro Cond.) https://fonts.google.com/specimen/Fira+Sans+Extra+Condensed
  - Liberation Serif (for Times New Roman)         included with most Linux distributions

If a font is missing, the app uses the next one on its list in
src/Rendering/Theme.h that is installed. Pages can then look a little
different from the website's. Without Pathfinder-Icons, action icons show
as their text (:a:, :aa:, :r:, ...).

Licensing
---------
Commercial fonts (Sabon, FF Good Pro, Gin, Taroca, ...) can't be shared:
keep them out of any repository or release. Free fonts under the SIL Open
Font License (EB Garamond, Open Sans, Fira Sans, ...) can be, together with
their license file.

For developers
--------------
In a Debug build, View > Icon Font Glyphs shows which character each icon
is on; edit actionGlyphs in src/Rendering/Theme.h if they don't match.
