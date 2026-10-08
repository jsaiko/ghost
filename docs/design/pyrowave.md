# PyroWave

`pyrowave` is an intra-only wavelet codec (CDF 9/7 over 32x32 coefficient
blocks, bit-plane coding, no entropy coder, exact per-frame rate control)
that runs entirely in Vulkan compute, from
[Themaister/pyrowave](https://github.com/Themaister/pyrowave). It trades
bandwidth for latency: encode and decode take well under a millisecond
at 4K on a desktop GPU, at several hundred Mbit/s, so it is for wired
LANs only. Code: `host/wraith/src/encode/pyrowave/`,
`client/spectre/src/decode/pyrowave_decode.*`.

No distribution packages it: `packaging/build-pyrowave.sh` builds the
pinned commit into `/usr/local`, and wraith and spectre pick up
`libpyrowave-shared` through pkg-config, building without the codec when
it's absent. The Windows client gets the same commit from a vcpkg port
(`packaging/windows/ports/pyrowave`). The bitstream is still a draft
upstream, so both ends must run the same commit.

## Costs

At 4K, 4:2:0:

- **GPU time.** About 0.2 ms to encode and 0.08 ms to decode on an
  RX 9070 XT; about 1.1 ms and 0.55 ms on an RX 590 (Polaris, no fp16),
  which is 4K60 on a GPU whose hardware encoder tops out near 24 fps.
- **Bitrate.** Photos and games look fine from 0.5 bit/pixel, but dense
  text shows halos until about 1.0. So the ceiling is
  `encode.pyrowave_bpp` (1.0) bits per output pixel at 60 fps, about
  500 Mbit/s at 4K and 125 at 1080p, in place of `max_bitrate_mbps`
  (`SessionServices::encode_bitrate_bps()`). Every frame is that large.
- **Wire time.** A 1 MB frame needs about 8.5 ms on a 1 Gbit/s link
  however it is paced, so on 1G it pays off at 1080p and 1440p; 4K wants
  2.5G or more.
- **4:4:4** looked no better on text and costs about 5x the encode on
  Polaris, so it's 4:2:0 only.

## Negotiation

gdp-spec.md §6.6. spectre offers PyroWave, first, only when asked with
`-C pyrowave` or when `probe_lan_link()` (`net/lan_link.*`) finds the
host directly reachable over a wired link of 1 Gbit/s or more: a route
without a gateway, or the local machine; Wi-Fi and speedless tunnels
don't count. On Linux the speed is sysfs's, following bridges, bonds and
VLANs to their ports; on Windows it is that of the route's interface,
which must be 802.3 Ethernet. macOS has no check. `-W` (spectre-qt's
"Use PyroWave on a wired LAN", on by default) turns the check off. wraith needs
`[encode.codecs] pyrowave = true` and never picks it on a `via_gateway`
session. Every Vulkan GPU can run it, so it plays no part in choosing the
render node.

## wraith

`PyrowaveEncoder` runs PyroWave on a `VkDevice` of its own on the render
node's GPU, with every feature the GPU offers enabled, lent to PyroWave
through `pyrowave_create_device`. Frames go into one optimal BGRA image,
and a worker thread runs PyroWave's scaled encode, which also does the
RGB→YCbCr conversion on the GPU (full-range BT.709). Two ways in:

- **`push()`** takes a dmabuf, imports it as a `VkImage`
  (`VK_EXT_image_drm_format_modifier`, cached per buffer) and copies it
  into that image on the GPU, waiting on the buffer's own fence. `push()`
  waits for the copy, so the buffer is free again when it returns. Only
  a modifier and plane count the driver listed
  (`supported_import_modifiers()`) is imported: an import with
  `DRM_FORMAT_MOD_INVALID` can hang the GPU.
- **`push_cpu()`** takes XRGB rows, copied into a host-cached staging
  buffer (not VRAM through the PCIe BAR, whose CPU writes are slow) and
  uploaded by the worker. Used without the import extensions, and for
  good after an import fails. PyroWave's own CPU entry point creates and
  uploads three images per frame and is far slower.

Every frame stands alone, so every packet is a keyframe and loss needs
no repair. The packet is the whole frame's bitstream, sequence header
first, sliced by GDP like any other. Refinement over PyroWave hashes on
the GPU like any dmabuf-taking base ([refinement](refinement.md#cost)).
PyroWave's output isn't a pure function of its input (the same frame
twice differs in about 0.5% of its bytes), so
`wraith_pyrowave_input_test` compares the encoder's input image
(`read_back_input()`), not packets.

## spectre

spectre decodes on the presenter's own device
(`VulkanDevice::compute_device()`, which also enables the features
PyroWave's shaders use) into frames from an FFmpeg Vulkan pool of 3-plane
4:2:0 images (`G8_B8_R8_3PLANE_420_UNORM`, mutable so PyroWave writes
each plane through an R8 storage view). They take the same
`AV_PIX_FMT_VULKAN` path as Vulkan Video's frames, sampled with their own
full-range BT.709 conversion. PyroWave records into spectre's command
buffer, and spectre submits it on the presenter queue, waiting on and
signalling the frame's timeline semaphore as FFmpeg expects. Each payload
is decoded on its own (`pyrowave_decoder_clear()` first), since PyroWave
would otherwise drop a frame whose 3-bit sequence number looks older
after a run of lost ones. lavapipe has the features but decodes wrongly,
so `supports_pyrowave()` rejects CPU devices.

Not every driver lets PyroWave write the planes of that image in place:
Intel's ANV refuses storage views of a planar format (found on a Skylake
HD 530, whose report said `the 3-plane 4:2:0 image can't be written
through storage views`). There the decoder is *staged*: PyroWave writes
three plain R8 images (luma size, then chroma at half), each frame's
command buffer then copies them into the planes of the output frame, and
the frame is only sampled and copied to. It is one GPU copy of a few
megabytes per frame. `VulkanDevice::pyrowave_writes_planes_directly()`
picks the path and `supports_pyrowave()` accepts either.
`SPECTRE_PYROWAVE_STAGED=1` forces the staged path on a GPU that doesn't
need it, to try it there. The staged path has been built but not run on
Intel hardware.

## Limitations

- Partial frames aren't decoded: PyroWave's blocks decode independently,
  but GDP drops an incomplete frame whole.
- The wired-LAN check runs on Linux and Windows, not macOS.
- Not possible on the Raspberry Pi 4 (no `shaderInt16`).
