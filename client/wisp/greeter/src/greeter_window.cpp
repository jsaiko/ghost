// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "greeter_window.h"

#include "agent_files.h"

#include "gdp/cert_fingerprint.hpp"
#include "gdp/error_codes.hpp"
#include "gdp/lobby_client.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QSocketNotifier>
#include <QStackedWidget>
#include <QStyle>
#include <QSysInfo>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>

namespace {

// spectre's kExitEndedByLocalLogin (client/spectre/src/stream/stream_session.hpp).
constexpr int kSpectreExitEndedByLocalLogin = 3;
// kExitAlreadyConnected and kExitTakenOver, same header.
constexpr int kSpectreExitAlreadyConnected = 4;
constexpr int kSpectreExitTakenOver = 5;

constexpr int kDeviceIdRole = Qt::UserRole;
constexpr int kTypeIdRole = Qt::UserRole + 1;
constexpr int kDeviceNameRole = Qt::UserRole + 2;
constexpr int kDeviceIndexRole = Qt::UserRole + 3;

// Host tiles as in spectre-qt's host picker (session_type_dialog.cpp): the
// display icon with some clear space above it, a tile per host.
constexpr QSize kIconSize(80, 72);
constexpr int kIconTopPad = 10;
constexpr QSize kTileSize(170, 136);
constexpr QSize kGridSize(180, 150);

constexpr auto kStyle = R"(
	QWidget { background: #11151a; color: #e4e7eb; font-size: 16px; }
	QLabel#title { font-size: 40px; font-weight: 300; }
	QLabel#subtitle { color: #8b95a1; }
	QLabel#error { color: #ff7b72; }
	QLabel#footer { color: #6b7580; font-size: 13px; }
	QLineEdit, QListWidget { background: #1c2229; border: 1px solid #2d3640; border-radius: 6px; padding: 8px; }
	QLineEdit:focus, QListWidget:focus { border-color: #4c8dd6; }
	QListWidget::item { padding: 10px; border-radius: 4px; }
	QListWidget::item:selected { background: #2b4c70; }
	QListWidget { outline: none; }
	QListWidget[tiles="true"]::item { padding: 0; }
	QListWidget::item:disabled { color: #6b7580; }
	QComboBox { background: #1c2229; border: 1px solid #2d3640; border-radius: 6px; padding: 7px 10px; }
	QComboBox:focus { border-color: #4c8dd6; }
	QComboBox:disabled { color: #8b95a1; }
	QComboBox QAbstractItemView { background: #1c2229; selection-background-color: #2b4c70; }
	QPushButton { background: #2b4c70; border: none; border-radius: 6px; padding: 9px 18px; }
	QPushButton:hover { background: #355d88; }
	QPushButton:disabled { background: #222a33; color: #6b7580; }
	QPushButton#power { background: transparent; color: #8b95a1; font-size: 13px; padding: 6px 10px; }
	QPushButton#power:hover { color: #e4e7eb; }
	QPushButton#power[armed="true"] { color: #ff7b72; }
)";

QString find_spectre_binary() {
	QString candidate = QDir(QCoreApplication::applicationDirPath()).filePath("spectre");
	return QFileInfo::exists(candidate) ? candidate : QString();
}

// The host icon, drawn at the display's pixel ratio so it stays sharp.
QIcon host_icon(qreal dpr) {
	const QPixmap src(QStringLiteral(":/icons/display-icon.png"));
	QPixmap scaled = src.scaled(kIconSize * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	scaled.setDevicePixelRatio(dpr);
	QPixmap out(QSize(kIconSize.width(), kIconSize.height() + kIconTopPad) * dpr);
	out.setDevicePixelRatio(dpr);
	out.fill(Qt::transparent);
	QPainter painter(&out);
	const QSizeF logical = scaled.size() / dpr;
	painter.drawPixmap(QPointF((kIconSize.width() - logical.width()) / 2,
						   kIconTopPad + (kIconSize.height() - logical.height()) / 2),
		scaled);
	painter.end();
	return QIcon(out);
}

// A fixed-width column in the middle of the screen, for a page's content.
QWidget *centered(QWidget *content, int width = 420) {
	auto *outer = new QWidget;
	auto *layout = new QHBoxLayout(outer);
	content->setFixedWidth(width);
	layout->addStretch();
	layout->addWidget(content);
	layout->addStretch();
	return outer;
}

} // namespace

GreeterWindow::GreeterWindow(const BootConfig &config, const QString &run_dir, QWidget *parent)
	: QWidget(parent), config_(config), run_dir_(run_dir) {
	setStyleSheet(kStyle);
	pages_ = new QStackedWidget;
	login_page_ = build_login_page();
	choice_page_ = build_choice_page();
	prompt_page_ = build_prompt_page();
	take_over_page_ = build_take_over_page();
	pages_->addWidget(login_page_);
	pages_->addWidget(choice_page_);
	pages_->addWidget(prompt_page_);
	pages_->addWidget(take_over_page_);

	auto *layout = new QVBoxLayout(this);
	layout->addStretch(2);
	layout->addWidget(pages_);
	layout->addStretch(3);
	layout->addWidget(build_footer());

	if (!config_.error.isEmpty()) {
		QWidget *fatal = build_fatal_page();
		pages_->addWidget(fatal);
		pages_->setCurrentWidget(fatal);
		return;
	}
	pages_->setCurrentWidget(login_page_);
	username_edit_->setFocus();
}

GreeterWindow::~GreeterWindow() {
	reset_login();
	if (spectre_process_) {
		spectre_process_->disconnect(this);
	}
}

QWidget *GreeterWindow::build_login_page() {
	auto *card = new QWidget;
	auto *layout = new QVBoxLayout(card);
	layout->setSpacing(12);

	auto *title = new QLabel("Welcome");
	title->setObjectName("title");
	title->setAlignment(Qt::AlignCenter);
	auto *subtitle = new QLabel("Sign in to your desktop");
	subtitle->setObjectName("subtitle");
	subtitle->setAlignment(Qt::AlignCenter);

	username_edit_ = new QLineEdit;
	username_edit_->setPlaceholderText("Username");
	password_edit_ = new QLineEdit;
	password_edit_->setPlaceholderText("Password");
	password_edit_->setEchoMode(QLineEdit::Password);
	sign_in_button_ = new QPushButton("Sign in");
	sign_in_button_->setDefault(true);

	status_label_ = new QLabel;
	status_label_->setObjectName("subtitle");
	status_label_->setAlignment(Qt::AlignCenter);
	error_label_ = new QLabel;
	error_label_->setObjectName("error");
	error_label_->setAlignment(Qt::AlignCenter);
	error_label_->setWordWrap(true);

	layout->addWidget(title);
	layout->addWidget(subtitle);
	layout->addSpacing(16);
	layout->addWidget(username_edit_);
	layout->addWidget(password_edit_);
	layout->addWidget(sign_in_button_);
	layout->addWidget(status_label_);
	layout->addWidget(error_label_);

	connect(sign_in_button_, &QPushButton::clicked, this, &GreeterWindow::start_login);
	connect(username_edit_, &QLineEdit::returnPressed, this, [this]() {
		(password_edit_->text().isEmpty() ? password_edit_ : username_edit_)->setFocus();
		if (!password_edit_->text().isEmpty()) {
			start_login();
		}
	});
	connect(password_edit_, &QLineEdit::returnPressed, this, &GreeterWindow::start_login);
	return centered(card);
}

QWidget *GreeterWindow::build_choice_page() {
	auto *card = new QWidget;
	auto *layout = new QVBoxLayout(card);
	layout->setSpacing(12);
	choice_title_ = new QLabel;
	choice_title_->setObjectName("title");
	choice_title_->setAlignment(Qt::AlignCenter);
	choice_list_ = new QListWidget;
	choice_list_->setMinimumHeight(320);
	choice_list_->setMovement(QListView::Static);
	choice_list_->setResizeMode(QListView::Adjust);
	choice_list_->setWordWrap(true);

	// Bottom row as in spectre-qt: the session for the selected host on the
	// left, the buttons on the right.
	auto *buttons = new QHBoxLayout;
	session_row_ = new QWidget;
	auto *session_layout = new QHBoxLayout(session_row_);
	session_layout->setContentsMargins(0, 0, 0, 0);
	session_layout->addWidget(new QLabel("Session:"));
	session_combo_ = new QComboBox;
	session_combo_->setMinimumContentsLength(16);
	session_layout->addWidget(session_combo_);
	auto *back = new QPushButton("Back");
	auto *go = new QPushButton("Connect");
	buttons->addWidget(session_row_);
	buttons->addStretch();
	buttons->addWidget(back);
	buttons->addWidget(go);
	layout->addWidget(choice_title_);
	layout->addWidget(choice_list_);
	layout->addLayout(buttons);

	connect(back, &QPushButton::clicked, this, [this]() { back_to_login(); });
	connect(go, &QPushButton::clicked, this, [this]() { accept_choice(choice_list_->currentItem()); });
	connect(choice_list_, &QListWidget::itemActivated, this, &GreeterWindow::accept_choice);
	connect(choice_list_, &QListWidget::currentItemChanged, this, [this, go]() {
		go->setEnabled(choice_list_->currentItem() != nullptr);
		update_session_box();
	});
	auto *escape = new QShortcut(Qt::Key_Escape, card);
	connect(escape, &QShortcut::activated, this, [this]() { back_to_login(); });
	return centered(card, 620);
}

QWidget *GreeterWindow::build_prompt_page() {
	auto *card = new QWidget;
	auto *layout = new QVBoxLayout(card);
	layout->setSpacing(12);
	prompt_label_ = new QLabel;
	prompt_label_->setWordWrap(true);
	prompt_edit_ = new QLineEdit;
	auto *buttons = new QHBoxLayout;
	auto *cancel = new QPushButton("Cancel");
	auto *ok = new QPushButton("Continue");
	buttons->addWidget(cancel);
	buttons->addStretch();
	buttons->addWidget(ok);
	layout->addWidget(prompt_label_);
	layout->addWidget(prompt_edit_);
	layout->addLayout(buttons);

	connect(ok, &QPushButton::clicked, this, &GreeterWindow::answer_prompt);
	connect(prompt_edit_, &QLineEdit::returnPressed, this, &GreeterWindow::answer_prompt);
	connect(cancel, &QPushButton::clicked, this, [this]() { back_to_login(); });
	return centered(card);
}

QWidget *GreeterWindow::build_take_over_page() {
	auto *card = new QWidget;
	auto *layout = new QVBoxLayout(card);
	layout->setSpacing(12);
	auto *message = new QLabel("Your session is already open on another client. Take it over? That client "
							   "will be disconnected.");
	message->setWordWrap(true);
	auto *buttons = new QHBoxLayout;
	auto *cancel = new QPushButton("Cancel");
	auto *ok = new QPushButton("Take over");
	ok->setDefault(true);
	buttons->addWidget(cancel);
	buttons->addStretch();
	buttons->addWidget(ok);
	layout->addWidget(message);
	layout->addLayout(buttons);

	connect(ok, &QPushButton::clicked, this, &GreeterWindow::take_over);
	connect(cancel, &QPushButton::clicked, this, [this]() { back_to_login(); });
	auto *escape = new QShortcut(Qt::Key_Escape, card);
	connect(escape, &QShortcut::activated, this, [this]() { back_to_login(); });
	return centered(card);
}

QWidget *GreeterWindow::build_fatal_page() {
	auto *card = new QWidget;
	auto *layout = new QVBoxLayout(card);
	layout->setSpacing(16);
	auto *title = new QLabel("Can't sign in");
	title->setObjectName("title");
	title->setAlignment(Qt::AlignCenter);
	auto *message = new QLabel(config_.error +
		"\n\nAsk your administrator to check the Wisp boot server's "
		"VEIL_HOST and VEIL_CERT_SHA256 settings.");
	message->setObjectName("error");
	message->setAlignment(Qt::AlignCenter);
	message->setWordWrap(true);
	layout->addWidget(title);
	layout->addWidget(message);
	return centered(card, 640);
}

QWidget *GreeterWindow::build_footer() {
	auto *footer = new QWidget;
	auto *layout = new QHBoxLayout(footer);
	QString where = config_.veil_host.isEmpty()
		? QString("no Veil configured")
		: QString("Veil %1:%2").arg(config_.veil_host).arg(config_.veil_port);
	auto *info = new QLabel(QSysInfo::machineHostName() + "  ·  " + where);
	info->setObjectName("footer");
	reboot_button_ = new QPushButton("Restart");
	reboot_button_->setObjectName("power");
	poweroff_button_ = new QPushButton("Shut down");
	poweroff_button_->setObjectName("power");
	layout->addWidget(info);
	layout->addStretch();
	layout->addWidget(reboot_button_);
	layout->addWidget(poweroff_button_);
	connect(reboot_button_, &QPushButton::clicked, this,
		[this]() { power_action(reboot_button_, "reboot"); });
	connect(poweroff_button_, &QPushButton::clicked, this,
		[this]() { power_action(poweroff_button_, "poweroff"); });
	return footer;
}

void GreeterWindow::power_action(QPushButton *button, const QString &verb) {
	// Two clicks, so a stray one doesn't take the client down: the first arms
	// the button for a few seconds, the second acts.
	if (!button->property("armed").toBool()) {
		QString label = button->text();
		button->setProperty("armed", true);
		button->setText(verb == "reboot" ? "Click again to restart" : "Click again to shut down");
		button->style()->polish(button);
		QTimer::singleShot(4000, button, [button, label]() {
			button->setProperty("armed", false);
			button->setText(label);
			button->style()->polish(button);
		});
		return;
	}
	// logind lets the active local session do this (polkit's allow_active).
	QProcess::startDetached("systemctl", {verb});
}

void GreeterWindow::set_status(const QString &status) {
	status_label_->setText(status);
	bool busy = !status.isEmpty();
	username_edit_->setEnabled(!busy);
	password_edit_->setEnabled(!busy);
	sign_in_button_->setEnabled(!busy);
}

void GreeterWindow::back_to_login(const QString &error) {
	reset_login();
	set_status(QString());
	// Whatever happened, the next attempt types the password again.
	password_edit_->clear();
	error_label_->setText(error);
	pages_->setCurrentWidget(login_page_);
	(username_edit_->text().isEmpty() ? username_edit_ : password_edit_)->setFocus();
}

void GreeterWindow::reset_login() {
	// The notifier first: it wraps login_client_'s notify_fd().
	delete login_notifier_;
	login_notifier_ = nullptr;
	login_client_.reset();
}

void GreeterWindow::start_login() {
	QString username = username_edit_->text().trimmed();
	if (username.isEmpty()) {
		error_label_->setText("Enter your username.");
		username_edit_->setFocus();
		return;
	}
	if (login_client_ || spectre_process_) {
		return;
	}
	error_label_->clear();
	first_prompt_answered_ = false;
	cert_mismatch_ = false;
	chosen_type_.clear();
	chosen_host_.clear();
	take_over_next_ = false;
	login_client_ = std::make_unique<gdp::LobbyClient>();
	gdp::LobbyClient *client = login_client_.get();

	// Two trust roots, and no prompt: the fingerprint the boot server
	// handed us, or -- for a Veil that presents its CA-issued web
	// certificate to clients ([lobby] clients_use_web_cert) -- a
	// certificate the image's CA store vouches for under the name we
	// dialed (gdp-spec.md §2.3). Anything else ends the login before the
	// password is sent.
	client->ca_trust.system = true;
	client->on_certificate = [this, client](const std::string &cert_sha256) {
		if (normalize_fingerprint(QString::fromStdString(cert_sha256)) == config_.veil_cert ||
			client->certificate_ca_verified()) {
			return true;
		}
		cert_mismatch_ = true;
		qWarning("wisp-greeter: Veil presented certificate %s, expected %s", cert_sha256.c_str(),
			qPrintable(config_.veil_cert));
		return false;
	};

	client->on_auth_prompt = [this](const std::string &prompt, bool echo) {
		if (!login_client_) {
			return;
		}
		if (!first_prompt_answered_) {
			// The password prompt, already answered by the form.
			first_prompt_answered_ = true;
			login_client_->respond(password_edit_->text().toStdString());
			set_status("Signing in…");
			return;
		}
		QString qprompt = QString::fromStdString(prompt);
		QTimer::singleShot(0, this, [this, qprompt, echo]() {
			if (login_client_) {
				ask_prompt(qprompt, echo);
			}
		});
	};

	// The rest are deferred to the next event-loop turn: each may end the
	// login, which destroys the LobbyClient whose callback this is.
	client->on_device_list = [this](const std::vector<gdp::DeviceInfo> &devices) {
		QTimer::singleShot(0, this, [this, devices]() {
			if (login_client_) {
				choose_device(devices);
			}
		});
	};
	client->on_session_list = [this](const gdp::SessionListInfo &list) {
		QTimer::singleShot(0, this, [this, list]() {
			if (login_client_) {
				choose_session(list);
			}
		});
	};
	client->on_redirect = [this](const std::string &host, uint16_t port, const std::string &token,
							  int64_t /*expiry_unix*/, const std::string &cert_sha256) {
		QString qhost = QString::fromStdString(host);
		QString qtoken = QString::fromStdString(token);
		QString qcert = QString::fromStdString(cert_sha256);
		QTimer::singleShot(0, this, [this, qhost, port, qtoken, qcert]() {
			reset_login();
			if (!gdp::is_sha256_hex(qcert.toStdString())) {
				back_to_login("The server didn't say which certificate the session will use, so the session "
							  "can't be verified.");
				return;
			}
			launch_spectre(qhost, port, qtoken, qcert);
		});
	};
	client->on_error = [this](const std::string &message) {
		QString qmessage = QString::fromStdString(message);
		if (cert_mismatch_) {
			qmessage = "The Veil server's certificate doesn't match the one this client was configured "
					   "with, so your password was not sent. Tell your administrator.";
		} else {
			switch (static_cast<gdp::ErrorCode>(login_client_ ? login_client_->error_code() : 0)) {
			case gdp::ErrorCode::kAuthFailed: qmessage = "Wrong username or password."; break;
			case gdp::ErrorCode::kHostOffline:
				qmessage = "That desktop isn't available right now. Try again later, or pick another.";
				break;
			case gdp::ErrorCode::kHostAuthFailed:
				qmessage = "The desktop rejected your password although the sign-in server accepted it. "
						   "Ask your administrator.";
				break;
			case gdp::ErrorCode::kNotEntitled: qmessage = "You aren't allowed to use that desktop."; break;
			default: break;
			}
		}
		QTimer::singleShot(0, this, [this, qmessage]() { back_to_login(qmessage); });
	};

	if (!client->connect(config_.veil_host.toStdString(), config_.veil_port, username.toStdString())) {
		login_client_.reset();
		error_label_->setText("Couldn't reach the sign-in server.");
		return;
	}
	login_notifier_ = new QSocketNotifier(client->notify_fd(), QSocketNotifier::Read, this);
	connect(login_notifier_, &QSocketNotifier::activated, this, [this]() {
		if (login_client_) {
			login_client_->dispatch();
		}
	});
	set_status("Connecting…");
}

void GreeterWindow::choose_device(const std::vector<gdp::DeviceInfo> &devices) {
	// One usable host with nothing to choose on it: don't ask.
	const gdp::DeviceInfo *only = nullptr;
	int online = 0;
	for (const auto &device : devices) {
		if (device.online) {
			++online;
			only = &device;
		}
	}
	if (online == 1 && devices.size() == 1 && (only->has_session || only->types.size() <= 1)) {
		chosen_type_ = only->types.empty() ? QString() : QString::fromStdString(only->types.front().id);
		chosen_host_ = QString::fromStdString(only->name);
		login_client_->select_device(only->id);
		set_status(QString("Connecting to %1…").arg(QString::fromStdString(only->name)));
		return;
	}

	// A tile per host, as in spectre-qt: its icon, its name, and a note
	// under it. The Session box below picks the desktop. Offline hosts are
	// listed but can't be picked.
	choice_mode_ = ChoiceMode::kDevice;
	choice_title_->setText("Choose a host");
	devices_ = devices;
	choice_list_->clear();
	choice_list_->setViewMode(QListView::IconMode);
	choice_list_->setUniformItemSizes(true);
	choice_list_->setSpacing(8);
	choice_list_->setGridSize(kGridSize);
	choice_list_->setIconSize(QSize(kIconSize.width(), kIconSize.height() + kIconTopPad));
	set_tiles(true);
	session_row_->show();
	const QIcon icon = host_icon(devicePixelRatioF());
	for (size_t i = 0; i < devices_.size(); ++i) {
		const auto &device = devices_[i];
		QString name = QString::fromStdString(device.name);
		QString text = name;
		if (!device.online) {
			text += "\noffline";
		} else if (device.has_session) {
			text += device.session_type.empty()
				? QString("\nrunning")
				: QString("\nrunning (%1)").arg(QString::fromStdString(device.session_type));
		}
		auto *item = new QListWidgetItem(icon, text, choice_list_);
		item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
		item->setSizeHint(kTileSize);
		item->setData(kDeviceIdRole, QString::fromStdString(device.id));
		item->setData(kDeviceNameRole, name);
		item->setData(kDeviceIndexRole, static_cast<int>(i));
		if (!device.online) {
			item->setFlags(item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
		}
	}
	// The first host that can be chosen: Veil lists running sessions first.
	for (int i = 0; i < choice_list_->count(); ++i) {
		if (choice_list_->item(i)->flags() & Qt::ItemIsEnabled) {
			choice_list_->setCurrentRow(i);
			break;
		}
	}
	set_status(QString());
	pages_->setCurrentWidget(choice_page_);
	choice_list_->setFocus();
}

void GreeterWindow::choose_session(const gdp::SessionListInfo &list) {
	// One session per user: a running one is resumed whatever is asked for.
	if (!list.running.empty()) {
		// Open on another client: ask now, while the login can still go on,
		// rather than let spectre be refused.
		if (list.running.front().viewer_attached) {
			ask_take_over(list.running.front().session_type);
			return;
		}
		login_client_->open_session(list.running.front().session_type);
		set_status("Resuming your session…");
		return;
	}
	// Picked together with the host; a type it no longer offers falls back
	// to its default.
	if (!chosen_type_.isEmpty() || list.types.size() <= 1) {
		bool offered = false;
		for (const auto &type : list.types) {
			offered = offered || QString::fromStdString(type.id) == chosen_type_;
		}
		login_client_->open_session(offered ? chosen_type_.toStdString() : std::string());
		set_status("Starting your session…");
		return;
	}
	choice_mode_ = ChoiceMode::kType;
	choice_title_->setText("Choose a desktop");
	choice_list_->clear();
	choice_list_->setViewMode(QListView::ListMode);
	choice_list_->setGridSize(QSize());
	choice_list_->setSpacing(0);
	choice_list_->setIconSize(QSize());
	set_tiles(false);
	session_row_->hide();
	for (const auto &type : list.types) {
		auto *item = new QListWidgetItem(QString::fromStdString(type.name), choice_list_);
		item->setData(kTypeIdRole, QString::fromStdString(type.id));
		if (type.id == list.default_type) {
			choice_list_->setCurrentItem(item);
		}
	}
	set_status(QString());
	pages_->setCurrentWidget(choice_page_);
	choice_list_->setFocus();
}

void GreeterWindow::accept_choice(QListWidgetItem *item) {
	if (!login_client_ || !item || !(item->flags() & Qt::ItemIsEnabled)) {
		return;
	}
	pages_->setCurrentWidget(login_page_);
	chosen_type_ = choice_mode_ == ChoiceMode::kDevice ? session_combo_->currentData().toString()
													   : item->data(kTypeIdRole).toString();
	if (choice_mode_ == ChoiceMode::kDevice) {
		chosen_host_ = item->data(kDeviceNameRole).toString();
		login_client_->select_device(item->data(kDeviceIdRole).toString().toStdString());
		set_status("Connecting…");
	} else {
		login_client_->open_session(chosen_type_.toStdString());
		set_status("Starting your session…");
	}
}

// Host tiles take the stylesheet's item padding away (it would eat into the
// tile's text area); the plain desktop list keeps it.
void GreeterWindow::set_tiles(bool tiles) {
	choice_list_->setProperty("tiles", tiles);
	choice_list_->style()->unpolish(choice_list_);
	choice_list_->style()->polish(choice_list_);
}

void GreeterWindow::update_session_box() {
	session_combo_->clear();
	QListWidgetItem *item = choice_list_->currentItem();
	if (choice_mode_ != ChoiceMode::kDevice || !item) {
		session_combo_->setEnabled(false);
		return;
	}
	const gdp::DeviceInfo &device = devices_[static_cast<size_t>(item->data(kDeviceIndexRole).toInt())];
	if (device.has_session) {
		// One session per user: whatever is running gets resumed.
		QString name = QString::fromStdString(device.session_type);
		for (const auto &type : device.types) {
			if (type.id == device.session_type) {
				name = QString::fromStdString(type.name);
			}
		}
		session_combo_->addItem(name.isEmpty() ? QString("Running session") : name,
			QString::fromStdString(device.session_type));
		session_combo_->setEnabled(false);
		return;
	}
	if (device.types.empty()) {
		// The host didn't say what it offers: it starts its own default.
		session_combo_->addItem("Default", QString());
		session_combo_->setEnabled(false);
		return;
	}
	for (const auto &type : device.types) {
		session_combo_->addItem(QString::fromStdString(type.name), QString::fromStdString(type.id));
	}
	int default_index = session_combo_->findData(QString::fromStdString(device.default_type));
	if (default_index >= 0) {
		session_combo_->setCurrentIndex(default_index);
	}
	session_combo_->setEnabled(device.types.size() > 1);
}

void GreeterWindow::ask_prompt(const QString &prompt, bool echo) {
	prompt_label_->setText(prompt);
	prompt_edit_->clear();
	prompt_edit_->setEchoMode(echo ? QLineEdit::Normal : QLineEdit::Password);
	pages_->setCurrentWidget(prompt_page_);
	prompt_edit_->setFocus();
}

void GreeterWindow::answer_prompt() {
	if (!login_client_) {
		return;
	}
	pages_->setCurrentWidget(login_page_);
	login_client_->respond(prompt_edit_->text().toStdString());
	prompt_edit_->clear();
	set_status("Signing in…");
}

void GreeterWindow::ask_take_over(const std::string &session_type) {
	take_over_type_ = session_type;
	set_status(QString());
	pages_->setCurrentWidget(take_over_page_);
	take_over_page_->setFocus();
}

void GreeterWindow::take_over() {
	if (!login_client_) {
		return;
	}
	take_over_next_ = true;
	pages_->setCurrentWidget(login_page_);
	login_client_->open_session(take_over_type_);
	set_status("Taking over your session…");
}

void GreeterWindow::launch_spectre(const QString &host, uint16_t port, const QString &token,
	const QString &cert_sha256) {
	QString spectre_path = find_spectre_binary();
	if (spectre_path.isEmpty()) {
		back_to_login("The stream client (spectre) is missing from this image.");
		return;
	}
	password_edit_->clear();
	QStringList args;
	args << "-h" << host << "-p" << QString::number(port) << "-P" << cert_sha256;
	// Kiosk: fullscreen for good. The session ends from spectre's own menu.
	// The rest is the profile Veil sent (through wisp-agent), read afresh
	// at every launch so a change applies from the next sign-in.
	args << "-K" << load_profile(run_dir_).to_args(screen(), /*debug=*/false);
	if (take_over_next_) {
		args << "-T"; // only for this one attempt, after the user agreed
		take_over_next_ = false;
	}

	spectre_process_ = new QProcess(this);
	spectre_last_error_.clear();
	// Passed on to our stderr (the kiosk unit's journal), keeping spectre's
	// last "spectre: ..." line to show if it fails.
	spectre_process_->setProcessChannelMode(QProcess::MergedChannels);
	// The token goes in the environment, not the arguments: other local
	// users can read a command line, not this user's environment.
	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	env.insert(QStringLiteral("SPECTRE_TOKEN"), token);
	if (load_debug_logging(run_dir_)) {
		env.insert(QStringLiteral("SPECTRE_LOG"), QStringLiteral("debug"));
	}
	spectre_process_->setProcessEnvironment(env);
	connect(spectre_process_, &QProcess::readyReadStandardOutput, this, [this]() {
		while (spectre_process_ && spectre_process_->canReadLine()) {
			QByteArray line = spectre_process_->readLine();
			fwrite(line.constData(), 1, line.size(), stderr);
			if (line.startsWith("spectre: ")) {
				spectre_last_error_ = QString::fromUtf8(line.mid(9)).trimmed();
			}
		}
		fflush(stderr);
	});
	connect(spectre_process_, &QProcess::finished, this, [this](int exit_code, QProcess::ExitStatus status) {
		handle_spectre_finished(exit_code, status == QProcess::CrashExit);
	});
	connect(spectre_process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) {
			handle_spectre_finished(-1, true);
		}
	});
	set_status("Starting the session…");
	spectre_process_->start(spectre_path, args);
	write_session(run_dir_, username_edit_->text().trimmed(), chosen_host_, spectre_process_->processId());
	// Out of the way: spectre's fullscreen window takes the screen (and
	// focus) until it exits.
	hide();
}

void GreeterWindow::handle_spectre_finished(int exit_code, bool crashed) {
	if (!spectre_process_) {
		return;
	}
	spectre_process_->deleteLater();
	spectre_process_ = nullptr;
	clear_session(run_dir_);
	QString message;
	if (!crashed && exit_code == kSpectreExitEndedByLocalLogin) {
		message = "You were signed out because you signed in at the desktop itself.";
	} else if (!crashed && exit_code == kSpectreExitAlreadyConnected) {
		// Another client attached after the sign-in asked (choose_session()).
		// The kiosk is shared and the name is gone, so the next sign-in asks
		// again rather than take the session over on this one's word.
		message = "Your session is already open on another client. Sign in again to take it over.";
	} else if (!crashed && exit_code == kSpectreExitTakenOver) {
		message = "Your session was taken over by another client.";
	} else if (crashed || exit_code != 0) {
		message = exit_code == -1 ? QString("The stream client couldn't be started.")
								  : QString("The session ended unexpectedly (exit code %1).").arg(exit_code);
		if (!spectre_last_error_.isEmpty()) {
			message += "\n" + spectre_last_error_;
		}
	}
	// A shared kiosk: the next person at it shouldn't see who used it last.
	username_edit_->clear();
	back_to_login(message);
	// show() keeps the window state it was hidden with (fullscreen, or
	// --windowed).
	show();
	activateWindow();
}
