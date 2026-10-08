// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "settings_dialog.h"
#include "ui_settings_dialog.h"

#include "fixed_size.h"

#include "gdp/video_codec.hpp"

#include <QSettings>

namespace {

// Offered in the resolution combo after "Match this display". Kept to the
// sizes a remote desktop is actually asked for rather than a full mode list,
// since this is a request to the host, not a local display mode.
const char *const kResolutions[] = {
	"1280x720",
	"1366x768",
	"1600x900",
	"1920x1080",
	"2560x1440",
	"3840x2160",
};

constexpr auto kFullscreenKey = "spectre/fullscreen";
constexpr auto kDecoderKey = "spectre/preferred_decoder";
constexpr auto kResolutionKey = "spectre/resolution";
constexpr auto kCodecKey = "spectre/preferred_codec";
constexpr auto kNetworkKey = "spectre/network_profile";
constexpr auto kTileOutlineKey = "spectre/debug_tile_outlines";
constexpr auto kKioskKey = "spectre/debug_kiosk";
constexpr auto kGamepadKey = "spectre/forward_gamepads";
constexpr auto kMicrophoneKey = "spectre/microphone";
constexpr auto kPyrowaveKey = "spectre/allow_pyrowave";
constexpr auto kFollowWindowKey = "spectre/follow_window";
constexpr auto kViewKey = "spectre/view";
constexpr auto kLosslessKey = "spectre/lossless_refinement";

} // namespace

SpectreSettings load_settings() {
	QSettings store;
	SpectreSettings settings;
	settings.fullscreen = store.value(kFullscreenKey, false).toBool();
	settings.preferred_decoder = store.value(kDecoderKey, settings.preferred_decoder).toString();
	settings.resolution = store.value(kResolutionKey, QString()).toString();
	settings.preferred_codec = store.value(kCodecKey, QString()).toString();
	settings.network_profile = store.value(kNetworkKey, settings.network_profile).toString();
	// Gamepad forwarding defaults on here, unlike the spectre CLI's -G
	// (docs/reference/command-line.md#spectre).
	settings.lossless_refinement = store.value(kLosslessKey, true).toBool();
	settings.debug_tile_outlines = store.value(kTileOutlineKey, false).toBool();
	settings.debug_kiosk = store.value(kKioskKey, false).toBool();
	settings.forward_gamepads = store.value(kGamepadKey, true).toBool();
	settings.microphone = store.value(kMicrophoneKey, false).toBool();
	settings.allow_pyrowave = store.value(kPyrowaveKey, true).toBool();
	settings.follow_window = store.value(kFollowWindowKey, false).toBool();
	settings.view = store.value(kViewKey, QString()).toString();
	return settings;
}

void save_settings(const SpectreSettings &settings) {
	QSettings store;
	store.setValue(kFullscreenKey, settings.fullscreen);
	store.setValue(kDecoderKey, settings.preferred_decoder);
	store.setValue(kResolutionKey, settings.resolution);
	store.setValue(kCodecKey, settings.preferred_codec);
	store.setValue(kNetworkKey, settings.network_profile);
	store.setValue(kTileOutlineKey, settings.debug_tile_outlines);
	store.setValue(kKioskKey, settings.debug_kiosk);
	store.setValue(kGamepadKey, settings.forward_gamepads);
	store.setValue(kMicrophoneKey, settings.microphone);
	store.setValue(kPyrowaveKey, settings.allow_pyrowave);
	store.setValue(kFollowWindowKey, settings.follow_window);
	store.setValue(kViewKey, settings.view);
	store.setValue(kLosslessKey, settings.lossless_refinement);
}

