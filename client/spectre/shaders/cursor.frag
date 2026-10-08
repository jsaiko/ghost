// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#version 450

// `cursorSampler` is a plain (non-ycbcr) sampler over a
// VK_FORMAT_B8G8R8A8_UNORM texture: wraith's DRM_FORMAT_ARGB8888
// CursorShape bytes (same byte order, no swizzle), the splash bitmap, or
// the lossless refinement plane. The pipelines' premultiplied blend state
// (VulkanDevice::create_pipeline) expects premultiplied alpha, which all
// three already are (CursorShape per gdp-spec.md §7.4; the plane's alpha is
// only ever 0 or 1, where the two conventions agree).
layout(set = 0, binding = 0) uniform sampler2D cursorSampler;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
	outColor = texture(cursorSampler, inUV);
}
