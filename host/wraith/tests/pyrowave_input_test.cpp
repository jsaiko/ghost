// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// PyrowaveEncoder's two ways in must put the same pixels in front of the
// encode: push_cpu() with host rows, and push() with a dmabuf -- linear,
// and in each of the GPU's own tiled layouts as a compositor would hand it
// over, drawn there by a GL blit that is only flushed, never waited for.
// Any mistake in the import's layout or the copy shows up as different
// pixels in the input image (read_back_input()). (It doesn't prove the
// copy's wait on the dmabuf's sync file: amdgpu already orders work on a
// shared buffer by itself.) The bitstreams themselves can't be compared:
// PyroWave's isn't a pure function of the pixels -- the same frame
// encoded twice differs in a fraction of a percent of its bytes.
//
// Needs a render node whose Vulkan driver imports dmabufs
// (WRAITH_TEST_RENDER_NODE, default /dev/dri/renderD128); without one it
// skips (exit 77).
#include "encode/pyrowave/pyrowave_encoder.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <libdrm/drm_fourcc.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(expr)                                                                                          \
	do {                                                                                                     \
		if (!(expr)) {                                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                         \
			g_failures++;                                                                                    \
		}                                                                                                    \
	} while (0)

constexpr int kSkip = 77;
constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;

// Gradients, edges and some noise: enough structure that a shifted, swapped
// or half-copied frame encodes differently.
std::vector<uint8_t> test_pixels(uint32_t seed) {
	std::vector<uint8_t> pixels((size_t)kWidth * kHeight * 4);
	for (uint32_t y = 0; y < kHeight; y++) {
		for (uint32_t x = 0; x < kWidth; x++) {
			seed = seed * 1664525u + 1013904223u;
			uint8_t *p = &pixels[((size_t)y * kWidth + x) * 4];
			p[0] = (uint8_t)(x * 255 / kWidth);                           // B
			p[1] = (uint8_t)(((x / 64 + y / 64) & 1) ? 200 : 40);         // G
			p[2] = (uint8_t)((y * 255 / kHeight) ^ ((seed >> 28) & 0x7)); // R
			p[3] = 0xff;
		}
	}
	return pixels;
}

struct Gl {
	EGLDisplay display = EGL_NO_DISPLAY;
	PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target = nullptr;
};

struct Bo {
	gbm_bo *bo = nullptr;
	wraith::DmabufFrame frame;
	EGLImageKHR image = EGL_NO_IMAGE_KHR;
	GLuint texture = 0, framebuffer = 0;
};

