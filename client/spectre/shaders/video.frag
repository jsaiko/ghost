// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#version 450

// `videoSampler` is a combined image sampler with an immutable sampler
// (video_image_source.hpp). On Linux that sampler carries a
// VkSamplerYcbcrConversion over VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, so the
// NV12 -> RGB conversion happens in fixed-function sampling hardware; on
// Windows the image is already BGRA. Either way texture() below returns
// RGB and this shader does no colour math.
layout(set = 0, binding = 0) uniform sampler2D videoSampler;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
	outColor = vec4(texture(videoSampler, inUV).rgb, 1.0);
}
