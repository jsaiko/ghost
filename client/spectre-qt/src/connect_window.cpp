// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "connect_window.h"
#include "ui_connect_window.h"

#include "session_type_dialog.h"
#include "fixed_size.h"
#include "settings_dialog.h"

#include "gdp/cert_fingerprint.hpp"
#include "gdp/error_codes.hpp"
#include "gdp/lobby_client.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSocketNotifier>
#include <QTimer>

#include <cstdio>
#include <utility>

namespace {

// The `spectre` stream binary is a sibling build target
// (client/spectre/CMakeLists.txt's `spectre` executable). The top-level
// CMakeLists.txt sets CMAKE_RUNTIME_OUTPUT_DIRECTORY so every executable
// in the tree lands in one bin/ directory, and `cmake --install` puts them
// side by side too -- so it is always next to this launcher.
QString find_spectre_binary() {
#ifdef _WIN32
	const QString name = "spectre.exe";
#else
	const QString name = "spectre";
#endif
	QString candidate = QDir(QCoreApplication::applicationDirPath()).filePath(name);
	if (QFileInfo::exists(candidate)) {
		return candidate;
	}
	return QString();
}

constexpr uint16_t kLoginPortDefault = 4442; // ghostd's lobby port (host/ghostd/src/config.rs)

// The host dropdown's history: the last kMaxRecentSessions launches, newest
// first, one per host string. Only what the form shows is kept -- never the
// password.
constexpr auto kRecentSessionsKey = "connect/recent_sessions";
constexpr int kMaxRecentSessions = 10;

QList<RecentSession> load_recent_sessions() {
	QSettings store;
	QList<RecentSession> sessions;
	int count = store.beginReadArray(kRecentSessionsKey);
	for (int i = 0; i < count && sessions.size() < kMaxRecentSessions; ++i) {
		store.setArrayIndex(i);
		RecentSession session;
		session.host = store.value("host").toString();
		session.username = store.value("username").toString();
		if (!session.host.isEmpty()) {
			sessions << session;
		}
	}
	store.endArray();
	return sessions;
}

void save_recent_sessions(const QList<RecentSession> &sessions) {
	QSettings store;
	store.remove(kRecentSessionsKey); // beginWriteArray leaves stale higher indices behind
	store.beginWriteArray(kRecentSessionsKey, sessions.size());
	for (int i = 0; i < sessions.size(); ++i) {
		store.setArrayIndex(i);
		store.setValue("host", sessions[i].host);
		store.setValue("username", sessions[i].username);
	}
	store.endArray();
}

// Splits "host:port" into its parts, falling back to `default_port` when no
// ":port" suffix is present. Returns false (leaving *host/*port untouched) if
// a suffix is present but isn't a valid port number.
//
// An IPv6 literal is full of colons itself, so the port suffix is only
// recognized in the two forms that can't be ambiguous: bracketed
// ("[::1]:4442", brackets stripped from *host), or a name/IPv4 with a
// single colon. A bare literal ("fd00::1") is all host -- otherwise its
// last group would be read as a port number and silently accepted.
bool parse_host_and_port(const QString &input, uint16_t default_port, QString *host, uint16_t *port) {
	if (input.startsWith('[')) {
		int close = input.indexOf(']');
		if (close < 0) {
			return false; // unterminated "[..." is a typo, not a hostname
		}
		QString inside = input.mid(1, close - 1);
		QString rest = input.mid(close + 1);
		if (rest.isEmpty()) {
			*host = inside;
			*port = default_port;
			return true;
		}
		if (!rest.startsWith(':')) {
			return false; // trailing junk after "]"
		}
		bool ok = false;
		uint value = rest.mid(1).toUInt(&ok);
		if (!ok || value == 0 || value > 65535) {
			return false;
		}
		*host = inside;
		*port = (uint16_t)value;
		return true;
	}

	int colon = input.lastIndexOf(':');
	if (colon < 0 || input.indexOf(':') != colon) {
		// No colon at all, or more than one: a bare IPv6 literal.
		*host = input;
		*port = default_port;
		return true;
	}
	bool ok = false;
	uint value = input.mid(colon + 1).toUInt(&ok);
	if (!ok || value == 0 || value > 65535) {
		return false;
	}
	*host = input.left(colon);
	*port = (uint16_t)value;
	return true;
}

} // namespace

