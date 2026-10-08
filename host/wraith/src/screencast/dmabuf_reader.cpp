// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/dmabuf_reader.hpp"

#include "util/log.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <libdrm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

namespace wraith {

namespace {

// encode/refine/tile_source.cpp's hash_tile(), one invocation per tile.
// The frame is sampled as the BGRA it is in memory, X byte 1.0 = 0xff --
// exactly what read() hands a CPU hash -- and the 64-bit FNV arithmetic is
// done in 32-bit halves: GLSL ES has no 64-bit integers, and the prime,
// 0x100000001b3, is (0x1b3, 0x100) in halves.
//
// The offset bases come in as uniforms, not literals: Mesa 26's radeonsi
// (ACO, gfx12 both) computes (literal ^ x) * k wrongly here, while a
// uniform holding the same value is right. tests/gpu_tile_hash_test.cpp
// holds this to the CPU hash bit for bit.
const char *kHashShader = R"(#version 310 es
layout(local_size_x = 64) in;
precision highp float;
precision highp int;
layout(binding = 0) uniform highp sampler2D frame;
layout(std430, binding = 0) writeonly buffer Hashes { uvec2 hashes[]; };
uniform ivec2 frame_size;
uniform int tile_size;
uniform int cols;
uniform int tiles;
uniform uvec2 seeds[4];

uvec2 fnv(uvec2 h, uvec2 c) {
	h ^= c;
	uint lo, hi;
	umulExtended(h.x, 0x1b3u, hi, lo);
	hi += h.x * 0x100u + h.y * 0x1b3u;
	return uvec2(lo, hi);
}

uint px(ivec2 p) {
	return packUnorm4x8(texelFetch(frame, p, 0).bgra);
}

void main() {
	int t = int(gl_GlobalInvocationID.x);
	if (t >= tiles) {
		return;
	}
	int x = (t % cols) * tile_size;
	int y = (t / cols) * tile_size;
	int w = min(tile_size, frame_size.x - x);
	int h = min(tile_size, frame_size.y - y);
	uvec2 h0 = seeds[0], h1 = seeds[1], h2 = seeds[2], h3 = seeds[3];
	int words = w / 2; // 8-byte words: two pixels each
	for (int row = 0; row < h; row++) {
		int i = 0;
		for (; i + 4 <= words; i += 4) {
			ivec2 p = ivec2(x + i * 2, y + row);
			h0 = fnv(h0, uvec2(px(p), px(p + ivec2(1, 0))));
			h1 = fnv(h1, uvec2(px(p + ivec2(2, 0)), px(p + ivec2(3, 0))));
			h2 = fnv(h2, uvec2(px(p + ivec2(4, 0)), px(p + ivec2(5, 0))));
			h3 = fnv(h3, uvec2(px(p + ivec2(6, 0)), px(p + ivec2(7, 0))));
		}
		for (; i < words; i++) {
			ivec2 p = ivec2(x + i * 2, y + row);
			h0 = fnv(h0, uvec2(px(p), px(p + ivec2(1, 0))));
		}
		if ((w & 1) != 0) { // an odd width's last pixel, a byte at a time
			uint v = px(ivec2(x + w - 1, y + row));
			for (int b = 0; b < 4; b++) {
				h0 = fnv(h0, uvec2((v >> (8 * b)) & 0xffu, 0u));
			}
		}
	}
	uvec2 r = h0;
	r = fnv(r, h1);
	r = fnv(r, h2);
	r = fnv(r, h3);
	hashes[t] = r;
}
)";

// Copies blocks of up to 16x16 pixels out of the frame into one packed
// buffer, one work group per block: (x | y << 16, w | h << 16, first
// destination word, words per destination row).
const char *kGatherShader = R"(#version 310 es
layout(local_size_x = 16, local_size_y = 16) in;
precision highp float;
precision highp int;
layout(binding = 0) uniform highp sampler2D frame;
layout(std430, binding = 1) readonly buffer Blocks { uvec4 blocks[]; };
layout(std430, binding = 2) writeonly buffer Pixels { uint pixels[]; };
uniform uint first_block;

void main() {
	uvec4 b = blocks[first_block + gl_WorkGroupID.x];
	uvec2 l = gl_LocalInvocationID.xy;
	if (l.x < (b.y & 0xffffu) && l.y < (b.y >> 16)) {
		ivec2 p = ivec2(int(b.x & 0xffffu) + int(l.x), int(b.x >> 16) + int(l.y));
		pixels[b.z + l.y * b.w + l.x] = packUnorm4x8(texelFetch(frame, p, 0).bgra);
	}
}
)";

