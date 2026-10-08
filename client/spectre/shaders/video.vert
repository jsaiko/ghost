// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#version 450

// Fullscreen triangle via gl_VertexIndex -- no vertex buffer needed. The
// third vertex extends past the clip volume on purpose; the rasterizer
// clips it, and the visible half covers exactly the screen.
layout(location = 0) out vec2 outUV;

// How much of the decoded texture is the picture: display size over coded
// size, (1, 1) when they match. AV1 cannot crop, so wraith pads a
// 1366-wide desktop to 1368 and the extra columns are part of the decoded
// frame; sampling all of it would stretch them into view and put the
// video a hair out of register with the lossless refinement plane, which
// is sized to the display. See VulkanPresenter::set_display_size().
layout(push_constant) uniform Push {
	vec2 uvScale;
} push;

void main() {
	vec2 positions[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
	vec2 uvs[3] = vec2[](vec2(0.0, 0.0), vec2(2.0, 0.0), vec2(0.0, 2.0));
	gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
	outUV = uvs[gl_VertexIndex] * push.uvScale;
}
