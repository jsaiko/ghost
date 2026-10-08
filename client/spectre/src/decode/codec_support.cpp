// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "decode/codec_support.hpp"

#include "present/vulkan_device.hpp"
#include "log.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <d3d11.h>
#include <dxgi1_2.h>
#elif defined(__APPLE__)
#include <VideoToolbox/VideoToolbox.h>
#else
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#endif

namespace spectre {

namespace {

// One bit per gdp::VideoCodec a hardware decoder can take.
using CodecSet = unsigned;

CodecSet codec_bit(gdp::VideoCodec codec) {
	switch (codec) {
	case gdp::VideoCodec::H264: return 1u << 0;
	case gdp::VideoCodec::H265: return 1u << 1;
	case gdp::VideoCodec::AV1: return 1u << 2;
	case gdp::VideoCodec::Pyrowave: return 1u << 3;
	case gdp::VideoCodec::Unknown: break;
	}
	return 0;
}

CodecSet vulkan_codecs(VkVideoCodecOperationFlagsKHR ops) {
	CodecSet set = 0;
	if (ops & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
		set |= codec_bit(gdp::VideoCodec::H264);
	}
	if (ops & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) {
		set |= codec_bit(gdp::VideoCodec::H265);
	}
	if (ops & VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) {
		set |= codec_bit(gdp::VideoCodec::AV1);
	}
	return set;
}

#ifdef _WIN32
// D3D11VA on the adapter behind `device`, as Decoder::create_native_device()
// would open it: the decoder profiles FFmpeg's d3d11va hwaccels use, each
// with the NV12 output they decode to.
CodecSet native_codecs(VkPhysicalDevice device) {
	VkPhysicalDeviceIDProperties id_props{};
	id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
	VkPhysicalDeviceProperties2 props2{};
	props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	props2.pNext = &id_props;
	vkGetPhysicalDeviceProperties2(device, &props2);

	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory))) {
		return 0;
	}
	IDXGIAdapter1 *match = nullptr;
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT i = 0; !match && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 desc{};
		if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
			memcmp(&desc.AdapterLuid, id_props.deviceLUID, sizeof(desc.AdapterLuid)) == 0) {
			match = adapter;
		} else {
			adapter->Release();
		}
	}
	factory->Release();
	if (!match) {
		return 0;
	}

	ID3D11Device *d3d = nullptr;
	HRESULT hr = D3D11CreateDevice(match, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
		nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
	match->Release();
	if (FAILED(hr)) {
		return 0;
	}
	ID3D11VideoDevice *video = nullptr;
	hr = d3d->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)&video);
	d3d->Release();
	if (FAILED(hr)) {
		return 0;
	}

	struct Profile {
		const GUID *guid;
		gdp::VideoCodec codec;
	};
	static const Profile kProfiles[] = {
		{&D3D11_DECODER_PROFILE_H264_VLD_NOFGT, gdp::VideoCodec::H264},
		{&D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, gdp::VideoCodec::H265},
		{&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, gdp::VideoCodec::AV1},
	};
	CodecSet set = 0;
	UINT count = video->GetVideoDecoderProfileCount();
	for (UINT i = 0; i < count; i++) {
		GUID guid{};
		if (FAILED(video->GetVideoDecoderProfile(i, &guid))) {
			continue;
		}
		for (const Profile &p : kProfiles) {
			BOOL nv12 = FALSE;
			if (guid == *p.guid &&
				SUCCEEDED(video->CheckVideoDecoderFormat(&guid, DXGI_FORMAT_NV12, &nv12)) && nv12) {
				set |= codec_bit(p.codec);
			}
		}
	}
	video->Release();
	return set;
}
#elif defined(__APPLE__)
// VideoToolbox, as Decoder::create_native_device() would open it: FFmpeg's
// hwaccel asks for a hardware decoder and fails over to software decode
// without one, so only what the Mac's media engine does counts. (Every Mac
// decodes H.264 in hardware; HEVC needs a 2015+ Intel GPU or Apple
// silicon; AV1 an M3 or later.)
CodecSet native_codecs() {
	struct Codec {
		CMVideoCodecType type;
		gdp::VideoCodec codec;
	};
	static const Codec kCodecs[] = {
		{kCMVideoCodecType_H264, gdp::VideoCodec::H264},
		{kCMVideoCodecType_HEVC, gdp::VideoCodec::H265},
		// kCMVideoCodecType_AV1 only exists in the macOS 14 SDK and later.
		{'av01', gdp::VideoCodec::AV1},
	};
	CodecSet set = 0;
	for (const Codec &c : kCodecs) {
		if (VTIsHardwareDecodeSupported(c.type)) {
			set |= codec_bit(c.codec);
		}
	}
	return set;
}
#else
// VA-API on `drm_render_node`, as Decoder::create_native_device() would open
// it: a VLD (decode) entrypoint on one of the profiles wraith encodes to.
CodecSet native_codecs(const char *drm_render_node) {
	int fd = open(drm_render_node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		return 0;
	}
	CodecSet set = 0;
	VADisplay display = vaGetDisplayDRM(fd);
	int major = 0;
	int minor = 0;
	if (display && vaInitialize(display, &major, &minor) == VA_STATUS_SUCCESS) {
		struct Profile {
			VAProfile profile;
			gdp::VideoCodec codec;
		};
		static const Profile kProfiles[] = {
			{VAProfileH264ConstrainedBaseline, gdp::VideoCodec::H264},
			{VAProfileH264Main, gdp::VideoCodec::H264},
			{VAProfileH264High, gdp::VideoCodec::H264},
			{VAProfileHEVCMain, gdp::VideoCodec::H265},
			{VAProfileAV1Profile0, gdp::VideoCodec::AV1},
		};
		int profile_count = vaMaxNumProfiles(display);
		std::vector<VAProfile> profiles((size_t)profile_count);
		std::vector<VAEntrypoint> entrypoints((size_t)vaMaxNumEntrypoints(display));
		if (vaQueryConfigProfiles(display, profiles.data(), &profile_count) == VA_STATUS_SUCCESS) {
			for (int i = 0; i < profile_count; i++) {
				for (const Profile &p : kProfiles) {
					if (profiles[i] != p.profile) {
						continue;
					}
					int entrypoint_count = 0;
					if (vaQueryConfigEntrypoints(display, p.profile, entrypoints.data(), &entrypoint_count) !=
						VA_STATUS_SUCCESS) {
						continue;
					}
					for (int e = 0; e < entrypoint_count; e++) {
						if (entrypoints[e] == VAEntrypointVLD) {
							set |= codec_bit(p.codec);
						}
					}
				}
			}
		}
		vaTerminate(display);
	}
	close(fd);
	return set;
}
#endif