GLuint compile_compute(const char *source, const char *name) {
	GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";
		glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
		WLOG_ERROR("dmabuf_reader: the %s shader doesn't compile: %s", name, log);
		glDeleteShader(shader);
		return 0;
	}
	GLuint program = glCreateProgram();
	glAttachShader(program, shader);
	glLinkProgram(program);
	glDeleteShader(shader);
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok) {
		WLOG_ERROR("dmabuf_reader: the %s shader doesn't link", name);
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

// Maps `bytes` of the bound shader storage buffer for reading once the
// dispatch before it has written them, copies them out and unmaps.
bool read_storage(size_t bytes, void *dst) {
	glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
	const void *mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT);
	if (!mapped) {
		return false;
	}
	memcpy(dst, mapped, bytes);
	return glUnmapBuffer(GL_SHADER_STORAGE_BUFFER) == GL_TRUE;
}

// Grows the shader storage buffer `buffer` (created on first use) to at
// least `bytes` and leaves it bound.
void reserve_storage(GLuint *buffer, size_t *capacity, size_t bytes, GLenum usage) {
	if (!*buffer) {
		glGenBuffers(1, buffer);
	}
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, *buffer);
	if (*capacity < bytes) {
		glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)bytes, nullptr, usage);
		*capacity = bytes;
	}
}

bool has_extension(const char *list, const char *name) {
	if (!list) {
		return false;
	}
	size_t len = strlen(name);
	for (const char *p = list; (p = strstr(p, name)) != nullptr; p += len) {
		if ((p == list || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) {
			return true;
		}
	}
	return false;
}

} // namespace

struct DmabufReader::Impl {
	struct gbm_device *gbm = nullptr;
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLContext context = EGL_NO_CONTEXT;
	bool have_modifiers = false;

	PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture = nullptr;
	PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers = nullptr;

	// One import per capture buffer: PipeWire hands the same few buffers
	// round and round, so each is imported once, not once a frame.
	struct Import {
		EGLImageKHR image = EGL_NO_IMAGE_KHR;
		GLuint texture = 0;
		GLuint framebuffer = 0;
		int32_t width = 0, height = 0;
		uint32_t format = 0;
		uint64_t modifier = 0;
		int fd = -1;
		uint32_t offset = 0, stride = 0;
	};
	std::map<void *, Import> imports;
	bool logged_failure = false;

