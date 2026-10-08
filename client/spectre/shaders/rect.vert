// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#version 450

// Draws one solid axis-aligned quad (triangle strip, 4 vertices, no vertex
// buffer): every filled rectangle in spectre's UI (menu panels, the stats
// backdrop and latency bar -- see ui/draw_list.hpp). Vulkan NDC: x right,
// y down, so (-1,-1) is the top-left of the screen.
layout(push_constant) uniform PushConstants {
	vec4 rect;  // x0, y0, width, height, all in NDC units
	vec4 color; // rgba, premultiplied (the pipeline blends)
} pc;

layout(location = 0) out vec4 outColor;

void main() {
	vec2 unit[4] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));
	vec2 pos = pc.rect.xy + unit[gl_VertexIndex] * pc.rect.zw;
	gl_Position = vec4(pos, 0.0, 1.0);
	outColor = pc.color;
}
