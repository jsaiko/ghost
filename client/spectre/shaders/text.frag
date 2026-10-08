// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#version 450

// The font atlas is a single-channel coverage texture (VK_FORMAT_R8_UNORM,
// see OverlayRenderer::create_text_pipeline); the glyph's color comes from
// the push constant. Output is premultiplied to match the pipeline's blend
// state (same as cursor.frag).
layout(set = 0, binding = 0) uniform sampler2D fontAtlas;

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main() {
	float coverage = texture(fontAtlas, inUV).r;
	float a = coverage * inColor.a;
	outColor = vec4(inColor.rgb * a, a);
}