	// Every entry point that touches GL calls this first. The context is
	// made current once at open(), but something else on the thread can
	// unbind it: on NVIDIA, destroying a Vulkan device (NVENC's dmabuf
	// importer, on every encoder reopen) leaves no EGL context current.
	bool make_current() {
		if (context == EGL_NO_CONTEXT) {
			return false;
		}
		if (eglGetCurrentContext() == context) {
			return true;
		}
		if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
			if (!logged_failure) {
				WLOG_ERROR("dmabuf_reader: eglMakeCurrent failed (0x%x)", eglGetError());
				logged_failure = true;
			}
			return false;
		}
		return true;
	}

	// GPU tile hashing (can_hash_tiles()): both programs, or neither.
	GLuint hash_program = 0;
	GLuint gather_program = 0;
	GLint hash_frame_size = -1, hash_tile_size = -1, hash_cols = -1, hash_tiles = -1;
	GLint gather_first_block = -1;
	GLuint hash_buffer = 0, block_buffer = 0, pixel_buffer = 0;
	size_t hash_capacity = 0, block_capacity = 0, pixel_capacity = 0;
	std::vector<uint32_t> blocks; // gather's descriptors, kept to avoid reallocating

	bool build_tile_programs() {
		hash_program = compile_compute(kHashShader, "tile hash");
		gather_program = compile_compute(kGatherShader, "tile gather");
		if (!hash_program || !gather_program) {
			return false;
		}
		hash_frame_size = glGetUniformLocation(hash_program, "frame_size");
		hash_tile_size = glGetUniformLocation(hash_program, "tile_size");
		hash_cols = glGetUniformLocation(hash_program, "cols");
		hash_tiles = glGetUniformLocation(hash_program, "tiles");
		gather_first_block = glGetUniformLocation(gather_program, "first_block");
		// The FNV offset bases (see kHashShader), set once.
		constexpr uint64_t kBasis = 1469598103934665603ull;
		const uint64_t seeds[4] = {kBasis, kBasis ^ 0x1111111111111111ull, kBasis ^ 0x2222222222222222ull,
			kBasis ^ 0x3333333333333333ull};
		GLuint halves[8];
		for (int i = 0; i < 4; i++) {
			halves[2 * i] = (GLuint)seeds[i];
			halves[2 * i + 1] = (GLuint)(seeds[i] >> 32);
		}
		glUseProgram(hash_program);
		glUniform2uiv(glGetUniformLocation(hash_program, "seeds"), 4, halves);
		glUseProgram(0);
		return glGetError() == GL_NO_ERROR;
	}

	void release_tile_programs() {
		GLuint buffers[] = {hash_buffer, block_buffer, pixel_buffer};
		glDeleteBuffers(3, buffers);
		if (hash_program) {
			glDeleteProgram(hash_program);
		}
		if (gather_program) {
			glDeleteProgram(gather_program);
		}
		hash_program = gather_program = 0;
		hash_buffer = block_buffer = pixel_buffer = 0;
	}

	// The cached import for `token`, re-imported if the buffer's parameters
	// changed (renegotiation); the usual case is a cache hit.
	// Waits (on the CPU, bounded) for the producer's writes still pending
	// on the buffer: a compositor may hand a frame over before its GPU has
	// finished the copy into it. NVIDIA's GL doesn't wait on a dmabuf's
	// fences by itself, so without this the tile hashes could see the
	// buffer's previous contents. Immediate when there is nothing pending,
	// or on a kernel without the ioctl (before 6.0).
	static void wait_for_producer(int dmabuf_fd) {
#ifdef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
		struct dma_buf_export_sync_file request = {};
		request.flags = DMA_BUF_SYNC_READ;
		request.fd = -1;
		if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request) != 0 || request.fd < 0) {
			return;
		}
		struct pollfd pfd = {request.fd, POLLIN, 0};
		poll(&pfd, 1, 100);
		::close(request.fd);
#else
		(void)dmabuf_fd;
