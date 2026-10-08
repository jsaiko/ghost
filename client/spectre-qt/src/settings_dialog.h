// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "spectre_settings.h"

#include <QDialog>

namespace Ui {
class SettingsDialog;
}

// spectre-qt keeps its settings in QSettings (so main.cpp sets an
// organization/application name).
SpectreSettings load_settings();
void save_settings(const SpectreSettings &settings);

class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	// `debug` shows or hides the Debug group (tile-outline toggle etc.) --
	// it mirrors spectre-qt's own -D/--debug flag (main.cpp) so those
	// options aren't in front of everyone by default.
	explicit SettingsDialog(const SpectreSettings &settings, bool debug, QWidget *parent = nullptr);
	~SettingsDialog() override;

	// The dialog's current state. Only meaningful after exec() returned
	// QDialog::Accepted.
	SpectreSettings settings() const;

private:
	Ui::SettingsDialog *ui;
};