ConnectWindow::ConnectWindow(bool debug, QWidget *parent)
	: QWidget(parent), ui(new Ui::ConnectWindow), settings_(load_settings()), debug_(debug) {
	ui->setupUi(this);
	ui->hostCombo->lineEdit()->setPlaceholderText(QString("192.168.1.100:%1").arg(kLoginPortDefault));
	make_window_fixed_size(this);
	connect(ui->connectButton, &QPushButton::clicked, this, &ConnectWindow::start_login);
	connect(ui->settingsButton, &QPushButton::clicked, this, &ConnectWindow::handle_settings_clicked);
	// Enter in any of the form fields is the same as clicking Connect.
	connect(ui->hostCombo->lineEdit(), &QLineEdit::returnPressed, this, &ConnectWindow::start_login);
	connect(ui->hostCombo, &QComboBox::activated, this, &ConnectWindow::handle_recent_session_activated);
	connect(ui->usernameEdit, &QLineEdit::returnPressed, this, &ConnectWindow::start_login);
	connect(ui->passwordEdit, &QLineEdit::returnPressed, this, &ConnectWindow::start_login);

	recent_sessions_ = load_recent_sessions();
	refresh_host_combo();
	if (!recent_sessions_.isEmpty()) {
		// Start from the last session, so reconnecting is just the password.
		ui->hostCombo->setCurrentIndex(0);
		handle_recent_session_activated(0);
	}
}

ConnectWindow::~ConnectWindow() {
	reset_login();
	delete ui;
}

void ConnectWindow::handle_recent_session_activated(int index) {
	if (index < 0 || index >= recent_sessions_.size()) {
		return;
	}
	const RecentSession &session = recent_sessions_[index];
	ui->usernameEdit->setText(session.username);
	ui->passwordEdit->clear();
	// Straight to whatever is still missing.
	(session.username.isEmpty() ? ui->usernameEdit : ui->passwordEdit)->setFocus();
}

void ConnectWindow::refresh_host_combo() {
	// clear() also empties the edit text; keep whatever is typed there.
	QString text = ui->hostCombo->currentText();
	ui->hostCombo->clear();
	for (const RecentSession &session : recent_sessions_) {
		ui->hostCombo->addItem(session.host);
	}
	ui->hostCombo->setCurrentIndex(-1);
	ui->hostCombo->setEditText(text);
}

void ConnectWindow::record_session() {
	RecentSession session;
	session.host = ui->hostCombo->currentText().trimmed();
	session.username = ui->usernameEdit->text().trimmed();
	for (int i = 0; i < recent_sessions_.size(); ++i) {
		if (recent_sessions_[i].host == session.host) {
			recent_sessions_.removeAt(i);
			break;
		}
	}
	recent_sessions_.prepend(session);
	while (recent_sessions_.size() > kMaxRecentSessions) {
		recent_sessions_.removeLast();
	}
	save_recent_sessions(recent_sessions_);
	refresh_host_combo();
}

void ConnectWindow::handle_settings_clicked() {
	SettingsDialog dialog(settings_, debug_, this);
	if (dialog.exec() != QDialog::Accepted) {
		return;
	}
	settings_ = dialog.settings();
	// Written through immediately rather than at exit: spectre is launched as
	// a child process and this window may be closed while one is running.
	save_settings(settings_);
}

