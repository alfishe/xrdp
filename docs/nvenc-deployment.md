# Deploying the `nvenc` branch

This branch changes more than xrdp itself. A working setup has five parts:

| Component | Source | Why it is needed |
|-----------|--------|------------------|
| xrdp, xrdp-chansrv | this branch | NVENC H.264 encoder, adaptive frames in flight, audio latency control, A/V sync |
| xorgxrdp | upstream v0.10.x (v0.10.5 tested) | X server backend matching xrdp 0.10 |
| pipewire-module-xrdp | `av-sync` branch of the alfishe fork | receives the playback latency that xrdp-chansrv reports |
| mutter (GNOME on X11 only) | Ubuntu 24.04 package plus `docs/patches/mutter-46-stage-update-scheduled.patch` | without it the session can stay black after login |
| NVIDIA driver | distribution package | `libcuda.so.1` and `libnvidia-encode.so.1`, loaded at run time |

Tested on Ubuntu 24.04 with GNOME Shell on X11 (xorgxrdp), a Maxwell GPU and
driver 580. The client was Windows App on macOS at 3840x2160.

## 1. xrdp

### Build

Build dependencies, in addition to the usual xrdp ones:

```sh
sudo apt install nv-codec-headers   # pkg-config module ffnvcodec >= 11.0
```

```sh
./bootstrap
./configure --enable-fuse --enable-jpeg --enable-rfxcodec --enable-mp3lame \
            --enable-vsock --enable-painter --enable-openh264 --enable-nvenc
make -j"$(nproc)"
make check            # tests/chansrv and tests/xrdp cover the new code
```

NVENC is loaded with `dlopen()`, so the binary has no link-time dependency
on the NVIDIA driver. If no NVENC session can be opened, xrdp falls back to
the software encoder that was compiled in and logs the encoder it chose.

### Install

**`make install` overwrites the configuration in `/etc/xrdp`**, including
`gfx.toml` and `xrdp.ini`. To update a running system, either back up
`/etc/xrdp` first and restore it afterwards, or install only the binaries:

```sh
sudo install -m 755 xrdp/.libs/xrdp           /usr/local/sbin/xrdp
sudo install -m 755 sesman/chansrv/.libs/xrdp-chansrv /usr/local/sbin/xrdp-chansrv
sudo install -m 755 libxrdp/.libs/libxrdp.so.0.0.0    /usr/local/lib/xrdp/
sudo install -m 755 xup/.libs/libxup.so               /usr/local/lib/xrdp/
```

Adjust the prefix if xrdp was configured with a different one.

Restart xrdp only when no sessions are connected. Restarting it while a
session runs leaves that session orphaned.

```sh
sudo systemctl restart xrdp
```

### Configuration

`/etc/xrdp/gfx.toml` must select NVENC; the shipped default is x264:

```toml
[codec]
order = [ "H.264", "RFX" ]
h264_encoder = "NVENC"
```

Check after connecting:

```sh
grep -E "using .* encoder|NVENC:" /var/log/xrdp.log | tail -2
# xrdp_encoder_create: using NVENC (GPU) for software encoder
# NVENC: encoder opened 3840x2160 ... preset p2 qp 23
```

`/etc/xrdp/xrdp.ini`, `[Channels]`/globals section:

```ini
; shortest gap between captured frames, a frame rate ceiling (7 = ~142 Hz)
h264_frame_interval=7
```

Frames in flight (how many frames may be sent before the client
acknowledges one) are adapted to the measured round trip:
`ceil(round_trip / frame_interval) + 1`, at most 16. The xrdp log shows
every change:

```
xrdp_mm_frames_in_flight: 8 frames in flight for 49 ms round trip at 7 ms frame interval
```

To pin a fixed value instead, set `XRDP_GFX_FRAMES_IN_FLIGHT` in the
environment of the xrdp service (for example in `/etc/default/xrdp`).

## 2. xorgxrdp

Use an upstream release that matches xrdp 0.10 (the Ubuntu 24.04 package is
0.9.x and does not). Build it against the xrdp headers installed above:

```sh
./bootstrap
./configure PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
make -j"$(nproc)"
sudo make install
```

## 3. pipewire-module-xrdp

xrdp-chansrv reports the playback latency it measures at the client, minus
the video latency, to the sound server. Video players use that latency to
time frames, so audio and video line up. The stock module ignores the
reports. Without the patched module audio still plays, but runs ahead of the
picture.

```sh
git clone -b av-sync https://github.com/alfishe/pipewire-module-xrdp
cd pipewire-module-xrdp
./bootstrap
./configure
make -j"$(nproc)"
make check    # needs pipewire, wireplumber and dbus-run-session, else skipped
```

The test runs a private PipeWire daemon with a fake chansrv socket and
checks that a player linked to `xrdp-sink` sees the reported latency. The
module publishes the latency as the `Latency` param of the sink's input port.
With PipeWire 1.0, a stream that sets `ProcessLatency` instead does not pass
it on, and players see 0.

If the distribution package `pipewire-module-xrdp` is installed, divert its
module so package updates do not put it back, then install the build:

```sh
M=/usr/lib/x86_64-linux-gnu/pipewire-0.3/libpipewire-module-xrdp.so
sudo dpkg-divert --local --rename --divert "$M.distrib" --add "$M"
sudo install -m 644 src/.libs/libpipewire-module-xrdp.so "$M"
```

A reported latency only changes after the new value has held for 2 s.
Players resynchronise on every change, which is audible, so short spikes
in the video latency are not passed on.

Takes effect for new sessions. Check in the chansrv log of a session
(`~/.local/share/xrdp/xrdp-chansrv.<display>.log`) once audio plays:

```
process_pcm_message: sound server accepts playback latency reports
```

and in PipeWire, the player's node should show it (in ns) once audio plays:

```sh
pw-dump | grep -A3 '"Latency"' | grep minNs
```

Known issue of the module's loader script (`load_pw_modules.sh`): at every
login it destroys the old `xrdp-sink` node and starts a new `pw-cli`, but
the `pw-cli` of the previous session keeps running. They are idle and
harmless, but they accumulate until the user's systemd instance stops.

## 4. mutter (GNOME Shell on X11)

Mutter 46 (Ubuntu 24.04) can leave the screen black after login, with only
the X cursor visible, until a new window is mapped.

The cause is a frame that is dispatched before the stage is mapped. It is
discarded, but the stage stays marked as having an update scheduled. With
xorgxrdp the virtual input devices appear at that moment and queue a device
event. From then on every redraw request is ignored.

This is mutter issue
[#3804](https://gitlab.gnome.org/GNOME/mutter/-/issues/3804), fixed in mutter 48 by commit
[24083e1e](https://gitlab.gnome.org/GNOME/mutter/-/commit/24083e1e5851a13b9d307d538d84f42091a7b220).
`docs/patches/mutter-46-stage-update-scheduled.patch` is the backport of that
fix as a Debian quilt patch, including the regression test adapted to mutter 46.

Rebuild the package that matches the installed version:

```sh
V=$(dpkg-query -W -f '${Version}' libmutter-14-0)   # e.g. 46.2-1ubuntu0.24.04.18
sudo apt install devscripts equivs quilt
# enable deb-src in /etc/apt/sources.list.d/ubuntu.sources, or download the
# .dsc and tarballs from the archive pool, then:
apt-get source "mutter=$V"
cd mutter-46.2
sudo mk-build-deps -i -r debian/control
mkdir -p debian/patches/xrdp
cp <xrdp>/docs/patches/mutter-46-stage-update-scheduled.patch \
   debian/patches/xrdp/stage-Clear-update_scheduled-when-update-discarded.patch
echo xrdp/stage-Clear-update_scheduled-when-update-discarded.patch >> debian/patches/series
QUILT_PATCHES=debian/patches quilt push -a
dch -v "$V+xrdp1" -D noble "Backport upstream fix for the stage never updating after a discarded frame."
DEB_BUILD_OPTIONS=nocheck dpkg-buildpackage -b -uc -us
```

Run the regression test from the build tree. It fails with
`'painted' should be TRUE` when the fix is missing:

```sh
meson test -C obj-x86_64-linux-gnu --no-rebuild stage
```

The test starts a headless mutter that opens every DRM device. A device
without a render node fails with `Failed to open gpu ... Couldn't find
render node device` (for example a BMC VGA next to the NVIDIA card), and
tests turn that warning into a failure. To hide such a device from the
test only, run it in a private mount namespace:

```sh
DEV=$(readlink -f /sys/class/drm/card1)      # the device without a render node
sudo unshare -m sh -c "mount --make-rprivate / && mount -t tmpfs none $DEV &&
  exec setpriv --reuid=$(id -u) --regid=$(id -g) --init-groups \
  env HOME=$HOME XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR \
  meson test -C obj-x86_64-linux-gnu --no-rebuild stage"
```

Install the rebuilt libraries and hold them, otherwise the next Ubuntu
update of mutter brings the bug back:

```sh
cd ..
sudo apt install ./libmutter-14-0_*+xrdp1_amd64.deb ./gir1.2-mutter-14_*+xrdp1_amd64.deb \
                 ./mutter-common_*+xrdp1_all.deb ./mutter-common-bin_*+xrdp1_amd64.deb
sudo apt-mark hold libmutter-14-0 gir1.2-mutter-14 mutter-common mutter-common-bin
```

When Ubuntu publishes a newer mutter, unhold, rebuild from the new source
with the same patch and hold again. GNOME Shell picks up the new library at
the next login.

Do not work around this with a delay before starting the session. A delay
only makes the race less likely.

## Known limits

- GNOME Shell under xorgxrdp renders with llvmpipe (software GL). At
  3840x2160 it draws about 20 frames per second while using about 1.5 CPU
  cores. xrdp, NVENC and the network keep up with more, so this is the
  frame rate ceiling.
- Heavy CPU load on the host (for example a parallel build) can starve the
  session and make audio stutter. Run such jobs with `nice -n 19`.