#endif
	}

	Import *find_import(const DmabufFrame &frame, void *token) {
		wait_for_producer(frame.fd[0]);
		auto it = imports.find(token);
		if (it != imports.end() &&
			(it->second.width != frame.width || it->second.height != frame.height ||
				it->second.format != frame.format || it->second.modifier != frame.modifier ||
				it->second.fd != frame.fd[0] || it->second.offset != frame.offset[0] ||
				it->second.stride != frame.stride[0])) {
			release(it->second);
			imports.erase(it);
			it = imports.end();
		}
		if (it == imports.end()) {
			Import import;
			if (!import_frame(frame, &import)) {
				return nullptr;
			}
			it = imports.emplace(token, import).first;
		}
		return &it->second;
	}

	void log_failure(const char *what, GLenum err) {
		if (!logged_failure) {
			WLOG_ERROR("dmabuf_reader: %s failed (0x%x)", what, err);
			logged_failure = true;
		}
	}

	void release(Import &import) {
		if (import.framebuffer) {
			glDeleteFramebuffers(1, &import.framebuffer);
		}
		if (import.texture) {
			glDeleteTextures(1, &import.texture);
		}
		if (import.image != EGL_NO_IMAGE_KHR) {
			destroy_image(display, import.image);
		}
		import = Import{};
	}

	bool import_frame(const DmabufFrame &frame, Import *out) {
		EGLint attribs[64];
		int n = 0;
		auto add = [&](EGLint key, EGLint value) {
			attribs[n++] = key;
			attribs[n++] = value;
		};
		add(EGL_WIDTH, frame.width);
		add(EGL_HEIGHT, frame.height);
		add(EGL_LINUX_DRM_FOURCC_EXT, (EGLint)frame.format);
		static const EGLint fd_keys[] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
			EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
		static const EGLint offset_keys[] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
			EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
		static const EGLint pitch_keys[] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
			EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
		static const EGLint mod_lo_keys[] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
			EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
			EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
		static const EGLint mod_hi_keys[] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
			EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
			EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
		for (int i = 0; i < frame.n_planes && i < DmabufFrame::kMaxPlanes; i++) {
			add(fd_keys[i], frame.fd[i]);
			add(offset_keys[i], (EGLint)frame.offset[i]);
			add(pitch_keys[i], (EGLint)frame.stride[i]);
			if (have_modifiers && frame.modifier != DRM_FORMAT_MOD_INVALID) {
				add(mod_lo_keys[i], (EGLint)(frame.modifier & 0xffffffff));
				add(mod_hi_keys[i], (EGLint)(frame.modifier >> 32));
			}
		}
		attribs[n++] = EGL_NONE;

		Import import;
		import.image = create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
		if (import.image == EGL_NO_IMAGE_KHR) {
			if (!logged_failure) {
				WLOG_ERROR(
					"dmabuf_reader: eglCreateImage failed (0x%x) for a %dx%d dmabuf, modifier 0x%016llx",
					eglGetError(), frame.width, frame.height, (unsigned long long)frame.modifier);
				logged_failure = true;
			}
			return false;
		}
		glGenTextures(1, &import.texture);
		glBindTexture(GL_TEXTURE_2D, import.texture);
		image_target_texture(GL_TEXTURE_2D, import.image);
		// texelFetch() (the tile shaders) reads nothing from a texture that
		// isn't complete, and the default minifying filter wants mipmaps.
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glGenFramebuffers(1, &import.framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, import.framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, import.texture, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			if (!logged_failure) {
				WLOG_ERROR("dmabuf_reader: an imported dmabuf can't be read as a framebuffer");
				logged_failure = true;
			}
			release(import);
			return false;
		}
		import.width = frame.width;
		import.height = frame.height;
		import.format = frame.format;
		import.modifier = frame.modifier;
		import.fd = frame.fd[0];
		import.offset = frame.offset[0];
		import.stride = frame.stride[0];
		*out = import;
		return true;
	}
};

DmabufReader::DmabufReader() : impl_(std::make_unique<Impl>()) {}

