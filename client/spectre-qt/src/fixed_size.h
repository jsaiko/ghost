// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QWidget>

#include <algorithm>

// Pins `window` to its layout's natural height, at the width it already has
// (the .ui's geometry) or its layout's minimum if that's wider, and takes
// the maximize button away -- a fixed size alone leaves that up to the
// window manager. Only call before the window is first shown:
// setWindowFlag() on a visible window hides it.
inline void make_window_fixed_size(QWidget *window) {
	window->setWindowFlag(Qt::WindowMaximizeButtonHint, false);
	int width = std::max(window->width(), window->minimumSizeHint().width());
	int height = window->hasHeightForWidth() ? window->heightForWidth(width) : window->sizeHint().height();
	window->setFixedSize(width, std::max(height, window->minimumSizeHint().height()));
}
