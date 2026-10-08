// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#version 450

// One glyph (or any textured UI quad): the positioned-quad trick from
// rect.vert/cursor.vert, plus a UV sub-rectangle so a single draw picks one
// cell out of the font atlas (ui/font.hpp's layout).
layout(push_constant) uniform PushConstants {
	vec4 rect;  // x0, y0, width, height in NDC
	vec4 uv;    // u0, v0, width, height in atlas texture space
	vec4 color; // rgba, straight alpha (text.frag premultiplies)
} pc;

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outColor;

void main() {
	vec2 unit[4] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));
	vec2 pos = pc.rect.xy + unit[gl_VertexIndex] * pc.rect.zw;
	gl_Position = vec4(pos, 0.0, 1.0);
	outUV = pc.uv.xy + unit[gl_VertexIndex] * pc.uv.zw;
	outColor = pc.color;
}
