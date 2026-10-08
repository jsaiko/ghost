// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Choosing and opening a DRM render node for the encoder or a gbm allocator.
#pragma once

#include <sys/types.h>

#include <string>

namespace wraith {

// The render node wraith encodes and reads frames back on when nothing else pins
// one. WLR_RENDER_DRM_DEVICE, if set, wins outright (the knob wlroots
// compositors such as labwc honour too, so one setting can pin both). Otherwise every render node is probed
// for hardware encode (hardware_encodable_codecs) and the one that encodes the most preferred codec wins --
// libdrm lists nodes in readdir order, which can change across a reboot, so "the first node" can mean the
// iGPU on one boot and the discrete GPU on the next. Ties, including a host where nothing encodes, keep the
// listed order. Probed once and cached; empty if there is no render node at all.
const std::string &preferred_render_node();

// Opens (O_RDWR | O_CLOEXEC) the render node whose device number is `want`,
// or, when `want` is 0 or no node matches, preferred_render_node(). -1 if
// there is none. The caller owns the fd.
int open_render_node(dev_t want = 0);

} // namespace wraith
