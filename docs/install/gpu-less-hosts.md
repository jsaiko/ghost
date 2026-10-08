# Hosts without a GPU

ghost runs on a host with no GPU: wraith falls back to the x264 software
encoder, and GNOME and the labwc-based session types render with
llvmpipe. KDE Plasma needs one extra step from the admin.

## Groups

A ghost session is seatless, so logind's `uaccess` ACLs never reach it
and group membership is the only way to a DRM device. With a GPU, every
session user needs `render`. On a GPU-less host set up with vgem
(below), they need `video` for vgem's card node. ghostd doesn't manage
this ([ADR 0012](../adr/0012-host-setup-stays-with-the-admin.md));
provision it wherever the accounts come from.

## KDE Plasma

kwin's screencast plugin requires OpenGL compositing, and kwin's virtual
backend offers OpenGL only when it can create a gbm device on a DRM
device, which a display-only adapter (bochs-drm in a VM) doesn't
provide. Without it, kwin falls back to QPainter, which has no screencast
path, and a `plasma` session fails to start after 30 s.

Loading the `vgem` module fixes it: kwin opens vgem's primary node, Mesa
backs it with llvmpipe, and capture takes the CPU path. Load it at boot
with `modules-load.d`:

```sh
echo vgem | sudo tee /etc/modules-load.d/vgem.conf
sudo modprobe vgem
```

What that costs:

- **Don't load vgem on a host with a real GPU.** libdrm doesn't sort
  devices, so kwin may pick vgem over the GPU and lose hardware
  rendering and encoding. (wraith ranks render nodes itself and isn't
  affected.)
- **Never unload it under a live session.** Xorg treats new DRM devices
  as GPUs and exits when one is removed.
- GNOME and KDE have been seen failing to start at 4K on a vgem host;
  lower resolutions work.

KWin 6.8 should make vgem unnecessary: it adds a software render device
backed by `/dev/udmabuf` and Mesa's EGL software device. Session users
will then need the `kvm` group (`/dev/udmabuf` is `root:kvm 0660`)
instead of `video`.
