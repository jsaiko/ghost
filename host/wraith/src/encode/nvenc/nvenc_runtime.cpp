// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/nvenc/nvenc_runtime.hpp"

#include "util/log.hpp"

#include <xf86drm.h>

#include <cstdio>
#include <memory>

namespace wraith {

namespace {

// PCI vendor ID for NVIDIA.
constexpr uint16_t kNvidiaVendorId = 0x10de;

// cuda_error_string() without going through nvenc_runtime(), which is
// still being initialised when load_runtime() needs it.
std::string error_string(CudaFunctions *cu, CUresult result) {
	const char *name = nullptr;
	const char *text = nullptr;
	if (cu) {
		cu->cuGetErrorName(result, &name);
		cu->cuGetErrorString(result, &text);
	}
	char buf[160];
	snprintf(buf, sizeof(buf), "%s (%s)", name ? name : "CUDA error",
		text ? text : std::to_string(result).c_str());
	return buf;
}

std::unique_ptr<NvencRuntime> load_runtime() {
	auto runtime = std::make_unique<NvencRuntime>();

	if (cuda_load_functions(&runtime->cu, nullptr) != 0) {
		WLOG_INFO("nvenc: libcuda.so.1 not available (no NVIDIA driver), NVENC disabled");
		return nullptr;
	}
	CUresult cr = runtime->cu->cuInit(0);
	if (cr != CUDA_SUCCESS) {
		WLOG_INFO("nvenc: cuInit failed: %s, NVENC disabled", error_string(runtime->cu, cr).c_str());
		cuda_free_functions(&runtime->cu);
		return nullptr;
	}

	// Loaded into a local and deliberately never freed: the function
	// pointers copied into `api` below live in this library.
	NvencFunctions *nvenc = nullptr;
	if (nvenc_load_functions(&nvenc, nullptr) != 0) {
		WLOG_INFO("nvenc: libnvidia-encode.so.1 not available, NVENC disabled");
		cuda_free_functions(&runtime->cu);
		return nullptr;
	}

	// NvEncodeAPICreateInstance refuses a struct version newer than the
	// driver knows with a bare NV_ENC_ERR_INVALID_VERSION; asking first
	// turns that into a message that says what to upgrade.
	uint32_t max_version = 0;
	if (nvenc->NvEncodeAPIGetMaxSupportedVersion(&max_version) != NV_ENC_SUCCESS) {
		WLOG_INFO("nvenc: NvEncodeAPIGetMaxSupportedVersion failed, NVENC disabled");
		nvenc_free_functions(&nvenc);
		cuda_free_functions(&runtime->cu);
		return nullptr;
	}
	uint32_t built_version = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
	if (max_version < built_version) {
		WLOG_INFO("nvenc: driver supports NVENC API %u.%u, wraith needs %u.%u (driver 530 or newer); "
				  "NVENC disabled",
			max_version >> 4, max_version & 0xf, (unsigned)NVENCAPI_MAJOR_VERSION,
			(unsigned)NVENCAPI_MINOR_VERSION);
		nvenc_free_functions(&nvenc);
		cuda_free_functions(&runtime->cu);
		return nullptr;
	}

	runtime->api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
	if (nvenc->NvEncodeAPICreateInstance(&runtime->api) != NV_ENC_SUCCESS) {
		WLOG_INFO("nvenc: NvEncodeAPICreateInstance failed, NVENC disabled");
		nvenc_free_functions(&nvenc);
		cuda_free_functions(&runtime->cu);
		return nullptr;
	}
	return runtime;
}

} // namespace

const NvencRuntime *nvenc_runtime() {
	static const std::unique_ptr<NvencRuntime> runtime = load_runtime();
	return runtime.get();
}

bool cuda_device_for_render_node(int drm_fd, CUdevice *device, std::string *why) {
	char bus_id[32] = {};
	if (drm_fd >= 0) {
		drmDevicePtr drm_device = nullptr;
		if (drmGetDevice2(drm_fd, 0, &drm_device) != 0 || !drm_device) {
			*why = "render node is not a DRM device libdrm can describe";
			return false;
		}
		bool nvidia = drm_device->bustype == DRM_BUS_PCI && drm_device->deviceinfo.pci &&
			drm_device->deviceinfo.pci->vendor_id == kNvidiaVendorId;
		if (nvidia) {
			const drmPciBusInfo *pci = drm_device->businfo.pci;
			snprintf(bus_id, sizeof(bus_id), "%04x:%02x:%02x.%x", pci->domain, pci->bus, pci->dev, pci->func);
		}
		drmFreeDevice(&drm_device);
		if (!nvidia) {
			*why = "render node is not an NVIDIA GPU";
			return false;
		}
	}

	const NvencRuntime *runtime = nvenc_runtime();
	if (!runtime) {
		*why = "NVIDIA driver libraries not available";
		return false;
	}

	CUresult cr;
	if (drm_fd < 0) {
		cr = runtime->cu->cuDeviceGet(device, 0);
	} else if (runtime->cu->cuDeviceGetByPCIBusId) {
		cr = runtime->cu->cuDeviceGetByPCIBusId(device, bus_id);
	} else {
		*why = "driver lacks cuDeviceGetByPCIBusId";
		return false;
	}
	if (cr != CUDA_SUCCESS) {
		*why = std::string("no CUDA device") + (drm_fd < 0 ? "" : std::string(" at PCI ") + bus_id) + ": " +
			cuda_error_string(cr);
		return false;
	}
	return true;
}

std::string cuda_error_string(CUresult result) {
	const NvencRuntime *runtime = nvenc_runtime();
	return error_string(runtime ? runtime->cu : nullptr, result);
}

void *open_nvenc_session(CUcontext context, const char *who) {
	const NvencRuntime *runtime = nvenc_runtime();
	if (!runtime) {
		return nullptr;
	}
	NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {};
	params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
	params.device = context;
	params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
	params.apiVersion = NVENCAPI_VERSION;
	void *session = nullptr;
	NVENCSTATUS st = runtime->api.nvEncOpenEncodeSessionEx(&params, &session);
	if (st != NV_ENC_SUCCESS) {
		// Consumer GeForce drivers cap concurrent sessions per system; a
		// full cap is the likeliest cause on a working driver, and it
		// comes back as a generic error, so say so.
		WLOG_ERROR("%s: nvEncOpenEncodeSessionEx failed (NVENCSTATUS %d) -- no NVENC on this GPU, or the "
				   "driver's concurrent session limit is reached",
			who, (int)st);
		// A session handle can come back even on failure; it still has to
		// be destroyed.
		if (session) {
			runtime->api.nvEncDestroyEncoder(session);
		}
		return nullptr;
	}
	return session;
}

CudaContextScope::CudaContextScope(CUcontext context) {
	const NvencRuntime *runtime = nvenc_runtime();
	if (runtime && context) {
		pushed_ = runtime->cu->cuCtxPushCurrent(context) == CUDA_SUCCESS;
	}
}

CudaContextScope::~CudaContextScope() {
	if (pushed_) {
		CUcontext popped = nullptr;
		nvenc_runtime()->cu->cuCtxPopCurrent(&popped);
	}
}

} // namespace wraith
