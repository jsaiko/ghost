// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The NVIDIA driver libraries NVENC needs, loaded at runtime.
//
// wraith does not link against libcuda or libnvidia-encode: both ship with
// the proprietary driver, not with a -dev package, and a wraith built on
// an AMD or Intel box has to run unchanged on an NVIDIA one. The vendored
// nv-codec-headers (third_party/ffnvcodec, MIT, the same headers FFmpeg
// builds against) supply the API definitions and a dlopen() loader;
// nvenc_runtime() runs that loader once per process and keeps the result.
//
// Nothing here is NVIDIA-hardware-specific enough to fail on a machine
// without it -- the loader simply reports the libraries missing -- so the
// NVENC backend is compiled into every build, and its rows in the encoder
// table (encoder_factory.cpp) fall through on hosts that can't use it.
#pragma once

// dynlink_loader.h reports load failures through these; wraith reports
// them itself, once, from nvenc_runtime() instead.
#define FFNV_LOG_FUNC(logctx, msg, ...) ((void)0)
#define FFNV_DEBUG_LOG_FUNC(logctx, msg, ...) ((void)0)
#include <ffnvcodec/dynlink_loader.h>

#include <string>

namespace wraith {

struct NvencRuntime {
	CudaFunctions *cu = nullptr;
	// Filled once by NvEncodeAPICreateInstance: the entry points are
	// process-global, not per encode session.
	NV_ENCODE_API_FUNCTION_LIST api = {};
};

// Loads libcuda.so.1 and libnvidia-encode.so.1, runs cuInit() and checks
// that the driver speaks at least the NVENC API version the vendored
// headers describe. Null, having logged why at INFO the first time, when
// any of that fails -- no NVIDIA driver, or one too old for the headers.
// Called once; later calls return the cached answer.
const NvencRuntime *nvenc_runtime();

// The CUDA device behind the render node `drm_fd`, matched by PCI bus ID,
// so NVENC runs on the GPU wraith renders on. With no render node at all
// (drm_fd < 0) it is simply the first CUDA device: the CPU upload path
// does not care where the pixels came from. False, with `why` set, for a
// render node that isn't an NVIDIA GPU -- checked from libdrm before the
// driver libraries are even loaded, so an AMD or Intel host never dlopen()s
// libcuda -- or when the runtime isn't available.
bool cuda_device_for_render_node(int drm_fd, CUdevice *device, std::string *why);

// "CUDA_ERROR_OUT_OF_MEMORY (out of memory)" for log lines.
std::string cuda_error_string(CUresult result);

// Opens an NVENC session on `context` (a CUDA context on the device to
// encode on). Null, having logged `who` and the reason, on failure. The
// caller closes it with api.nvEncDestroyEncoder().
void *open_nvenc_session(CUcontext context, const char *who);

// Makes a CUDA context current for the lifetime of the object, restoring
// whatever was current before. Every NVENC entry point that touches CUDA
// memory, and every cu* call, happens inside one.
class CudaContextScope {
public:
	explicit CudaContextScope(CUcontext context);
	~CudaContextScope();
	CudaContextScope(const CudaContextScope &) = delete;
	CudaContextScope &operator=(const CudaContextScope &) = delete;

private:
	bool pushed_ = false;
};

} // namespace wraith
