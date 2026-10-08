// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Turns a decoded AVFrame into a sampled VkImageView the video pipeline can
// draw. Linux and macOS stay NV12 and Windows converts to BGRA, a choice
// forced by what each platform can share into Vulkan:
//
// Linux -- everything stays NV12 (VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) and
// YUV -> RGB happens in a VK_KHR_sampler_ycbcr_conversion sampler, not a
// shader:
// - AV_PIX_FMT_VAAPI: imports the decoded VASurface's dmabuf directly as a
//   VkImage (VK_EXT_image_drm_format_modifier, disjoint per-plane binding,
//   no copy). Every frame gets its own VASurface, so the import is
//   per-frame and torn down again in release() once the frame is drawn.
// - AV_PIX_FMT_YUV420P (software fallback): CPU-uploads into a persistent
//   host-visible staging VkImage, interleaving the separate U/V planes into
//   NV12 on the way in.
//
// macOS -- NV12 and the same ycbcr sampler as Linux, on MoltenVK:
// - AV_PIX_FMT_VIDEOTOOLBOX: the frame is a CVPixelBuffer (NV12, video
//   range, IOSurface-backed). Its two planes are copied into the same
//   persistent staging image the software fallback uses -- one memcpy per
//   row, no conversion, and the GPU shares system memory with the CPU on
//   every Mac this runs on. Not a zero-copy import: MoltenVK (as of 1.4.2)
//   rejects CoreVideo's NV12 IOSurfaces for VK_EXT_metal_objects' import
//   (it wants the whole surface described as 6-byte 2x2 blocks, CoreVideo
//   describes it per plane), and importing the planes as separate Metal
//   textures overwrites the multi-planar image's format with the last
//   plane's.
// - AV_PIX_FMT_YUV420P (software fallback): as on Linux.
//
// Windows -- everything is BGRA (VK_FORMAT_B8G8R8A8_UNORM) and there is no
// ycbcr sampler at all, because sharing a *planar* texture out of D3D11
// does not work: D3D11 packs NV12's chroma plane at pitch*height, Vulkan
// pads the luma plane's height first and looks for chroma further along, so
// the imported image reads chroma from the wrong offset and runs off the
// end of the allocation. Nothing about how the texture is created fixes
// that, and the driver reports DEDICATED_ONLY, which rules out binding each
// plane at an explicit offset the way the dmabuf path does. A single-plane
// texture has no plane offsets to disagree about, hence:
// - AV_PIX_FMT_D3D11: the decoder writes into a texture *array* it owns,
//   whose slices can't be shared out of D3D11 anyway, so ID3D11VideoProcessor
//   converts the frame's slice NV12 -> BGRA (on the GPU, fixed-function)
//   into one shareable single-plane texture, which is imported once as a
//   VkImage (VK_KHR_external_memory_win32) and reused for every later
//   frame. The pixels never leave the GPU.
// - AV_PIX_FMT_YUV420P (software fallback): CPU-uploads into the staging
//   image, converting YUV420P -> BGRA on the way in so that it lands on the
//   same format and sampler as the hardware path.
//
// Every platform -- AV_PIX_FMT_VULKAN (Vulkan Video decode): FFmpeg decoded
// straight into an NV12 VkImage on this very device, so there is nothing to
// import: acquire() just makes a view of it. What it does need is FFmpeg's
// sync protocol -- each image carries a timeline semaphore the present
// submit must wait on and then signal one higher (timeline_sync()), and its
// current layout, which the acquire barrier transitions from and
// mark_submitted() records the new one of. On Windows this is the one path
// that stays NV12, so it gets the ycbcr sampler there too. PyroWave's
// compute decode hands its frames over the same way, as 3-plane I420
// images (pyrowave_decode.hpp).
//
// Every frame is one of three formats (VideoFormat) and each format has
// its own sampler -- owned here, since it's what makes the views of one
// format interchangeable -- and its own pipeline in VulkanPresenter.
#pragma once

#include "present/vulkan_device.hpp"

#include <cstdint>

struct AVFrame;
struct AVHWFramesContext;
struct AVVkFrame;
#ifdef _WIN32
struct ID3D11Texture2D;
struct ID3D11Query;
struct ID3D11VideoDevice;
struct ID3D11VideoContext;
struct ID3D11VideoProcessor;
struct ID3D11VideoProcessorEnumerator;
struct ID3D11VideoProcessorOutputView;
struct AVD3D11VADeviceContext;
#endif