void ConnectWindow::start_login() {
	QString host;
	uint16_t port;
	bool host_ok =
		parse_host_and_port(ui->hostCombo->currentText().trimmed(), kLoginPortDefault, &host, &port);
	QString username = ui->usernameEdit->text().trimmed();
	if (!host_ok || host.isEmpty()) {
		QMessageBox::warning(this, "spectre", "Enter a host address.");
		return;
	}
	if (username.isEmpty()) {
		QMessageBox::warning(this, "spectre", "Enter a username.");
		return;
	}
	if (login_client_) {
		return; // a login is already in flight; the button should be disabled anyway
	}

	first_prompt_answered_ = false;
	chosen_type_.clear();
	chosen_device_name_.clear();
	chosen_device_id_.clear();
	login_host_ = host;
	login_port_ = port;
	rejected_cert_.clear();
	login_client_ = std::make_unique<gdp::LobbyClient>();
	gdp::LobbyClient *client = login_client_.get();

	client->ca_trust.system = true;
	if (QString bundle = KnownHosts::ca_bundle_path(); QFileInfo::exists(bundle)) {
		client->ca_trust.extra_file = QDir::toNativeSeparators(bundle).toStdString();
	}
	client->on_certificate = [this, client](const std::string &cert_sha256) {
		// A certificate a trusted CA issued for the name typed: no prompt,
		// and no pin either, so that its routine renewals (a new key every
		// few months) never look like a changed host (gdp-spec.md §2.3).
		if (client->certificate_ca_verified()) {
			return true;
		}
		QString fingerprint = QString::fromStdString(cert_sha256);
		if (known_hosts_.check(login_host_, login_port_, fingerprint) == KnownHosts::Status::kTrusted) {
			return true;
		}
		// Nothing has been sent yet, the password included; on_error
		// (below) asks the user about it once this login has wound down.
		rejected_cert_ = fingerprint;
		return false;
	};

	client->on_auth_prompt = [this](const std::string &prompt, bool echo) {
		if (!login_client_) {
			return;
		}
		if (!first_prompt_answered_) {
			// The common case: one "Password:"-style prompt, already
			// collected by the form itself -- answer it without bothering
			// the user with a second box for the same thing.
			first_prompt_answered_ = true;
			login_client_->respond(ui->passwordEdit->text().toStdString());
			set_busy(true, tr("Authenticating…"));
			return;
		}
		// Anything past the first prompt (a second factor, etc.) needs an
		// actual answer from the user -- a modal dialog, so deferred like
		// on_session_list below.
		QString qprompt = QString::fromStdString(prompt);
		QTimer::singleShot(0, this, [this, qprompt, echo]() {
			if (login_client_) {
				answer_auth_prompt(qprompt, echo);
			}
		});
	};

	client->on_session_list = [this](const gdp::SessionListInfo &list) {
		// Deferred to the next event-loop turn for the same reason
		// on_redirect/on_error are (see reset_login()'s comment):
		// choose_session() may run a modal dialog and may end the login,
		// neither of which is safe from inside this LobbyClient's own
		// callback.
		QTimer::singleShot(0, this, [this, list]() {
			if (login_client_) {
				choose_session(list);
			}
		});
	};

	client->on_device_list = [this](const std::vector<gdp::DeviceInfo> &devices) {
		// Deferred like on_session_list: choose_device() may run a modal
		// dialog.
		QTimer::singleShot(0, this, [this, devices]() {
			if (login_client_) {
				choose_device(devices);
			}
		});
	};

	client->on_redirect = [this](const std::string &host, uint16_t port, const std::string &token,
							  int64_t /*expiry_unix*/, const std::string &cert_sha256) {
		// Deferred to the next event-loop turn: reset_login() (called from
		// the queued lambda below) destroys the LobbyClient this callback
		// is a member of, which must never happen while this callback is
		// still on the call stack -- see reset_login()'s comment.
		QString qhost = QString::fromStdString(host);
		QString qtoken = QString::fromStdString(token);
		QString qcert = QString::fromStdString(cert_sha256);
		QTimer::singleShot(0, this, [this, qhost, port, qtoken, qcert]() {
			reset_login();
			set_busy(false);
			if (!gdp::is_sha256_hex(qcert.toStdString())) {
				// spectre refuses to connect without one; say why here
				// rather than let it fail with nothing on screen.
				QMessageBox::warning(this, "spectre",
					"The login server didn't say which certificate the session will use, so the "
					"session can't be verified. The host's ghostd is probably older than this client.");
				return;
			}
			launch_spectre(qhost, port, qtoken, qcert);
		});
	};

	client->on_error = [this](const std::string &message) {
		QString qmessage = QString::fromStdString(message);
		// Veil's own refusals (gdp-spec.md §12) get words of our own: the
		// server's text names the host, but not what to do about it.
		switch (static_cast<gdp::ErrorCode>(login_client_ ? login_client_->error_code() : 0)) {
		case gdp::ErrorCode::kHostOffline:
			qmessage =
				tr("That host isn't connected to the broker right now. Try again later, or pick another host.") +
				"\n\n" + qmessage;
			break;
		case gdp::ErrorCode::kHostAuthFailed:
			qmessage =
				tr("The host rejected your password, although the broker accepted it. Your password on "
				   "that host is probably different from your broker password; ask your administrator.");
			break;
		case gdp::ErrorCode::kNotEntitled: qmessage = tr("You aren't allowed to use that host."); break;
		default: break;
		}
		QTimer::singleShot(0, this, [this, qmessage]() {
			reset_login();
			set_busy(false);
			if (prompt_dialog_) {
				prompt_dialog_->reject();
			}
			if (!rejected_cert_.isEmpty()) {
				QString fingerprint = rejected_cert_;
				rejected_cert_.clear();
				if (confirm_host_certificate(fingerprint)) {
					start_login(); // the form still holds everything, password included
				}
				return;
			}
			QMessageBox::warning(this, "spectre", qmessage);
		});
	};

	if (!client->connect(host.toStdString(), port, username.toStdString())) {
		login_client_.reset(); // connect() itself failed synchronously; no callback will ever fire
		QMessageBox::critical(this, "spectre", "Failed to reach the login server.");
		return;
	}

#ifdef _WIN32
	// notify_fd() is always -1 on Windows -- libgdp's EventQueue has no
	// eventfd equivalent there -- and QSocketNotifier rejects an invalid
	// socket outright ("QSocketNotifier: Invalid socket specified"), so
	// dispatch() would never run and login would hang forever. Poll it on a
	// timer instead, as spectre's own stream_session.cpp does.
	auto *timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, [this]() {
		if (login_client_) {
			login_client_->dispatch();
		}
	});
	timer->start(20);
	login_notifier_ = timer;