DmabufReader::~DmabufReader() {
	if (!impl_) {
		return;
	}
	forget_all();
	if (impl_->make_current()) {
		impl_->release_tile_programs();
	}
	if (impl_->display != EGL_NO_DISPLAY) {
		eglMakeCurrent(impl_->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (impl_->context != EGL_NO_CONTEXT) {
			eglDestroyContext(impl_->display, impl_->context);
		}
		eglTerminate(impl_->display);
	}
	if (impl_->gbm) {
		gbm_device_destroy(impl_->gbm);
	}
}

bool DmabufReader::is_open() const {
	return impl_->context != EGL_NO_CONTEXT;
}

bool DmabufReader::open(int drm_fd) {
	Impl &s = *impl_;
	if (drm_fd < 0) {
		return false;
	}
	const char *client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	if (!has_extension(client_exts, "EGL_KHR_platform_gbm") &&
		!has_extension(client_exts, "EGL_MESA_platform_gbm")) {
		WLOG_INFO("dmabuf_reader: EGL has no GBM platform");
		return false;
	}
	s.gbm = gbm_create_device(drm_fd);
	if (!s.gbm) {
		WLOG_ERROR("dmabuf_reader: gbm_create_device failed");
		return false;
	}
	auto get_platform_display =
		reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
	if (!get_platform_display) {
		return false;
	}
	s.display = get_platform_display(EGL_PLATFORM_GBM_KHR, s.gbm, nullptr);
	if (s.display == EGL_NO_DISPLAY || !eglInitialize(s.display, nullptr, nullptr)) {
		WLOG_ERROR("dmabuf_reader: EGL display init failed (0x%x)", eglGetError());
		s.display = EGL_NO_DISPLAY;
		return false;
	}
	const char *exts = eglQueryString(s.display, EGL_EXTENSIONS);
	if (!has_extension(exts, "EGL_EXT_image_dma_buf_import") ||
		!has_extension(exts, "EGL_KHR_surfaceless_context") ||
		!has_extension(exts, "EGL_KHR_no_config_context")) {
		WLOG_INFO("dmabuf_reader: EGL lacks dmabuf import or surfaceless/no-config contexts");
		return false;
	}
	s.have_modifiers = has_extension(exts, "EGL_EXT_image_dma_buf_import_modifiers");
	s.create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
	s.destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
	s.image_target_texture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
		eglGetProcAddress("glEGLImageTargetTexture2DOES"));
	if (s.have_modifiers) {
		s.query_modifiers = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
			eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
	}
	if (!s.create_image || !s.destroy_image || !s.image_target_texture) {
		return false;
	}

	eglBindAPI(EGL_OPENGL_ES_API);
	// GLES 3.1 for the tile shaders' compute; plain 2.0 still does the
	// read-back.
	const EGLint es31_attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
	const EGLint es2_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	bool es31 = true;
	EGLContext context = eglCreateContext(s.display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, es31_attribs);
	if (context == EGL_NO_CONTEXT) {
		es31 = false;
		context = eglCreateContext(s.display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, es2_attribs);
	}
	if (context == EGL_NO_CONTEXT) {
		WLOG_ERROR("dmabuf_reader: eglCreateContext failed (0x%x)", eglGetError());
		return false;
	}
	if (!eglMakeCurrent(s.display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
		WLOG_ERROR("dmabuf_reader: eglMakeCurrent failed (0x%x)", eglGetError());
		eglDestroyContext(s.display, context);
		return false;
	}
	// XRGB8888 is B, G, R, X in memory: read back as BGRA, it lands as is.
	const char *gl_exts = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
	if (!has_extension(gl_exts, "GL_EXT_read_format_bgra") || !has_extension(gl_exts, "GL_OES_EGL_image")) {
		WLOG_INFO("dmabuf_reader: GLES lacks BGRA read-back or EGL images");
		eglMakeCurrent(s.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroyContext(s.display, context);
		return false;
	}
	s.context = context;
	bool tiles = es31 && s.build_tile_programs();
	if (!tiles) {
		s.release_tile_programs();
	}
	WLOG_INFO("dmabuf_reader: reading captured dmabufs back on %s%s",
		reinterpret_cast<const char *>(glGetString(GL_RENDERER)),
		tiles ? ", tile hashing on the GPU" : (es31 ? "" : " (no GLES 3.1: tile hashing on the CPU)"));
	return true;
}

std::vector<uint64_t> DmabufReader::supported_modifiers(uint32_t drm_format) const {
	const Impl &s = *impl_;
	std::vector<uint64_t> out;
	if (!is_open()) {
		return out;
	}
	if (s.query_modifiers) {
		EGLint count = 0;
		if (s.query_modifiers(s.display, (EGLint)drm_format, 0, nullptr, nullptr, &count) && count > 0) {
			std::vector<EGLuint64KHR> mods((size_t)count);
			std::vector<EGLBoolean> external((size_t)count);
			if (s.query_modifiers(s.display, (EGLint)drm_format, count, mods.data(), external.data(),
					&count)) {
				for (EGLint i = 0; i < count; i++) {
					if (!external[(size_t)i]) { // external-only can't be a 2D texture
						out.push_back(mods[(size_t)i]);
					}
				}
			}
		}
	}
	if (out.empty()) {
		out.push_back(DRM_FORMAT_MOD_LINEAR);
	}
	return out;
}

bool DmabufReader::read(const DmabufFrame &frame, void *token, uint8_t *dst) {
	Impl &s = *impl_;
	if (!is_open() || frame.n_planes < 1 || !s.make_current()) {
		return false;
	}
	Impl::Import *import = s.find_import(frame, token);
	if (!import) {
		return false;
	}
	// A texture attached to a framebuffer reads in its own row order, top
	// first, so no flip.
	glBindFramebuffer(GL_FRAMEBUFFER, import->framebuffer);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, frame.width, frame.height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, dst);
	GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		s.log_failure("glReadPixels", err);
		return false;
	}
	return true;
}

bool DmabufReader::can_hash_tiles() const {
	return is_open() && impl_->hash_program != 0;
}