bool make_bo(gbm_device *gbm, const uint64_t *modifier, Bo *out) {
	out->bo = modifier
		? gbm_bo_create_with_modifiers2(gbm, kWidth, kHeight, GBM_FORMAT_XRGB8888, modifier, 1,
			  GBM_BO_USE_RENDERING)
		: gbm_bo_create(gbm, kWidth, kHeight, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
	if (!out->bo) {
		return false;
	}
	wraith::DmabufFrame &f = out->frame;
	f.width = kWidth;
	f.height = kHeight;
	f.format = DRM_FORMAT_XRGB8888;
	f.modifier = gbm_bo_get_modifier(out->bo);
	if (!modifier && f.modifier == DRM_FORMAT_MOD_INVALID) {
		f.modifier = DRM_FORMAT_MOD_LINEAR; // asked for linear, and gbm won't say
	}
	f.n_planes = gbm_bo_get_plane_count(out->bo);
	for (int i = 0; i < f.n_planes && i < wraith::DmabufFrame::kMaxPlanes; i++) {
		f.fd[i] = gbm_bo_get_fd_for_plane(out->bo, i);
		f.offset[i] = gbm_bo_get_offset(out->bo, i);
		f.stride[i] = gbm_bo_get_stride_for_plane(out->bo, i);
	}
	return f.fd[0] >= 0;
}

bool fill_linear(Bo *bo, const std::vector<uint8_t> &pixels) {
	uint32_t stride = 0;
	void *map_data = nullptr;
	auto *map = static_cast<uint8_t *>(
		gbm_bo_map(bo->bo, 0, 0, kWidth, kHeight, GBM_BO_TRANSFER_WRITE, &stride, &map_data));
	if (!map) {
		return false;
	}
	for (uint32_t y = 0; y < kHeight; y++) {
		std::copy_n(&pixels[(size_t)y * kWidth * 4], kWidth * 4, map + (size_t)y * stride);
	}
	gbm_bo_unmap(bo->bo, map_data);
	return true;
}

bool gl_import(Gl &gl, Bo *bo) {
	const wraith::DmabufFrame &f = bo->frame;
	EGLint attribs[] = {EGL_WIDTH, f.width, EGL_HEIGHT, f.height, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)f.format,
		EGL_DMA_BUF_PLANE0_FD_EXT, f.fd[0], EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)f.offset[0],
		EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)f.stride[0], EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
		(EGLint)(f.modifier & 0xffffffff), EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(f.modifier >> 32),
		EGL_NONE};
	bo->image = gl.create_image(gl.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
	if (bo->image == EGL_NO_IMAGE_KHR) {
		return false;
	}
	glGenTextures(1, &bo->texture);
	glBindTexture(GL_TEXTURE_2D, bo->texture);
	gl.image_target(GL_TEXTURE_2D, bo->image);
	glGenFramebuffers(1, &bo->framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, bo->framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bo->texture, 0);
	return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void release(Gl &gl, Bo *bo) {
	if (bo->framebuffer) {
		glDeleteFramebuffers(1, &bo->framebuffer);
	}
	if (bo->texture) {
		glDeleteTextures(1, &bo->texture);
	}
	if (bo->image != EGL_NO_IMAGE_KHR) {
		gl.destroy_image(gl.display, bo->image);
	}
	for (int i = 0; i < bo->frame.n_planes && i < wraith::DmabufFrame::kMaxPlanes; i++) {
		if (bo->frame.fd[i] >= 0) {
			close(bo->frame.fd[i]);
		}
	}
	if (bo->bo) {
		gbm_bo_destroy(bo->bo);
	}
	*bo = Bo{};
}

// Whether the last push encoded one non-empty packet, and its input image
// held exactly `pixels`' colour (the X/alpha byte is nobody's business).
bool encoded(wraith::PyrowaveEncoder &encoder, const std::vector<uint8_t> &pixels) {
	std::vector<wraith::EncodedPacket> out = encoder.poll();
	if (out.size() != 1 || out.front().data.empty()) {
		fprintf(stderr, "expected one packet, got %zu\n", out.size());
		return false;
	}
	std::vector<uint8_t> input;
	if (!encoder.read_back_input(&input) || input.size() != pixels.size()) {
		fprintf(stderr, "couldn't read the input image back\n");
		return false;
	}
	size_t differ = 0, first = SIZE_MAX;
	for (size_t i = 0; i < pixels.size(); i++) {
		if (i % 4 != 3 && input[i] != pixels[i]) {
			differ++;
			first = std::min(first, i);
		}
	}
	if (differ) {
		fprintf(stderr, "%zu bytes of the input image differ, first at pixel (%zu, %zu)\n", differ,
			(first / 4) % kWidth, (first / 4) / kWidth);
	}
	return differ == 0;
}

int run(int fd, gbm_device *gbm, Gl &gl) {
	wraith::PyrowaveEncoder encoder;
	wraith::EncoderConfig config;
	config.codec = gdp::VideoCodec::Pyrowave;
	config.width = kWidth;
	config.height = kHeight;
	config.bitrate_bps = 400'000'000;
	config.drm_fd = fd;
	if (!encoder.open(config)) {
		printf("pyrowave_input_test: no PyroWave device on this render node, skipped\n");
		return kSkip;
	}
	if (encoder.wants_cpu_frame()) {
		printf("pyrowave_input_test: the Vulkan driver can't import dmabufs, skipped\n");
		return kSkip;
	}
	std::vector<uint64_t> modifiers = encoder.supported_import_modifiers(DRM_FORMAT_XRGB8888);
	CHECK(!modifiers.empty());
	CHECK(encoder.supported_import_modifiers(DRM_FORMAT_NV12).empty());

	std::vector<uint8_t> pixels = test_pixels(1);
	int64_t pts = 0;

	// A modifier the encoder didn't offer is refused before anything is
	// submitted (one it can't describe would hang the GPU), and the encoder
	// then asks for read-back frames -- checked on a throwaway encoder.
	{
		wraith::PyrowaveEncoder refuser;
		CHECK(refuser.open(config));
		wraith::DmabufFrame bogus;
		bogus.width = kWidth;
		bogus.height = kHeight;
		bogus.format = DRM_FORMAT_XRGB8888;
		bogus.modifier = DRM_FORMAT_MOD_INVALID;
		bogus.n_planes = 1;
		bogus.fd[0] = -1;
		CHECK(!refuser.push(bogus, 1));
		CHECK(refuser.wants_cpu_frame());
		CHECK(refuser.supported_import_modifiers(DRM_FORMAT_XRGB8888).empty());
		refuser.close();
	}
	std::vector<uint8_t> other = test_pixels(2);
	CHECK(encoder.push_cpu(pixels.data(), kWidth, kHeight, kWidth * 4, pts += 16667, nullptr));
	CHECK(encoded(encoder, pixels));

	Bo linear;
	CHECK(make_bo(gbm, nullptr, &linear) && fill_linear(&linear, pixels));
	bool linear_importable =
		std::find(modifiers.begin(), modifiers.end(), DRM_FORMAT_MOD_LINEAR) != modifiers.end();
	if (linear_importable) {
		CHECK(encoder.push(linear.frame, pts += 16667));
		CHECK(encoded(encoder, pixels));
	}

	// The same pixels in each tiled layout the encoder offers, drawn there
	// by GL and handed over unfinished.
	CHECK(gl_import(gl, &linear));
	int tiled = 0;
	for (uint64_t modifier : modifiers) {
		if (modifier == DRM_FORMAT_MOD_LINEAR) {
			continue;
		}
		Bo bo;
		if (!make_bo(gbm, &modifier, &bo) || !gl_import(gl, &bo)) {
			release(gl, &bo); // gbm or GL can't make this one; not the encoder's business
			continue;
		}
		glBindFramebuffer(GL_READ_FRAMEBUFFER, linear.framebuffer);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bo.framebuffer);
		glBlitFramebuffer(0, 0, kWidth, kHeight, 0, 0, kWidth, kHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glFlush();
		bool pushed = encoder.push(bo.frame, pts += 16667);
		CHECK(pushed);
		if (!encoded(encoder, pixels)) {
			fprintf(stderr, "modifier 0x%016llx: the dmabuf didn't reach the encode as drawn\n",
				(unsigned long long)modifier);
			g_failures++;
		}
		// Host rows in between, then the same buffer again: from the cached
		// import this time.
		CHECK(encoder.push_cpu(other.data(), kWidth, kHeight, kWidth * 4, pts += 16667, nullptr));
		CHECK(encoded(encoder, other));
		CHECK(encoder.push(bo.frame, pts += 16667));
		CHECK(encoded(encoder, pixels));
		release(gl, &bo);
		tiled++;
	}
	CHECK(tiled > 0);
	CHECK(!encoder.wants_cpu_frame()); // nothing failed along the way
	release(gl, &linear);
	encoder.close();
	printf("pyrowave_input_test: %s (%d tiled layout(s)%s)\n", g_failures ? "FAILED" : "ok", tiled,
		linear_importable ? " + linear" : "");
	return g_failures ? 1 : 0;
}

} // namespace