SettingsDialog::SettingsDialog(const SpectreSettings &settings, bool debug, QWidget *parent)
	: QDialog(parent), ui(new Ui::SettingsDialog) {
	ui->setupUi(this);
	ui->debugGroup->setVisible(debug);

	ui->resolutionCombo->addItem(tr("Match this display"), QString());
	for (const char *resolution : kResolutions) {
		QString size = QString::fromLatin1(resolution);
		ui->resolutionCombo->addItem(size, size);
	}
	int index = ui->resolutionCombo->findData(settings.resolution);
	ui->resolutionCombo->setCurrentIndex(index >= 0 ? index : 0);

	ui->resolutionCombo->setToolTip(
		tr("Match this display asks for this screen's full resolution, windowed or fullscreen. "
		   "Reconnecting to a session that's already running resizes it to this too. The session "
		   "menu changes it mid-session."));

	ui->fullscreenCheck->setChecked(settings.fullscreen);
	ui->followWindowCheck->setChecked(settings.follow_window);
	ui->viewCombo->addItem(tr("As last set in the session menu"), QString());
	ui->viewCombo->addItem(tr("Fit to window"), QStringLiteral("fit"));
	ui->viewCombo->addItem(tr("Actual size"), QStringLiteral("actual"));
	int view_index = ui->viewCombo->findData(settings.view);
	ui->viewCombo->setCurrentIndex(view_index >= 0 ? view_index : 0);
	// spectre ignores -A at actual size (the window shows part of the
	// desktop rather than asking for a size). "As last set" could be
	// either, so the checkbox stays live for it.
	auto update_follow_enabled = [this]() {
		ui->followWindowCheck->setEnabled(
			ui->viewCombo->currentData().toString() != QStringLiteral("actual"));
	};
	connect(ui->viewCombo, &QComboBox::currentIndexChanged, this, update_follow_enabled);
	update_follow_enabled();
	ui->viewCombo->setToolTip(
		tr("Fit to window scales the remote desktop to the window. Actual size shows it pixel for pixel: "
		   "centred when it is smaller than the window, and when it is bigger, push the pointer "
		   "against an edge to scroll. The session menu switches between them mid-session."));
	// Most-preferred first: Vulkan Video, the platform's own, software.
	ui->decoderCombo->addItem(tr("Vulkan Video"), QStringLiteral("vulkan"));
#if defined(_WIN32)
	ui->decoderCombo->addItem(tr("D3D11VA"), QStringLiteral("native"));
#elif defined(__APPLE__)
	ui->decoderCombo->addItem(tr("VideoToolbox"), QStringLiteral("native"));
#else
	ui->decoderCombo->addItem(tr("VA-API"), QStringLiteral("native"));
#endif
	ui->decoderCombo->addItem(tr("Software"), QStringLiteral("software"));
	int decoder_index = ui->decoderCombo->findData(settings.preferred_decoder);
	ui->decoderCombo->setCurrentIndex(decoder_index >= 0 ? decoder_index : 0);
	ui->tileOutlineCheck->setChecked(settings.debug_tile_outlines);
	ui->kioskCheck->setChecked(settings.debug_kiosk);
	ui->gamepadCheck->setChecked(settings.forward_gamepads);
	ui->microphoneCheck->setChecked(settings.microphone);
	ui->pyrowaveCheck->setChecked(settings.allow_pyrowave);
	ui->losslessCheck->setChecked(settings.lossless_refinement);

	// "Auto" plus every codec token this build's spectre knows about
	// (gdp/video_codec.hpp), so the dropdown can never offer a token the
	// running spectre binary would reject as unrecognized.
	ui->codecCombo->addItem(tr("Auto"), QString());
	for (const std::string &token : gdp::all_video_codec_tokens()) {
		QString label = QString::fromStdString(token);
		if (token == "h265") {
			label = tr("%1 (req. hardware)").arg(label);
		} else if (token == "av1") {
			label = tr("%1 (req. newer hardware)").arg(label);
		} else if (token == "pyrowave") {
			label = tr("%1 (wired LAN only)").arg(label);
		}
		ui->codecCombo->addItem(label, QString::fromStdString(token));
	}
	int codec_index = ui->codecCombo->findData(settings.preferred_codec);
	ui->codecCombo->setCurrentIndex(codec_index >= 0 ? codec_index : 0);

	ui->networkCombo->addItem(tr("Auto"), QStringLiteral("auto"));
	ui->networkCombo->addItem(tr("LAN"), QStringLiteral("lan"));
	ui->networkCombo->addItem(tr("Internet"), QStringLiteral("internet"));
	ui->networkCombo->addItem(tr("Mobile / lossy Wi-Fi"), QStringLiteral("mobile"));
	int network_index = ui->networkCombo->findData(settings.network_profile);
	ui->networkCombo->setCurrentIndex(network_index >= 0 ? network_index : 0);
	make_window_fixed_size(this);
}

SettingsDialog::~SettingsDialog() {
	delete ui;
}

SpectreSettings SettingsDialog::settings() const {
	SpectreSettings settings;
	settings.fullscreen = ui->fullscreenCheck->isChecked();
	settings.follow_window = ui->followWindowCheck->isChecked();
	settings.view = ui->viewCombo->currentData().toString();
	settings.preferred_decoder = ui->decoderCombo->currentData().toString();
	settings.resolution = ui->resolutionCombo->currentData().toString();
	settings.preferred_codec = ui->codecCombo->currentData().toString();
	settings.network_profile = ui->networkCombo->currentData().toString();
	settings.debug_tile_outlines = ui->tileOutlineCheck->isChecked();
	settings.debug_kiosk = ui->kioskCheck->isChecked();
	settings.forward_gamepads = ui->gamepadCheck->isChecked();
	settings.microphone = ui->microphoneCheck->isChecked();
	settings.allow_pyrowave = ui->pyrowaveCheck->isChecked();
	settings.lossless_refinement = ui->losslessCheck->isChecked();
	return settings;
}
