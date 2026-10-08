// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session_type_dialog.h"
#include "ui_session_type_dialog.h"

#include "fixed_size.h"

#include "gdp/lobby_client.hpp"

#include <QFont>
#include <QIcon>
#include <QLabel>
#include <QListWidgetItem>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// The host icon with some clear space above it: IconMode draws the icon
// flush against the top of a tile, which looks cramped when the tile is
// selected. The icon is drawn at the display's pixel ratio so it stays
// sharp on HiDPI screens.
constexpr QSize kIconSize(80, 72);
constexpr int kIconTopPad = 10;

QIcon host_icon(qreal dpr) {
	const QPixmap src(QStringLiteral(":/icons/display-icon.png"));
	QPixmap scaled = src.scaled(kIconSize * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	// Device pixels, so the painter must not read them as logical ones.
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

} // namespace

SessionTypeDialog::SessionTypeDialog(const std::vector<gdp::DeviceInfo> &devices, QWidget *parent, Mode mode)
	: QDialog(parent), ui(new Ui::SessionTypeDialog), devices_(devices) {
	ui->setupUi(this);
	const QIcon icon = host_icon(devicePixelRatioF());
	ui->deviceList->setIconSize(QSize(kIconSize.width(), kIconSize.height() + kIconTopPad));
	for (size_t i = 0; i < devices_.size(); ++i) {
		const auto &device = devices_[i];
		// A tile: the host's icon, its name, and a note under it.
		QString text = QString::fromStdString(device.name);
		if (!device.online) {
			text += tr("\noffline");
		} else if (device.has_session) {
			text += device.session_type.empty()
				? tr("\nrunning")
				: tr("\nrunning (%1)").arg(QString::fromStdString(device.session_type));
		}
		auto *item = new QListWidgetItem(icon, text, ui->deviceList);
		item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
		// IconMode wraps the text at the icon's width unless told how big
		// the tile is.
		item->setSizeHint(QSize(170, 128));
		item->setData(Qt::UserRole, QString::fromStdString(device.id));
		item->setData(Qt::UserRole + 1, static_cast<int>(i));
		if (!device.online) {
			item->setFlags(item->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
		}
	}
	QPushButton *ok = ui->buttonBox->button(QDialogButtonBox::Ok);
	ok->setText(tr("Connect"));
	auto update_ok = [this, ok]() { ok->setEnabled(ui->deviceList->currentItem() != nullptr); };
	connect(ui->deviceList, &QListWidget::currentItemChanged, this, [this, update_ok]() {
		update_ok();
		update_session_box();
	});
	connect(ui->deviceList, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
		if (item->flags() & Qt::ItemIsEnabled) {
			accept();
		}
	});
	// The first host that can be chosen: the server lists running sessions
	// first, so this resumes one when there is one.
	for (int i = 0; i < ui->deviceList->count(); ++i) {
		if (ui->deviceList->item(i)->flags() & Qt::ItemIsEnabled) {
			ui->deviceList->setCurrentRow(i);
			break;
		}
	}
	update_ok();
	update_session_box();
	if (devices_.empty()) {
		ui->promptLabel->setText(tr("You have no hosts to connect to. Ask your administrator for access."));
	}
	if (mode == Mode::SingleHost && devices_.size() == 1) {
		// Nothing to choose between: the host's icon with its name under it,
		// in place of the tile grid (which stays, hidden, so the session box
		// and the Connect button work as in the grid).
		ui->promptLabel->hide();
		ui->deviceList->hide();
		const QSize shown(144, 128);
		const qreal dpr = devicePixelRatioF();
		QPixmap pixmap = QPixmap(QStringLiteral(":/icons/display-icon.png"))
							 .scaled(shown * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		pixmap.setDevicePixelRatio(dpr);
		auto *picture = new QLabel(this);
		picture->setPixmap(pixmap);
		picture->setAlignment(Qt::AlignCenter);
		auto *name = new QLabel(QString::fromStdString(devices_.front().name), this);
		name->setAlignment(Qt::AlignCenter);
		name->setWordWrap(true);
		name->setTextInteractionFlags(Qt::TextSelectableByMouse);
		QFont font = name->font();
		font.setPointSizeF(font.pointSizeF() * 1.25);
		name->setFont(font);
		auto *host = new QVBoxLayout;
		host->addStretch();
		host->addWidget(picture);
		host->addWidget(name);
		host->addStretch();
		ui->mainLayout->insertLayout(1, host, 1);
		resize(440, 10);
	}
	make_window_fixed_size(this);
}

SessionTypeDialog::~SessionTypeDialog() {
	delete ui;
}

QString SessionTypeDialog::selected_device_id() const {
	QListWidgetItem *item = ui->deviceList->currentItem();
	return item ? item->data(Qt::UserRole).toString() : QString();
}

QString SessionTypeDialog::selected_type_id() const {
	return ui->sessionCombo->currentData().toString();
}

void SessionTypeDialog::update_session_box() {
	ui->sessionCombo->clear();
	QListWidgetItem *item = ui->deviceList->currentItem();
	if (!item) {
		ui->sessionCombo->setEnabled(false);
		return;
	}
	const gdp::DeviceInfo &device = devices_[static_cast<size_t>(item->data(Qt::UserRole + 1).toInt())];
	if (device.has_session) {
		// One session per user: whatever is running gets resumed, so there is
		// nothing to choose -- show what it is.
		QString name = QString::fromStdString(device.session_type);
		for (const auto &type : device.types) {
			if (type.id == device.session_type) {
				name = QString::fromStdString(type.name);
			}
		}
		ui->sessionCombo->addItem(name.isEmpty() ? tr("Running session") : name, QString());
		ui->sessionCombo->setToolTip(tr("Your session on this host is already running and will be resumed."));
		ui->sessionCombo->setEnabled(false);
		return;
	}
	ui->sessionCombo->setToolTip(QString());
	if (device.types.empty()) {
		// The host didn't say what it offers: it starts its own default.
		ui->sessionCombo->addItem(tr("Default"), QString());
		ui->sessionCombo->setEnabled(false);
		return;
	}
	for (const auto &type : device.types) {
		ui->sessionCombo->addItem(QString::fromStdString(type.name), QString::fromStdString(type.id));
	}
	int default_index = ui->sessionCombo->findData(QString::fromStdString(device.default_type));
	if (default_index >= 0) {
		ui->sessionCombo->setCurrentIndex(default_index);
	}
	ui->sessionCombo->setEnabled(device.types.size() > 1);
}