#else
	auto *notifier = new QSocketNotifier(client->notify_fd(), QSocketNotifier::Read, this);
	connect(notifier, &QSocketNotifier::activated, this, [this]() {
		if (login_client_) {
			login_client_->dispatch();
		}
	});
	login_notifier_ = notifier;
#endif
	set_busy(true, tr("Connecting…"));
}

bool ConnectWindow::confirm_host_certificate(const QString &cert_sha256) {
	// Two lines of 16 byte pairs, monospaced, so it can be compared by eye.
	QString formatted = QString::fromStdString(gdp::format_fingerprint(cert_sha256.toStdString()));
	QString shown = "<pre>" + formatted.left(47) + "\n" + formatted.mid(48) + "</pre>";
	QString where = login_port_ == kLoginPortDefault
		? login_host_.toHtmlEscaped()
		: login_host_.toHtmlEscaped() + ":" + QString::number(login_port_);
	QString check = "On the host, <tt>openssl x509 -in /etc/ghost/host-cert.pem -noout -fingerprint "
					"-sha256</tt> prints the fingerprint to compare.";

	QMessageBox box(this);
	box.setWindowTitle("spectre");
	box.setTextFormat(Qt::RichText);
	QPushButton *trust_button;
	if (known_hosts_.check(login_host_, login_port_, cert_sha256) == KnownHosts::Status::kMismatch) {
		box.setIcon(QMessageBox::Critical);
		box.setText("<b>The certificate of " + where + " has changed.</b>");
		box.setInformativeText("This is not the certificate you trusted before. It can mean someone is "
							   "intercepting the connection to capture your password -- or that the host "
							   "was reinstalled or its certificate was replaced.<br><br>New fingerprint:" +
			shown + check + "<br><br>Only trust it if you know why it changed.");
		trust_button = box.addButton("Trust New Certificate", QMessageBox::DestructiveRole);
		box.setDefaultButton(box.addButton(QMessageBox::Cancel));
	} else {
		box.setIcon(QMessageBox::Question);
		box.setText("First connection to " + where + ".");
		box.setInformativeText(
			"spectre hasn't seen this host before. Its certificate fingerprint is:" + shown + check +
			"<br><br>Trust this host? spectre will remember it and warn you if it ever changes.");
		trust_button = box.addButton("Trust and Connect", QMessageBox::AcceptRole);
		box.addButton(QMessageBox::Cancel);
		box.setDefaultButton(trust_button);
	}
	box.exec();
	if (box.clickedButton() != trust_button) {
		return false;
	}

	QString error;
	if (!known_hosts_.trust(login_host_, login_port_, cert_sha256, &error)) {
		// Still trusted for this run; it just won't be remembered.
		QMessageBox::warning(this, "spectre",
			"Couldn't save the trusted certificate to " + KnownHosts::path() + ": " + error);
	}
	return true;
}

