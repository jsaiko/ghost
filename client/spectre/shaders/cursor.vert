// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#version 450

// Same positioned-quad trick as rect.vert (triangle strip, 4 vertices, no
// vertex buffer), but also emits UVs for cursor.frag to sample a texture
// with. Shared by the cursor, the splash bitmap and the lossless
// refinement plane.
layout(push_constant) uniform PushConstants {
	vec4 rect; // x0, y0, width, height, all in NDC units
} pc;

layout(location = 0) out vec2 outUV;

void main() {
	vec2 unit[4] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));
	vec2 pos = pc.rect.xy + unit[gl_VertexIndex] * pc.rect.zw;
	gl_Position = vec4(pos, 0.0, 1.0);
	outUV = unit[gl_VertexIndex];
}