int main() {
	const char *node = getenv("WRAITH_TEST_RENDER_NODE");
	if (!node) {
		node = "/dev/dri/renderD128";
	}
	int fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("pyrowave_input_test: no %s, skipped\n", node);
		return kSkip;
	}
	gbm_device *gbm = gbm_create_device(fd);
	Gl gl;
	auto get_display =
		reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
	gl.display = gbm && get_display ? get_display(EGL_PLATFORM_GBM_KHR, gbm, nullptr) : EGL_NO_DISPLAY;
	EGLContext context = EGL_NO_CONTEXT;
	if (gl.display != EGL_NO_DISPLAY && eglInitialize(gl.display, nullptr, nullptr)) {
		eglBindAPI(EGL_OPENGL_ES_API);
		const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
		context = eglCreateContext(gl.display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs);
	}
	if (context == EGL_NO_CONTEXT || !eglMakeCurrent(gl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
		printf("pyrowave_input_test: no GLES 3 on %s, skipped\n", node);
		return kSkip;
	}
	gl.create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
	gl.destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
	gl.image_target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
		eglGetProcAddress("glEGLImageTargetTexture2DOES"));
	int result = run(fd, gbm, gl);
	eglMakeCurrent(gl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(gl.display, context);
	eglTerminate(gl.display);
	gbm_device_destroy(gbm);
	close(fd);
	return result;
}