void ConnectWindow::reset_login() {
	// The notifier must go first: it wraps login_client_'s notify_fd(),
	// which becomes invalid the moment login_client_ itself is destroyed.
	delete login_notifier_;
	login_notifier_ = nullptr;
	login_client_.reset();
}

void ConnectWindow::choose_session(const gdp::SessionListInfo &list) {
	// One session per uid: an already-running session is resumed no matter
	// what type is requested (gdp-spec.md §4.6), so there's nothing to ask.
	if (!list.running.empty()) {
		// Open on another client: ask now, while the login can still go
		// on, rather than let spectre be refused and log in again. Not
		// when the user already agreed (the retry after a refusal).
		if (list.running.front().viewer_attached && !take_over_next_) {
			QMessageBox box(QMessageBox::Question, "spectre",
				tr("Your session is already open on another client. Take it over? That client will be "
				   "disconnected."),
				QMessageBox::Yes | QMessageBox::No, this);
			prompt_dialog_ = &box;
			int answer = box.exec();
			prompt_dialog_ = nullptr;
			// As below: the login may have ended while it was open.
			if (!login_client_) {
				return;
			}
			if (answer != QMessageBox::Yes) {
				reset_login();
				set_busy(false);
				return;
			}
			take_over_next_ = true;
		}
		login_client_->open_session(list.running.front().session_type);
		set_busy(true, tr("Resuming session…"));
		return;
	}
	// A Veil login chose the desktop together with the host. A type the host
	// no longer offers falls back to its default.
	if (!chosen_type_.isEmpty()) {
		const std::string wanted = chosen_type_.toStdString();
		bool offered = false;
		for (const auto &type : list.types) {
			offered = offered || type.id == wanted;
		}
		login_client_->open_session(offered ? wanted : std::string());
		set_busy(true, tr("Starting session…"));
		return;
	}
	if (list.types.size() <= 1) {
		login_client_->open_session(std::string());
		set_busy(true, tr("Starting session…"));
		return;
	}

	// A direct login, or a broker host that didn't list its desktops: the
	// host picker again, showing the one host with its desktops.
	gdp::DeviceInfo host;
	host.name = !chosen_device_name_.isEmpty()
		? chosen_device_name_.toStdString()
		: (login_port_ == kLoginPortDefault ? login_host_
											: QString("%1:%2").arg(login_host_).arg(login_port_))
			  .toStdString();
	host.online = true;
	host.types = list.types;
	host.default_type = list.default_type;
	SessionTypeDialog dialog({host}, this, SessionTypeDialog::Mode::SingleHost);
	prompt_dialog_ = &dialog;
	int result = dialog.exec();
	prompt_dialog_ = nullptr;
	// The dialog's nested event loop keeps dispatching lobby events, so an
	// on_error (e.g. the server timing out the choice) may have ended the
	// login while it was open.
	if (!login_client_) {
		return;
	}
	if (result != QDialog::Accepted) {
		reset_login();
		set_busy(false);
		return;
	}
	login_client_->open_session(dialog.selected_type_id().toStdString());
	set_busy(true, tr("Starting session…"));
}