bool DmabufReader::hash_tiles(const DmabufFrame &frame, void *token, uint32_t tile_size, uint64_t *out) {
	Impl &s = *impl_;
	if (!can_hash_tiles() || frame.n_planes < 1 || tile_size == 0 || !s.make_current()) {
		return false;
	}
	Impl::Import *import = s.find_import(frame, token);
	if (!import) {
		return false;
	}
	const uint32_t cols = ((uint32_t)frame.width + tile_size - 1) / tile_size;
	const uint32_t rows = ((uint32_t)frame.height + tile_size - 1) / tile_size;
	const uint32_t tiles = cols * rows;
	const size_t bytes = (size_t)tiles * 8;

	reserve_storage(&s.hash_buffer, &s.hash_capacity, bytes, GL_STREAM_READ);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s.hash_buffer);
	glUseProgram(s.hash_program);
	glUniform2i(s.hash_frame_size, frame.width, frame.height);
	glUniform1i(s.hash_tile_size, (GLint)tile_size);
	glUniform1i(s.hash_cols, (GLint)cols);
	glUniform1i(s.hash_tiles, (GLint)tiles);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, import->texture);
	glDispatchCompute((tiles + 63) / 64, 1, 1);
	// Little-endian halves (lo, hi) land as the uint64_t they are.
	bool ok = read_storage(bytes, out);
	glUseProgram(0);
	GLenum err = glGetError();
	if (!ok || err != GL_NO_ERROR) {
		s.log_failure("tile hashing", err);
		return false;
	}
	return true;
}

bool DmabufReader::read_rects(const DmabufFrame &frame, void *token,
	const std::vector<gdp::RefineRect> &rects, std::vector<uint32_t> *out) {
	Impl &s = *impl_;
	if (!can_hash_tiles() || frame.n_planes < 1 || !s.make_current()) {
		return false;
	}
	// Each rect cut into blocks of at most 16x16, one work group each,
	// written where that block sits in the rect's packed rows.
	constexpr uint32_t kBlock = 16;
	s.blocks.clear();
	uint32_t words = 0;
	for (const gdp::RefineRect &rect : rects) {
		if (rect.x + rect.width > (uint32_t)frame.width || rect.y + rect.height > (uint32_t)frame.height) {
			return false;
		}
		for (uint32_t by = 0; by < rect.height; by += kBlock) {
			for (uint32_t bx = 0; bx < rect.width; bx += kBlock) {
				uint32_t w = std::min(kBlock, rect.width - bx);
				uint32_t h = std::min(kBlock, rect.height - by);
				s.blocks.push_back((rect.x + bx) | (rect.y + by) << 16);
				s.blocks.push_back(w | h << 16);
				s.blocks.push_back(words + by * rect.width + bx);
				s.blocks.push_back(rect.width);
			}
		}
		words += (uint32_t)rect.width * rect.height;
	}
	out->resize(words);
	if (words == 0) {
		return true;
	}
	Impl::Import *import = s.find_import(frame, token);
	if (!import) {
		return false;
	}

	reserve_storage(&s.block_buffer, &s.block_capacity, s.blocks.size() * 4, GL_STREAM_DRAW);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)(s.blocks.size() * 4), s.blocks.data());
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, s.block_buffer);
	reserve_storage(&s.pixel_buffer, &s.pixel_capacity, (size_t)words * 4, GL_STREAM_READ);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s.pixel_buffer);
	glUseProgram(s.gather_program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, import->texture);
	// 65535 work groups per dispatch is all GLES promises.
	constexpr uint32_t kMaxGroups = 65535;
	const uint32_t n_blocks = (uint32_t)(s.blocks.size() / 4);
	for (uint32_t first = 0; first < n_blocks; first += kMaxGroups) {
		glUniform1ui(s.gather_first_block, first);
		glDispatchCompute(std::min(kMaxGroups, n_blocks - first), 1, 1);
	}
	bool ok = read_storage((size_t)words * 4, out->data());
	glUseProgram(0);
	GLenum err = glGetError();
	if (!ok || err != GL_NO_ERROR) {
		s.log_failure("tile gather", err);
		return false;
	}
	return true;
}

void DmabufReader::forget(void *token) {
	auto it = impl_->imports.find(token);
	if (it != impl_->imports.end()) {
		impl_->make_current();
		impl_->release(it->second);
		impl_->imports.erase(it);
	}
}

void DmabufReader::forget_all() {
	if (!impl_->imports.empty()) {
		impl_->make_current();
	}
	for (auto &entry : impl_->imports) {
		impl_->release(entry.second);
	}
	impl_->imports.clear();
}

} // namespace wraith