// The hardware half: what Vulkan Video and the platform decoder can each
// take on the GPU the presenter will land on. Mirrors the gates
// StreamSession::open_window() and Decoder::open() apply to the real thing.
void probe_hardware(const char *drm_render_node, CodecSet *vulkan, CodecSet *native, CodecSet *compute) {
	*vulkan = 0;
	*native = 0;
	*compute = 0;

	VkApplicationInfo app_info{};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "spectre";
	app_info.apiVersion = VK_API_VERSION_1_3;
	std::vector<const char *> instance_extensions;
	VkInstanceCreateInfo instance_info{};
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.flags = VulkanDevice::portability_instance_setup(&instance_extensions);
	instance_info.pApplicationInfo = &app_info;
	instance_info.enabledExtensionCount = (uint32_t)instance_extensions.size();
	instance_info.ppEnabledExtensionNames = instance_extensions.data();
	VkInstance instance = VK_NULL_HANDLE;
	if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
		SLOG_INFO("codec_support: no Vulkan instance, assuming software decode only");
		return;
	}

	bool matched_render_node = false;
	VkPhysicalDevice device =
		VulkanDevice::choose_physical_device(instance, drm_render_node, &matched_render_node);
	if (device) {
#ifdef SPECTRE_HAVE_PYROWAVE
		// PyroWave's compute decode (pyrowave_decode.hpp), whose gate
		// VulkanDevice::create_device() applies too.
		if (VulkanDevice::supports_pyrowave(device)) {
			*compute = codec_bit(gdp::VideoCodec::Pyrowave);
		}
#endif
#ifdef SPECTRE_VULKAN_DECODE
		uint32_t decode_family = 0;
		VkVideoCodecOperationFlagsKHR decode_ops = 0;
		std::vector<const char *> extensions;
		if (VulkanDevice::probe_video_decode(device, &decode_family, &decode_ops, &extensions)) {
			*vulkan = vulkan_codecs(decode_ops);
		}
#endif
		if (VulkanDevice::can_import_native_frames(device, matched_render_node)) {
#if defined(_WIN32)
			*native = native_codecs(device);
#elif defined(__APPLE__)
			*native = native_codecs();
#else
			// NVIDIA has no VA-API of its own (see open_window()).
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(device, &props);
			constexpr uint32_t kNvidiaVendorId = 0x10de;
			if (props.vendorID != kNvidiaVendorId) {
				*native = native_codecs(drm_render_node);
			}
#endif
		}
	}
	vkDestroyInstance(instance, nullptr);
}

