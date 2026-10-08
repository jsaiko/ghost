# Encoding

How wraith turns captured frames into a video stream, and which encoder
it picks. Code: `host/wraith/src/encode/`. PyroWave and lossless
refinement have their own pages: [PyroWave](pyrowave.md),
[refinement](refinement.md).

## Codecs

Everything codec-specific lives in four places; the encoder interface,
datagram framing, reassembly, presentation and input are codec-agnostic.

1. **The wire token**, `libgdp/include/gdp/video_codec.hpp`: the enum
   entry, `video_codec_from_token()` / `video_codec_token()`, and the
   token's place in `all_video_codec_tokens()`, which is the preference
   order.
2. **The encoder backend** in `host/wraith/src/encode/`, registered in
   `encoder_factory.cpp`'s table. A row names the codec, the backend,
   whether it is hardware (skipped under `wraith -F` or
   `encode.force_software`) and whether it needs a render node. Rows for
   one codec are tried in order, so "hardware, then software" is two
   rows.
3. **The decoder mapping**, `client/spectre/src/decode/decoder.cpp`'s
   `find_codec()`. Resolving through `avcodec_find_decoder()` keeps
   `can_decode()` honest on an FFmpeg built without a codec.
4. **The spec**, gdp-spec.md §6.6.

| Codec | Token | wraith | spectre |
|---|---|---|---|
| H.264 | `h264` | VA-API or NVENC, falling back to x264 | FFmpeg, hardware or software |
| H.265 | `h265` | VA-API or NVENC; no software fallback | FFmpeg, hardware only |
| AV1 | `av1` | VA-API or NVENC; no software fallback (off by default) | FFmpeg, hardware only |
| PyroWave | `pyrowave` | Vulkan compute (off by default) | Vulkan compute |

`h264` is the default and the universal floor. Both ends intersect their
preference list with what they can do (`supported_video_codecs()` in
wraith, limited by wraith.toml's `[encode.codecs]`;
`Decoder::can_decode()` in spectre), so a build never offers a codec it
can't serve. wraith opens its encoder before any client attaches, on the
default codec, and reopens it for the codec a session negotiates, trying
the client's preferences in order until one opens.

Every backend codes one slice (one tile for AV1) per picture, with no
B-frames, a single reference, and a key frame every `encode.gop` frames
(0 for key frames only on request) or on request, so a session behaves
the same whichever codec it negotiated. Colour is BT.601 limited range
for H.264, H.265 and AV1 (gdp-spec.md §7.3). Every H.264 and AV1 encoder
states it in the stream (VUI or sequence header); VA-API's H.265 doesn't,
and relies on the convention. A decoder left to guess may take HD video as
BT.709 (Firefox does), which shifts every colour.

## Choosing the render node