void ConnectWindow::choose_device(const std::vector<gdp::DeviceInfo> &devices) {
	// Taking a session over: the user picked its host on the login before,
	// so pick it again rather than show the picker a second time.
	QString again = std::exchange(take_over_device_, QString());
	for (const auto &device : devices) {
		if (!again.isEmpty() && device.online && device.id == again.toStdString()) {
			chosen_device_id_ = again;
			chosen_device_name_ = QString::fromStdString(device.name);
			chosen_type_ = QString::fromStdString(device.session_type);
			login_client_->select_device(device.id);
			set_busy(true, tr("Connecting to %1…").arg(chosen_device_name_));
			return;
		}
	}

	// Only one host that could work, and nothing to choose on it (its
	// session is running, or it offers at most one desktop): don't ask.
	int online = 0;
	const gdp::DeviceInfo *only = nullptr;
	for (const auto &device : devices) {
		if (device.online) {
			++online;
			only = &device;
		}
	}
	if (online == 1 && devices.size() == 1 && (only->has_session || only->types.size() <= 1)) {
		chosen_device_id_ = QString::fromStdString(only->id);
		chosen_device_name_ = QString::fromStdString(only->name);
		chosen_type_ = only->types.empty() ? QString() : QString::fromStdString(only->types.front().id);
		login_client_->select_device(only->id);
		set_busy(true, tr("Connecting to %1…").arg(QString::fromStdString(only->name)));
		return;
	}

	SessionTypeDialog dialog(devices, this);
	prompt_dialog_ = &dialog;
	int result = dialog.exec();
	prompt_dialog_ = nullptr;
	// As in choose_session(): the login may have ended while it was open.
	if (!login_client_) {
		return;
	}
	if (result != QDialog::Accepted || dialog.selected_device_id().isEmpty()) {
		reset_login();
		set_busy(false);
		return;
	}
	chosen_type_ = dialog.selected_type_id();
	chosen_device_id_ = dialog.selected_device_id();
	for (const auto &device : devices) {
		if (device.id == dialog.selected_device_id().toStdString()) {
			chosen_device_name_ = QString::fromStdString(device.name);
		}
	}
	login_client_->select_device(dialog.selected_device_id().toStdString());
	set_busy(true, tr("Connecting…"));
}

void ConnectWindow::answer_auth_prompt(const QString &prompt, bool echo) {
	QInputDialog dialog(this);
	dialog.setWindowTitle("spectre");
	dialog.setLabelText(prompt);
	dialog.setTextEchoMode(echo ? QLineEdit::Normal : QLineEdit::Password);
	prompt_dialog_ = &dialog;
	dialog.exec();
	prompt_dialog_ = nullptr;
	// Same as choose_session(): the login may have ended while it was open.
	if (!login_client_) {
		return;
	}
	// Respond either way -- an empty/cancelled answer just fails whatever
	// PAM module asked, same as typing nothing would.
	login_client_->respond(dialog.textValue().toStdString());
	set_busy(true, tr("Authenticating…"));
}

void ConnectWindow::set_busy(bool busy, const QString &status) {
	ui->hostCombo->setEnabled(!busy);
	ui->usernameEdit->setEnabled(!busy);
	ui->passwordEdit->setEnabled(!busy);
	ui->connectButton->setEnabled(!busy);
	ui->settingsButton->setEnabled(!busy);
	ui->statusLabel->setText(status);
}