namespace spectre {

// The image formats a frame can reach the video pipeline in: NV12 through a
// ycbcr-conversion sampler, BGRA through a plain one (Windows' D3D11VA
// and software paths -- see the file comment), or 3-plane I420 through a
// ycbcr sampler of its own -- PyroWave's compute decode
// (pyrowave_decode.hpp), which writes the planes separately and is
// full-range BT.709 where everything else is studio-range BT.601.
enum class VideoFormat {
	Nv12,
	Bgra,
	I420,
};
constexpr int kVideoFormatCount = 3;

class VideoImageSource {
public:
	VideoImageSource() = default;
	~VideoImageSource();
	VideoImageSource(const VideoImageSource &) = delete;
	VideoImageSource &operator=(const VideoImageSource &) = delete;

	// Creates the samplers (and the ycbcr conversion behind the NV12 one).
	// `device` must outlive this object.
	bool init(VulkanDevice &device);

	// Whether frames can ever arrive in `format` on this build and device --
	// the formats VulkanPresenter builds a pipeline for. NV12 always on
	// Linux and macOS; on Windows BGRA always, NV12 when the ycbcr sampler
	// could be made (needed only by Vulkan Video decode); I420 wherever its
	// sampler could be made (PyroWave).
	bool supports(VideoFormat format) const { return sampler_[(int)format] != VK_NULL_HANDLE; }
	// The immutable sampler `format`'s descriptor set layout must be built
	// with (a ycbcr-conversion sampler can't be bound any other way), and
	// how many descriptor pool slots that one binding consumes.
	VkSampler sampler(VideoFormat format) const { return sampler_[(int)format]; }
	// Some implementations need more than one descriptor pool slot to back
	// a single multi-planar combined-image-sampler binding (the Vulkan
	// spec's "Sampler Y'CBCR Conversion" chapter) -- queried, not assumed.
	// Always 1 for BGRA.
	uint32_t descriptor_count(VideoFormat format) const { return descriptor_count_[(int)format]; }

	// Imports, converts, uploads, or just views `frame` (see the file
	// comment for which). On success view() is valid until release().
	// `frame` must be AV_PIX_FMT_VAAPI, AV_PIX_FMT_D3D11,
	// AV_PIX_FMT_VIDEOTOOLBOX or AV_PIX_FMT_VULKAN (NV12 internally -- true
	// of every profile wraith's FFmpeg-decoded encoders use -- or PyroWave's
	// I420), or AV_PIX_FMT_YUV420P (software and V4L2 decode).
	bool acquire(AVFrame *frame);
	VkImageView view() const { return current_view_; }
	// The acquired frame's format -- which pipeline draws it.
	VideoFormat format() const { return current_format_; }
	// Width and height of the acquired image, which can exceed the frame's
	// own: a Vulkan-decoded image is the codec's coded size, padding and
	// all (VulkanPresenter crops it off with video.vert's uvScale).
	uint32_t image_width() const { return current_width_; }
	uint32_t image_height() const { return current_height_; }
	// The layout the acquired image is in once record_acquire_barrier() has
	// run -- what a descriptor referencing view() must declare. Everything
	// but a shared D3D11 texture uses SHADER_READ_ONLY_OPTIMAL; that one
	// stays in GENERAL, the only layout whose meaning both APIs agree on
	// for memory D3D11 writes and Vulkan reads.
	VkImageLayout layout() const;

	// Records the barrier that makes the acquired image shader-readable:
	// host-write -> shader-read for the staging image, or an ownership
	// transfer from the producing API (VK_QUEUE_FAMILY_FOREIGN_EXT for a
	// dmabuf import, VK_QUEUE_FAMILY_EXTERNAL for a shared D3D11 texture).
	// `queue_family_index` is the destination family (the device's one
	// graphics queue).
	void record_acquire_barrier(VkCommandBuffer cmd, uint32_t queue_family_index);

	// A Vulkan-decoded frame's timeline semaphore: the present submit must
	// wait for `wait_value` before touching the image and signal
	// `signal_value` once done with it, then call mark_submitted(). False
	// (nothing to do) for every other kind of frame.
	bool timeline_sync(VkSemaphore *semaphore, uint64_t *wait_value, uint64_t *signal_value) const;
	// Records what the submit just did to a Vulkan-decoded frame (its new
	// layout, and the semaphore value it will reach) so FFmpeg picks up from
	// there when it next touches the image. A no-op for any other frame.
	void mark_submitted();