On a multi-GPU host, libdrm lists render nodes in readdir order, which
can change across reboots, so "the first node" may be the iGPU on one
boot and the discrete GPU on the next. `preferred_render_node()`
(`util/render_node.cpp`) therefore probes every render node for VA-API
encode entrypoints and NVENC codecs (`hardware_encodable_codecs()`,
using each hardware row's own profile) and takes the node that encodes
the most preferred enabled codec, keeping listed order on a tie.
`WLR_RENDER_DRM_DEVICE` in wraith's environment overrides the probe. The
journal logs what each node can encode and which was chosen
(`render node: ...`).

## VA-API

The three VA-API backends share everything that isn't the codec.
`VaapiEncoderBase` (`encode/vaapi/vaapi_encoder_base.*`) owns the
display, the VPP RGB→NV12 conversion, both surface pools, the dmabuf
import, the CPU upload path and the coded-buffer drain.
`VaapiH264Encoder`, `VaapiHevcEncoder` and `VaapiAv1Encoder` add their
parameter buffers, their header writer (`h264_bitstream.*`,
`hevc_bitstream.*` over the shared RBSP code in `nal_bitstream.*`;
`av1_obu.*` for AV1's OBUs) and their level selection.

Per frame, the captured RGB dmabuf is imported as a VA surface (no copy),
converted to NV12 by VPP, and encoded. The CPU upload path pins its
surface to `VA_FOURCC_BGRX`.

**Input and reconstruction surfaces stay separate.** Input surfaces are
only ever the `vaBeginPicture` render target; `CurrPic` and
`ReferenceFrames` name a dedicated DPB pool. Mesa's
`va/picture_h264_enc.c` turns whatever surface is named as `CurrPic` into
a DPB surface by destroying and reallocating its buffer without updating
the encoder's target, so naming the input surface there leaves the
encoder reading freed memory.

**Rate control** is VBR. The rate-control and frame-rate parameter
buffers are re-sent with every IDR, not only after `open()` and
`set_bitrate()`: radeonsi takes the frame rate from each new sequence
parameter buffer over the misc buffer's, and otherwise budgets every
frame for its default rate, overshooting the target.

**H.264.** SPS and PPS are written by wraith. The SPS carries a VUI with
the BT.601 studio-range colour description and `bitstream_restriction`
(no reordering), or some hardware decoders hold several frames before
output. radeonsi also needs a packed slice header
every frame, only to learn `nal_ref_idc` and `nal_unit_type`.

**H.265 on radeonsi.** The driver regenerates the VPS, SPS and PPS it
emits from what it parses out of the packed headers wraith supplies, and
emits none at all for a picture without a packed slice header
(`radeon_vcn_enc_encode_headers` returns early when `num_slices == 0`),
so the slice header is sent even though the driver writes the real one.
The frame is padded to whole 64-px coding tree blocks rather than the
8-px minimum coding block, because the driver otherwise pads to its own
CTB size and emits no conformance window for the difference.

**AV1 on radeonsi.** Mesa takes the whole sequence header and much of
the frame header by parsing the OBUs wraith writes
(`av1_sequence_header()` / `av1_frame_header()` in `picture_av1_enc.c`),
so picture size, colour description, order-hint bits, refresh flags and
reference indices exist only in `av1_obu.*`. The frame header is what
the driver counts as the picture's "slice", and a temporal delimiter OBU
goes ahead of it as the one header copied through verbatim. AV1 has no
cropping syntax and radeonsi rewrites the sequence header with its own
8-aligned size, so the AV1 backend pads to exactly 8 pixels and
replicates the edge into the padding. spectre crops it off using
`SessionAccept`'s display size; any other consumer of the stream sees up
to 7 extra columns or rows.

## NVENC

`encode/nvenc/`: one `NvencEncoder` for all three codecs. NVENC takes the
codec as a GUID and writes every header itself, so none of the VA-API
bitstream code has an equivalent. It is compiled into every build and
costs nothing without NVIDIA hardware: libcuda and libnvidia-encode are
loaded with `dlopen()` through the vendored nv-codec-headers loader
(`host/wraith/third_party/ffnvcodec`, MIT, API 12.1, driver R530 or
newer), and a render node whose PCI vendor isn't NVIDIA is refused before
either library loads. Its rows come after VA-API's for each codec and
don't need a render node; without one, NVENC uses the first CUDA device.

Configuration mirrors VA-API's: preset P4 with ultra-low-latency tuning,
I and P frames only, VBR with the ceiling as both average and maximum and
a one-second VBV, headers on every IDR. Input is packed RGB, and NVENC
converts it to YUV with the matrix and range the VUI names, BT.709 when
it names none. So the H.264 and H.265 VUI, like AV1's sequence header,
state BT.601 limited range; unsignalled, the picture decodes darker than
the lossless tiles.

Two input paths, fixed at open unless the first fails:

- **Zero-copy** (`push()`): CUDA can't import a dmabuf, so
  `CudaDmabufImporter` goes through Vulkan. The dmabuf is imported as a
  `VkImage` (explicit modifier; its implicit fence waited on through
  `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` and a sync-fd semaphore) and copied
  into a Vulkan buffer that CUDA imports once as external memory and
  NVENC registers. Each capture buffer is imported once and cached by
  its dmabuf inode (at most 8, least recently used evicted), re-imported
  only if its layout changes. The Vulkan device is matched to the CUDA
  device by UUID. If an import fails at runtime, the encoder asks for CPU
  frames from then on.
- **CPU upload** (`push_cpu()`): `cuMemcpy2D` into the same kind of
  buffer. Used when the importer can't start, when
  `encode.nvenc_zero_copy = false`, and under lossless refinement on a
  CPU path.

NVENC is asynchronous, like VA-API and PyroWave: two slots, each with
its own input buffer, registration and bitstream buffer. `push()` copies into a free
slot and submits; a worker thread blocks in `nvEncLockBitstream()` and
signals `completion_fd()`; `poll()` hands finished packets out in order
on the session's thread. `ready_for_frame()` is false while both slots
are busy, so the session skips frames and re-delivers the newest when a
slot frees. A caller that doesn't ask for asynchronous mode gets the
synchronous one.

Consumer GeForce drivers cap concurrent NVENC sessions per system, which
a multi-user host will reach; the open error says so.

## Software

x264 (`encode/software/`), used when no hardware row opens or `-F` /
`encode.force_software` forces it. H.264 only.

- `xrgb_convert.cpp` has an AVX2 XRGB→I420 conversion that must stay
  byte-identical to the scalar path (`xrgb_convert_test`).
- Threads are `SLICED`, not `FRAME`: frame threading breaks the
  per-call pts that `push_cpu()` stamps.
- Preset `superfast`: `ultrafast` drops intra 4x4 prediction, which
  ruins text, and slower presets add latency for no visible gain.
- No 4:4:4: VA-API decoders can't decode it. Every encoder sends 8-bit
  4:2:0 SDR, the baseline of gdp-spec.md §7.3; the wire can negotiate
  more depth, 4:4:4 and HDR, but no encoder produces them.

## Limitations

- NVENC has run on one GPU, an RTX 3070 Ti (Ampere): zero-copy and CPU
  upload input, colours, a still desktop's bitrate near zero, resizes,
  4K at about 60 fps once encoding overlapped the next frame's copy, and
  `set_bitrate()` (a 5 Mbit/s throttle: 80 -> 3 Mbps in about 5 s, frames
  34 -> 8 KB, back to full rate once lifted). Not yet seen: AV1, which
  needs Ada or newer (Ampere declines it and the session settles on the
  next codec).
- NVIDIA lowers its clocks under a light load, and NVENC slows with
  them: a 4K frame's encode went from 9 to 17 ms on an RTX 3070 Ti. Pinning clocks
  (`nvidia-smi -lgc`) is the host administrator's choice; wraith doesn't.
- AV1 can't crop, so non-multiple-of-8 sizes carry padding (cropped by
  spectre).
- Software encode is H.264 only, by decision.