void ConnectWindow::launch_spectre(const QString &host, uint16_t port, const QString &token,
	const QString &cert_sha256) {
	QString spectre_path = find_spectre_binary();
	if (spectre_path.isEmpty()) {
		QMessageBox::critical(this, "spectre", "Couldn't find the spectre stream binary.");
		return;
	}
	// Reached from a login's redirect, with the form still holding what the
	// user typed (it's disabled, not cleared, while the login is in flight),
	// so that's what goes in the history.
	record_session();
	// The password has done its job (the host has it, or the session is
	// already open): don't leave it in the form for the session's whole
	// life and then show it again, filled in, when the session ends. The
	// next connect asks for it like a fresh start does.
	ui->passwordEdit->clear();
	QStringList args;
	args << "-h" << host << "-p" << QString::number(port) << "-P" << cert_sha256;
	if (take_over_next_) {
		args << "-T"; // only for this one attempt, after the user agreed
		take_over_next_ = false;
	}
	args << settings_.to_args(screen(), debug_);
	if (debug_) {
		qInfo().noquote() << spectre_path << args.join(' ');
	}
	// A member QProcess rather than startDetached(): startDetached() forgets
	// the child immediately, giving no way to notice when it exits. Tracking
	// it lets this window hide while spectre is up and reappear once it's
	// not.
	spectre_process_ = new QProcess(this);
	spectre_output_.clear();
	// spectre logs to stderr; merging the channels keeps those lines in the
	// order they were written relative to anything on stdout, so the capture
	// reads exactly like a terminal run would.
	spectre_process_->setProcessChannelMode(QProcess::MergedChannels);
	// The token goes in the environment, not the arguments: other local
	// users can read a command line, not this user's environment.
	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	if (!token.isEmpty()) {
		env.insert(QStringLiteral("SPECTRE_TOKEN"), token);
	}
	if (debug_) {
		// spectre's debug-level lines (client/spectre/src/log.hpp) as well.
		env.insert(QStringLiteral("SPECTRE_LOG"), QStringLiteral("debug"));
	}
	spectre_process_->setProcessEnvironment(env);
	connect(spectre_process_, &QProcess::readyReadStandardOutput, this, &ConnectWindow::read_spectre_output);
	connect(spectre_process_, &QProcess::finished, this, [this](int exit_code, QProcess::ExitStatus status) {
		read_spectre_output(); // whatever is still sitting in the pipe
		// handle_spectre_finished() drops the process and re-shows this
		// window, so take a copy first and report once it's back up --
		// a dialog parented to a hidden window would go unnoticed.
		QString output = spectre_output_;
		handle_spectre_finished();
		// spectre's kExitEndedByLocalLogin
		// (client/spectre/src/stream/stream_session.hpp): an ordinary end,
		// not a failure.
		constexpr int kSpectreExitEndedByLocalLogin = 3;
		if (status == QProcess::NormalExit && exit_code == kSpectreExitEndedByLocalLogin) {
			QMessageBox::information(this, "spectre",
				tr("You were signed out because you logged in at the host itself."));
			return;
		}
		// kExitAlreadyConnected and kExitTakenOver, same header: this user's
		// session is open on another client, or was just taken over by one.
		constexpr int kSpectreExitAlreadyConnected = 4;
		constexpr int kSpectreExitTakenOver = 5;
		if (status == QProcess::NormalExit && exit_code == kSpectreExitAlreadyConnected) {
			auto answer = QMessageBox::question(this, "spectre",
				tr("Your session is already open on another client. Take it over? That client will be "
				   "disconnected."));
			if (answer == QMessageBox::Yes) {
				// The password was cleared when spectre started, so the
				// next Connect needs it again; it carries -T.
				take_over_next_ = true;
				take_over_device_ = chosen_device_id_;
				ui->statusLabel->setText(tr("Enter your password to take the session over."));
				ui->passwordEdit->setFocus();
			}
			return;
		}
		if (status == QProcess::NormalExit && exit_code == kSpectreExitTakenOver) {
			QMessageBox::information(this, "spectre", tr("Your session was taken over by another client."));
			return;
		}
		if (status == QProcess::CrashExit || exit_code != 0) {
			report_abnormal_exit(exit_code, status == QProcess::CrashExit, output);
		}
	});
	connect(spectre_process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) {
			handle_spectre_finished();
			QMessageBox::critical(this, "spectre", "Failed to launch the stream client.");
		}
	});
	spectre_process_->start(spectre_path, args);
	hide();
}

void ConnectWindow::read_spectre_output() {
	if (!spectre_process_) {
		return;
	}
	QString chunk = QString::fromLocal8Bit(spectre_process_->readAllStandardOutput());
	if (chunk.isEmpty()) {
		return;
	}
	if (debug_) {
		// So a -D run still watches spectre live, the way running it by hand
		// would -- the capture below is only read back on an abnormal exit.
		fputs(chunk.toLocal8Bit().constData(), stderr);
		fflush(stderr);
	}
	spectre_output_ += chunk;
	constexpr int kMaxOutputChars = 256 * 1024;
	if (spectre_output_.size() > kMaxOutputChars) {
		spectre_output_.remove(0, spectre_output_.size() - kMaxOutputChars);
	}
}

void ConnectWindow::report_abnormal_exit(int exit_code, bool crashed, const QString &output) {
	QMessageBox box(QMessageBox::Warning, "spectre", tr("The stream client closed unexpectedly."),
		QMessageBox::Ok, this);
	box.setInformativeText(crashed ? tr("spectre was terminated by a signal.")
								   : tr("spectre exited with status %1.").arg(exit_code));
	if (!output.isEmpty()) {
		box.setDetailedText(output);
	}
	box.exec();
}

void ConnectWindow::handle_spectre_finished() {
	if (spectre_process_) {
		spectre_process_->deleteLater();
		spectre_process_ = nullptr;
	}
	set_busy(false);
	show();
}