	// Drops the per-frame dmabuf import or Vulkan image view (and hands a
	// Vulkan frame back to FFmpeg if it was never submitted). The software
	// staging image and the shared D3D11 texture are both persistent, so
	// this is a no-op for those. Only call once the GPU is done reading the
	// image -- i.e. after the present loop's vkQueueWaitIdle.
	void release();

private:
	// What produced the acquired image.
	enum class Source {
		None,
		Software,
		Dmabuf,
		D3d11,
		Vulkan,
	};

	bool create_nv12_sampler();
	bool create_i420_sampler();

	bool acquire_vulkan_frame(AVFrame *frame);
	void release_vulkan_frame();

#ifdef __linux__
	struct Imported {
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		VkImageView view = VK_NULL_HANDLE;
	};
	bool import_frame(AVFrame *frame, Imported *out);
	void destroy_imported(Imported *imported);
#endif

	bool ensure_software_image(uint32_t width, uint32_t height);
	void destroy_software_image();
	bool upload_software_frame(AVFrame *frame);
#ifdef __APPLE__
	bool upload_videotoolbox_frame(AVFrame *frame);
#endif

#ifdef _WIN32
	bool convert_shared_frame(AVFrame *frame);
	bool ensure_shared_texture(AVD3D11VADeviceContext *d3d11, uint32_t width, uint32_t height);
	void destroy_shared_texture();
#endif

	VulkanDevice *dev_ = nullptr;

	VkSampler sampler_[kVideoFormatCount] = {};
	uint32_t descriptor_count_[kVideoFormatCount] = {1, 1, 1};
	VkSamplerYcbcrConversion ycbcr_conversion_ = VK_NULL_HANDLE;
	VkSamplerYcbcrConversion i420_conversion_ = VK_NULL_HANDLE;

	// What acquire() last produced.
	VkImageView current_view_ = VK_NULL_HANDLE;
	Source current_source_ = Source::None;
	VideoFormat current_format_ = VideoFormat::Nv12;
	// The acquired Vulkan frame's format (acquire_vulkan_frame()).
	VideoFormat vk_format_ = VideoFormat::Nv12;
	uint32_t current_width_ = 0;
	uint32_t current_height_ = 0;

	// Vulkan Video path only: the view made for the acquired frame's image,
	// and that frame (locked through its frames context's lock_frame() from
	// acquire() until mark_submitted() or release()) with the layout it was
	// in at acquire time.
	VkImageView vk_view_ = VK_NULL_HANDLE;
	VkImage vk_image_ = VK_NULL_HANDLE;
	AVHWFramesContext *vk_frames_ = nullptr;
	AVVkFrame *vk_frame_ = nullptr;
	VkImageLayout vk_old_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
#ifdef __linux__
	Imported imported_; // dmabuf path only; stays empty otherwise
#endif

#ifdef _WIN32
	// D3D11VA hardware-decode target: our own single-plane BGRA texture (see
	// the file comment for why not NV12), imported into Vulkan as hw_image_
	// and reused until the frame size changes. hw_convert_done_ is the event
	// query convert_shared_frame() waits on so the shader never samples a
	// conversion the video engine hasn't finished -- Vulkan has no way to
	// see a D3D11 submission, so nothing else orders the two.
	ID3D11Texture2D *hw_texture_ = nullptr;
	void *hw_shared_handle_ = nullptr; // HANDLE; ours to CloseHandle, import does not take it
	ID3D11Query *hw_convert_done_ = nullptr;
	ID3D11VideoProcessor *hw_processor_ = nullptr;
	ID3D11VideoProcessorEnumerator *hw_processor_enum_ = nullptr;
	ID3D11VideoProcessorOutputView *hw_processor_output_ = nullptr;
	OwnedImage hw_image_;
	uint32_t hw_width_ = 0;
	uint32_t hw_height_ = 0;
#endif

	// The upload target for AV_PIX_FMT_YUV420P frames (software and V4L2
	// decode), and macOS' VideoToolbox frames too: a persistent staging
	// image in the platform's video format, recreated only when the frame
	// size changes. Kept mapped for its whole
	// life, like LosslessPlane's image, since it is rewritten every frame;
	// sw_planes_ is each plane's offset/pitch within that mapping (one plane
	// on Windows' BGRA, two on Linux's and macOS' NV12).
	OwnedImage sw_image_;
	uint8_t *sw_mapped_ = nullptr;
	VkSubresourceLayout sw_planes_[2] = {};
	uint32_t sw_image_width_ = 0;
	uint32_t sw_image_height_ = 0;
};

} // namespace spectre
