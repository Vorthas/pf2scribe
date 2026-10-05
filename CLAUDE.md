# Scribe for Pathfinder 2e (pf2scribe)

A native Qt6 / C++20 desktop version of https://scribe.pf2.tools/: write
Scribe markdown on the left, see Paizo-style pages on the right, export PDF.
Binary/CMake target `pf2scribe`, display name "Scribe for Pathfinder 2e".
Nothing may imply it's official or from Paizo or pf2.tools. GPL-3.0-or-later,
author handle "Vorthas"; every source file starts with:

    // SPDX-License-Identifier: GPL-3.0-or-later
    // Copyright (C) 2026 Vorthas

## Build and layout

- `./build.sh` (Release build in `build/`; extra arguments go to cmake).
  The user's `build/` is configured with `-DSCRIBE_EMBED_ALL_FONTS=ON`
  (embeds commercial fonts: that binary is never shared). The default embeds
  only `fonts/free/`. Re-run cmake after adding or removing fonts.
- `src/`: `main.cpp`, `MainWindow` at the root; `Rendering/` (ScribeParser,
  PageLayout, Theme, TraitBadge, Fonts, ImageCache, PdfOutline), `Editor/`
  (CodeEditor, FindBar, ScribeHighlighter), `Widgets/` (PreviewWidget,
  PreviewFindBar, FileTree, TocPanel, ShortcutsDialog, IconBrowser),
  `VimEngine/`. Includes are relative to `src/` ("Rendering/PageLayout.h").
- `fonts/`: commercial fonts stay local (ignored by git); `fonts/free/`
  holds free fonts, each family with its own OFL license file;
  `fonts/Unused/` is neither loaded nor embedded. See `fonts/README.txt`.
- `samples/`: only `showcase.md` and `showcase-drake.png` are published;
  `bigTest.md` (the user's Zelda bestiary, ~10k lines, 234 images in
  `samples/Resource/`) is the main real-world test document. Never edit the
  user's markdown files.
- `tools/`: `Todo.txt` (the user's list; they rewrite it between rounds, so
  re-read it each session; dated short versions like `2026-10-05-Todo.txt`),
  `Optimizations.txt` (performance plan and dated MEASUREMENT LOG; add an
  entry when re-measuring), `scripts/` (extract_embedded_fonts.py,
  resize_image.sh, make_transparent.py). Todo files and Optimizations.txt
  are git-ignored.
- Next project: `../PF2eMonsterBuilderNative/` (monster builder; its own
  CLAUDE.md and plan). It copies code from here; don't change pf2scribe for
  it unless the user asks.

## How the user works

- One feature at a time, each tested before the next. They often want a
  plan and clarifying questions before any code ("read through and ask me
  for clarification"); notes for them go into the Todo file where asked.
- The user writes README.md themselves: suggest lines, never edit it.
- Keep the app lightweight: some extra memory for responsiveness is fine,
  gigabytes are not. Quote measured numbers for trade-offs.
- Goal: render very close to scribe.pf2.tools ("very similar, not 100%").
  Native-only extras are deliberate (text wrapping around sidebars and image
  outlines, boxes split across pages, themes, compact spacing, contents
  panel, PDF bookmarks, find in pages).
- Never push without the user saying so. No surprise deletions (no `rm` on
  their files or renders; write test output to fresh folders).
- Tests never touch the user's settings: `XDG_CONFIG_HOME` pointing at a
  scratch folder, organization name "PF2eScribeNativeTest".
- Don't change the Options/Settings menus (or other menus) beyond what was
  asked.
- Files from pf2.tools (its CSS, example.md, exports) have no published
  license: all rights reserved, never in the repository. Reading the site to
  match its rendering is fine.

## Technical lessons (read before touching these areas)

- Threads and fonts: layouts are built on one long-lived worker thread
  (`builders_`, max 1 thread, expiry -1) while the preview shows the last
  one; generation numbers, cancel flags, swap after a quiet pause. Qt font
  engines/FreeType are per thread, so anything that reads or paints a
  background-built layout takes `textEngineMutex()` (a fair lock in
  PageLayout), the build thread must outlive every layout, and PDF export
  builds on the GUI thread. Breaking this caused FreeType segfaults.
- Lazy layout: Qt lays out ~1000 characters at once and the rest on a timer
  that never runs here. Any edit after setTextWidth, or geometry read, needs
  `frameBoundingRect(rootFrame)` first, or long stat blocks lose their end.
  Theme::finishLayout's order is load-bearing (moving the tab-stop edit
  earlier changed 213 pages to 209 on 2026-10-04).
- Fonts: FF Good Pro's files claim regular weight for every style and its
  italics lack the italic flag (Qt then slants them twice). Fonts.cpp's
  `hideBrokenInstalledFonts()` (fontconfig, before QApplication) and
  `fixStyleMetadata()` repair both, for fonts/ and installed fonts.
- Images: ImageCache decodes and tints each file once, shared by all
  layouts, decodes in parallel, trims idle images to 128 MB. Opaque images
  wrap as rectangles; transparent PNGs follow their outline.
- Trait rows: end caps are part of the first/last badge; rows slightly too
  long are squeezed (block user data, set in reflow/finishLayout).
- ThreadSanitizer reports through Qt's thread pool/mutexes are false
  positives (Qt isn't instrumented); prefer std::mutex in new code.

## Testing approach

- Offscreen harnesses (`QT_QPA_PLATFORM=offscreen`) linking `src/` (minus
  main.cpp) in a scratch folder; build each in a fresh build directory if
  automoc goes stale. Run from the project root so `./fonts` loads, and
  link Fontconfig with SCRIBE_HAVE_FONTCONFIG like the app, or layouts
  differ.
- Rendering changes: hash every page of bigTest.md (all three themes, with
  and without compact spacing), showcase.md, example.md and test.md before
  and after; freeze a copy of bigTest.md first, since the user edits it.
  Text changes: also check all text is present (pdftotext).
- Performance: median of repeat builds, time until pages show, VmRSS/VmHWM;
  log in tools/Optimizations.txt.