// spectre -X's token for the platform decoder.
#if defined(_WIN32)
constexpr const char *kNativeDecodeToken = "d3d11va";
#elif defined(__APPLE__)
constexpr const char *kNativeDecodeToken = "videotoolbox";
#else
constexpr const char *kNativeDecodeToken = "vaapi";
#endif

} // namespace

bool v4l2_m2m_decodes(gdp::VideoCodec codec) {
#if defined(__linux__)
	uint32_t want = codec == gdp::VideoCodec::H264 ? V4L2_PIX_FMT_H264
		: codec == gdp::VideoCodec::H265           ? V4L2_PIX_FMT_HEVC
												   : 0;
	if (!want) {
		return false;
	}
	for (int n = 0; n < 64; n++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/video%d", n);
		int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			continue;
		}
		v4l2_capability cap{};
		bool found = false;
		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
			uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
			// The coded side is the OUTPUT queue of a memory-to-memory device.
			for (v4l2_buf_type type : {V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_BUF_TYPE_VIDEO_OUTPUT}) {
				bool m2m = type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? (caps & V4L2_CAP_VIDEO_M2M_MPLANE)
																	 : (caps & V4L2_CAP_VIDEO_M2M);
				v4l2_fmtdesc desc{};
				desc.type = type;
				while (m2m && !found && ioctl(fd, VIDIOC_ENUM_FMT, &desc) == 0) {
					found = desc.pixelformat == want;
					desc.index++;
				}
			}
		}
		close(fd);
		if (found) {
			return true;
		}
	}
	return false;
#else
	(void)codec;
	return false;
#endif
}

std::vector<std::string> probe_decode_paths(const char *drm_render_node) {
	CodecSet vulkan = 0;
	CodecSet native = 0;
	CodecSet compute = 0;
	probe_hardware(drm_render_node, &vulkan, &native, &compute);
	std::vector<std::string> result;
	for (const std::string &token : gdp::all_video_codec_tokens()) {
		if (!Decoder::can_decode(token)) {
			continue;
		}
		CodecSet bit = codec_bit(gdp::video_codec_from_token(token));
		if (vulkan & bit) {
			result.push_back("vulkan:" + token);
		}
		if (native & bit) {
			result.push_back(std::string(kNativeDecodeToken) + ":" + token);
		}
		if (compute & bit) {
			result.push_back("compute:" + token);
		}
		if (v4l2_m2m_decodes(gdp::video_codec_from_token(token))) {
			result.push_back("v4l2:" + token);
		}
		if (Decoder::can_decode_in_software(token)) {
			result.push_back("software:" + token);
		}
	}
	return result;
}

std::vector<std::string> probe_decodable_codecs(DecodeBackend backend, const char *drm_render_node) {
	CodecSet vulkan = 0;
	CodecSet native = 0;
	CodecSet compute = 0;
	if (backend != DecodeBackend::Software) {
		probe_hardware(drm_render_node, &vulkan, &native, &compute);
	}

	std::vector<std::string> result;
	for (const std::string &token : gdp::all_video_codec_tokens()) {
		// Every path decodes through FFmpeg's decoder for the codec, the
		// hardware ones included (as its hwaccel) -- except pyrowave, which
		// is its own (can_decode() says whether it's built in).
		if (!Decoder::can_decode(token)) {
			continue;
		}
		gdp::VideoCodec codec = gdp::video_codec_from_token(token);
		CodecSet bit = codec_bit(codec);
		// Software decode only ever covers h264 (see can_decode_in_software()).
		bool software = Decoder::can_decode_in_software(token);
		bool on_vulkan = (vulkan & bit) != 0;
		bool on_native = (native & bit) != 0;
		bool on_compute = (compute & bit) != 0;
		bool on_v4l2 = backend != DecodeBackend::Software && v4l2_m2m_decodes(codec);
		SLOG_DEBUG("codec_support: %s -- software %s, vulkan %s, %s %s, compute %s, v4l2 %s", token.c_str(),
			software ? "yes" : "no", on_vulkan ? "yes" : "no", kNativeDecodeName, on_native ? "yes" : "no",
			on_compute ? "yes" : "no", on_v4l2 ? "yes" : "no");
		if (software || on_vulkan || on_native || on_compute || on_v4l2) {
			result.push_back(token);
		}
	}
	return result;
}

} // namespace spectre
