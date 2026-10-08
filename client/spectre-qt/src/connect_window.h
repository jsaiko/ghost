// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "known_hosts.h"
#include "settings_dialog.h"

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <memory>
#include <vector>

class QProcess;

namespace Ui {
class ConnectWindow;
}

namespace gdp {
class LobbyClient;
struct DeviceInfo;
struct SessionListInfo;
} // namespace gdp

// One entry of the host dropdown's history (connect_window.cpp's
// load_recent_sessions()). Selecting it restores the username too.
struct RecentSession {
	QString host; // as typed, port suffix included
	QString username;
};

class ConnectWindow : public QWidget {
	Q_OBJECT

public:
	explicit ConnectWindow(bool debug = false, QWidget *parent = nullptr);
	~ConnectWindow() override;

private slots:
	// Named handle_*, not on_*: they're wired explicitly via connect() in
	// the constructor, and on_<widget>_<signal> would make Qt's
	// connectSlotsByName() (run by ui->setupUi()) go looking for a widget
	// literally named "settings" -- none exists (the button is
	// "settingsButton") -- so it'd just log a spurious "No matching signal"
	// warning.
	void handle_recent_session_activated(int index);
	void handle_settings_clicked();
	void handle_spectre_finished();
	// Drains whatever spectre has written since the last call into
	// spectre_output_ (and, with -D, straight on to our own stderr).
	void read_spectre_output();

private:
	// Connect: the button and Enter in any form field.
	void start_login();
	void launch_spectre(const QString &host, uint16_t port, const QString &token, const QString &cert_sha256);
	// The trust-on-first-use prompt (gdp-spec.md §2.3) for a login server
	// whose certificate `cert_sha256` isn't the one pinned for
	// login_host_/login_port_: a first-use question, or a loud warning when
	// a different certificate is already pinned. Returns true once the user
	// has chosen to trust it and it's recorded.
	bool confirm_host_certificate(const QString &cert_sha256);
	// Moves the form's current host/username to the front of
	// recent_sessions_ and persists it.
	void record_session();
	void refresh_host_combo();
	// The "spectre closed unexpectedly" dialog, with `output` behind its
	// Show Details button -- spectre reports why it gave up on stderr
	// (a missing Vulkan device, a rejected session), which is otherwise
	// invisible to someone who launched from a desktop icon.
	void report_abnormal_exit(int exit_code, bool crashed, const QString &output);
	void set_busy(bool busy, const QString &status = QString());
	// Answers a SessionList (gdp-spec.md §4.6): resumes a running
	// session, starts the type chosen with the host (a Veil login), or, when
	// there is a real choice and none was made yet (a direct login), asks in
	// the host picker showing just this host.
	void choose_session(const gdp::SessionListInfo &list);
	// Answers Veil's DeviceList (gdp-spec.md §5): asks which host and which
	// desktop on it, unless there's nothing to choose.
	void choose_device(const std::vector<gdp::DeviceInfo> &devices);
	// Asks the user for a lobby auth prompt past the first one (a second
	// factor) and sends the answer.
	void answer_auth_prompt(const QString &prompt, bool echo);
	// Tears down login_client_/login_notifier_. Only ever called once
	// control has returned to the Qt event loop after the callback that
	// decided to end the login (queued via QTimer::singleShot(0, ...) at
	// the call site) -- never directly from inside on_auth_prompt/
	// on_redirect/on_error themselves, since those callbacks are member
	// std::functions owned by the very LobbyClient this destroys; freeing
	// it while one of its own callbacks is still on the call stack would
	// free that callback's captured state out from under itself.
	void reset_login();

	Ui::ConnectWindow *ui;

	// What the host picker chose for this login: the desktop (empty: the
	// host's default), the Veil device, and the host's name, for the
	// direct-login picker's tile when the host didn't list its desktops.
	QString chosen_type_;
	QString chosen_device_id_;
	QString chosen_device_name_;

	// Owns the lobby-phase connection (gdp-spec.md §4) for as long as
	// a login is in flight; reset once it ends (success or failure) so a
	// later login starts clean. login_notifier_ has `this` as its QObject
	// parent, but that alone doesn't guarantee it's destroyed before
	// login_client_ (a plain unique_ptr member) -- reset_login() always
	// deletes it first explicitly, since it wraps a fd that becomes
	// invalid the moment login_client_ itself is gone.
	//
	// Declared as the QObject base rather than QSocketNotifier: it's a
	// QSocketNotifier on notify_fd() where that's meaningful, but a QTimer
	// polling dispatch() on Windows, where notify_fd() is always -1 (see
	// start_login()'s platform split in the .cpp) -- `delete` through the
	// base still runs the right derived destructor (QObject's is virtual).
	std::unique_ptr<gdp::LobbyClient> login_client_;
	QObject *login_notifier_ = nullptr;
	// Set on the *first* AuthChallenge only: the common case is a single
	// "Password:" PAM prompt, already answered by the form's own password
	// field, so that one doesn't need a second, redundant dialog. Any
	// further prompt (2FA, a second factor, etc.) does pop one.
	bool first_prompt_answered_ = false;
	// choose_session()'s or answer_auth_prompt()'s dialog while it's open.
	// Its exec() keeps dispatching lobby events, so an on_error (the server
	// timing out the choice) can end the login underneath it -- the error
	// path closes it.
	QPointer<QDialog> prompt_dialog_;

	// Pinned login-server certificates (known_hosts.h).
	KnownHosts known_hosts_;
	// The host/port the in-flight login dialed, which is what a pin is
	// keyed on.
	QString login_host_;
	uint16_t login_port_ = 0;
	// Set when on_certificate refused an unpinned or changed certificate:
	// the login then fails, and its on_error asks about this fingerprint
	// (confirm_host_certificate()) instead of reporting an error, and logs
	// in again if the user trusts it. Asking from inside on_certificate
	// itself isn't possible -- it must answer synchronously, from inside
	// the LobbyClient's own dispatch().
	QString rejected_cert_;

	// The currently-running spectre stream client, if any. Owned so its
	// finished signal can re-show this window once the user closes spectre;
	// `this` is its QObject parent, so it's also cleaned up if ConnectWindow
	// itself is destroyed first (unlikely, since spectre running is what
	// hides it).
	QProcess *spectre_process_ = nullptr;
	// spectre's merged stdout/stderr for the run in spectre_process_, kept
	// so an abnormal exit can show it. Trimmed from the front past a cap:
	// a long session logs steadily and only the tail explains an exit.
	QString spectre_output_;

	// Loaded from QSettings at construction, rewritten whenever the settings
	// dialog is accepted, and turned into flags on every launch_spectre().
	SpectreSettings settings_;

	// Newest first; mirrors hostCombo's items index for index.
	QList<RecentSession> recent_sessions_;
	// The user agreed to take over their session from another client: the
	// next spectre launch carries -T (spectre's kExitAlreadyConnected).
	bool take_over_next_ = false;
	// The Veil device of the login spectre came back from with
	// kExitAlreadyConnected: the take-over's login picks it again from the
	// DeviceList instead of asking (choose_device()).
	QString take_over_device_;

	// Set from spectre-qt's own -D/--debug flag (main.cpp): prints the
	// spectre command line on every launch_spectre(), mirrors spectre's
	// output to our stderr live, and shows the settings dialog's Debug
	// group.
	bool debug_ = false;
};
