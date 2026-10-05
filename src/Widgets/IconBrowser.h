// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include "Rendering/Theme.h"

#include <QDialog>

// Lists every glyph in the icon font, so the token -> glyph table in Theme.h
// can be checked (or corrected) by eye.
class IconBrowser : public QDialog
{
    public:
    explicit IconBrowser(const scribe::Theme &theme, QWidget *parent = nullptr);
};
