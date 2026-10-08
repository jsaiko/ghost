// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The kiosk login screen: one fullscreen window whose pages replace
// spectre-qt's dialogs (sign-in, host/desktop choice, extra PAM prompts),
// since there is no desktop to float a dialog over and nobody should be
// able to dismiss it.
#pragma once

#include "boot_config.h"

#include <QWidget>

#include <memory>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QProcess;
class QPushButton;
class QStackedWidget;
class QSocketNotifier;

namespace gdp {
class LobbyClient;
struct DeviceInfo;
struct SessionListInfo;
} // namespace gdp

class GreeterWindow : public QWidget {
	Q_OBJECT

public:
	// `run_dir` holds the files shared with wisp-agent (agent_files.h).
	GreeterWindow(const BootConfig &config, const QString &run_dir, QWidget *parent = nullptr);
	~GreeterWindow() override;

private:
	enum class ChoiceMode { kDevice, kType };

	QWidget *build_login_page();
	QWidget *build_choice_page();
	QWidget *build_prompt_page();
	QWidget *build_take_over_page();
	QWidget *build_fatal_page();
	QWidget *build_footer();

	void start_login();
	// Destroys the LobbyClient. Never from inside one of its callbacks: those
	// are always deferred to the next event-loop turn first.
	void reset_login();
	// Back to the sign-in page, showing `error` (if any) under the form.
	void back_to_login(const QString &error = QString());
	void set_status(const QString &status);

	void choose_device(const std::vector<gdp::DeviceInfo> &devices);
	void choose_session(const gdp::SessionListInfo &list);
	void accept_choice(QListWidgetItem *item);
	// Fills the Session box for the selected host tile, as spectre-qt's
	// host picker does.
	void update_session_box();
	void set_tiles(bool tiles);
	void ask_prompt(const QString &prompt, bool echo);
	void answer_prompt();
	// The running session is open on another client: asks whether to take
	// it over, and resumes it with -T or ends the login.
	void ask_take_over(const std::string &session_type);
	void take_over();

	void launch_spectre(const QString &host, uint16_t port, const QString &token, const QString &cert_sha256);
	void handle_spectre_finished(int exit_code, bool crashed);

	void power_action(QPushButton *button, const QString &verb);

	BootConfig config_;
	QString run_dir_;

	QStackedWidget *pages_ = nullptr;
	QWidget *login_page_ = nullptr;
	QWidget *choice_page_ = nullptr;
	QWidget *prompt_page_ = nullptr;
	QWidget *take_over_page_ = nullptr;

	QLineEdit *username_edit_ = nullptr;
	QLineEdit *password_edit_ = nullptr;
	QPushButton *sign_in_button_ = nullptr;
	QLabel *status_label_ = nullptr;
	QLabel *error_label_ = nullptr;

	QLabel *choice_title_ = nullptr;
	QListWidget *choice_list_ = nullptr;
	ChoiceMode choice_mode_ = ChoiceMode::kDevice;
	QWidget *session_row_ = nullptr;
	QComboBox *session_combo_ = nullptr;
	std::vector<gdp::DeviceInfo> devices_; // the tiles' hosts, by kDeviceIndexRole

	QLabel *prompt_label_ = nullptr;
	QLineEdit *prompt_edit_ = nullptr;

	QPushButton *reboot_button_ = nullptr;
	QPushButton *poweroff_button_ = nullptr;

	std::unique_ptr<gdp::LobbyClient> login_client_;
	QSocketNotifier *login_notifier_ = nullptr;
	bool first_prompt_answered_ = false;
	bool cert_mismatch_ = false;
	QString chosen_type_; // the desktop picked together with the host, if any
	QString chosen_host_; // the host's name, for session.json
	// The person agreed to take their session over (ask_take_over()): the
	// launch carries -T. Never outlives that one launch.
	bool take_over_next_ = false;
	std::string take_over_type_; // the running session's type, for SessionOpen

	QProcess *spectre_process_ = nullptr;
	QString spectre_last_error_; // its last "spectre: ..." line
};
