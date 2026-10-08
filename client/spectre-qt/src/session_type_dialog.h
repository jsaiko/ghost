// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "gdp/lobby_client.hpp"

#include <QDialog>

#include <vector>

namespace Ui {
class SessionTypeDialog;
}

// The host and session picker. For a Veil login (gdp-spec.md §5) it lists
// the hosts this user may log in to as tiles, those with a running session
// first; offline hosts can't be chosen. For a direct login it shows the one
// host. The "Session" box at the bottom left follows the selected host:
// that host's desktops with its default chosen, or, where the user's
// session is already running, just that session (it is resumed, whatever
// the type).
class SessionTypeDialog : public QDialog {
	Q_OBJECT

public:
	enum class Mode {
		Tiles,      // the hosts as a grid of tiles
		SingleHost, // a direct login: the one host's icon and name, centred
	};

	explicit SessionTypeDialog(const std::vector<gdp::DeviceInfo> &devices, QWidget *parent = nullptr,
		Mode mode = Mode::Tiles);
	~SessionTypeDialog() override;

	QString selected_device_id() const;
	// The desktop to start, or empty for the host's own default (a running
	// session, or a host that didn't say what it offers).
	QString selected_type_id() const;

private:
	// Fills the session box for the selected host.
	void update_session_box();

	Ui::SessionTypeDialog *ui;
	std::vector<gdp::DeviceInfo> devices_;
};
