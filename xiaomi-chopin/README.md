# Nura on the Xiaomi chopin (Redmi Note 10 Pro 5G / POCO X3 GT)

An unofficial port of [Nura](https://gitlab.postmarketos.org/postmarketOS/pmaports) (formerly postmarketOS; edge, OpenRC, Phosh) to the Xiaomi **chopin**, the MediaTek MT6893 (Dimensity 1100) phone sold as Redmi Note 10 Pro 5G and POCO X3 GT. It uses the phone's downstream 4.14 kernel, so it belongs to Nura's `downstream` device category. It is not part of Nura's official package repositories, and the Nura project neither builds nor supports it.

**Installing erases all Android data. Never lock the bootloader again after installing: with a custom boot image, relocking can leave the phone unbootable.**

## Status

Verified on a global chopin with stock firmware MIUI V14.0.6.0.TKPMIXM. The image was built in a clean environment from this branch and installed by the steps below; the hardware checks were repeated after a reboot.

| Area | Status |
| --- | --- |
| Boot and storage | Downstream 4.14 kernel on `boot_a`; root filesystem in `userdata` as a nested 4096-byte-sector GPT that grows to the whole partition on first boot |
| Display and GPU | Native 1080×2400 panel at scale 3; Mali-G77 OpenGL ES through a private Mesa build; no Vulkan |
| Touch and keyboard | Novatek touchscreen; Stevia on-screen keyboard |
| Wi-Fi | MediaTek connectivity with the phone's own calibration; WPA3-Personal; reconnects after reboot and suspend |
| Bluetooth | Factory address; pairing and encrypted connections; headphone playback |
| Audio | Stereo speakers and two microphones through PipeWire, with speaker protection using the phone's own calibration |
| Sensors | Automatic rotation and ambient-light auto-brightness; **no proximity sensor** |
| Charging and power | Charge window 55–75% with thermal current limits; native `s2idle` suspend with RTC wake |
| Video decode | H.264 through V4L2 is bit-exact. Applications decode in software, because GStreamer's GL display of the hardware decoder's frames is corrupted |
| Browsers | Firefox and Epiphany (WebKitGTK sandbox enabled) with GPU rendering |
| Location | The stock GNSS stack runs and feeds geoclue; **no position fix obtained indoors** |
| Cameras | All four sensors are detected; **no preview or capture** |
| Not verified | Cellular and calls, USB/headset microphones and Bluetooth hands-free, codecs other than H.264, power-button wake from suspend, standby battery drain |

### Packages on this branch

| Package | Version | Purpose |
| --- | --- | --- |
| `linux-xiaomi-chopin` | 4.14.336-r26 | Downstream kernel ([xiaomi-mt6893-dev/kernel_xiaomi_mt6893](https://github.com/xiaomi-mt6893-dev/kernel_xiaomi_mt6893)) with the port's fixes |
| `device-xiaomi-chopin` | 1-r42 | Boot image, services, calibration loaders, runtime for the stock vendor daemons |
| `firmware-xiaomi-chopin` | 20240528-r3 | Stock firmware, extracted by you (see below) |
| `mesa-panfork-chopin` | 23.0.0_git20240119-r12 | Mali-G77 OpenGL ES for the vendor GPU driver, in a private prefix |
| `gst-plugins-base` | 1.28.7-r2 | GL shader fix for MediaTek tiled video frames |
| `phoc` | 0.57.0-r3 | Suspend crash and rotation blanking fixes |
| `pipewire` | 1.6.9-r1 | Launcher readiness fix |
| `polkit` | 127-r4 | Daemon starts as its own user |
| `iio-sensor-proxy` | 3.9-r3 | MediaTek sensor-hub backend |
| `bubblewrap` | 0.13.0-r1 | Keeps the fallbacks Linux 4.14 needs |
| `postmarketos-ui-phosh` | 33-r1 | Installs the Stevia keyboard with its schemas |

### Known issues

- `rc-status` shows `polkit` as `failed`. D-Bus activation starts `polkitd` as its own user, and a second start at boot loses the race for the bus name. Authorization works.
- `modemmanager` fails (there is no modem support in this port), and `user.greetd` shows `failed`. Neither affects the session.
- If Tailscale is installed and starts before Wi-Fi has DNS, name resolution fails. Running `rc-service tailscale stop; resolvconf -u; rc-service tailscale start` once fixes it permanently.
- This branch's packages have higher release numbers than the official Nura packages of the same versions. When Nura releases a newer version of one of them, `apk upgrade` installs it and the port's fix is lost. Rebuild from an updated branch instead.
- This branch is pinned to pmaports commit `2713656a9869d1a7df0588e388f3446e8d707e39` (2026-09-30), but the binary packages it builds against come from the moving edge repositories. Builds made much later may need adjustments.

## Installation

### 1. What you need

- A chopin with **stock MIUI V14.0.6.0.TKPMIXM (global)** installed on slot `a`. The firmware, the vendor daemons and the kernel drivers were verified only with that release. To install it, flash the official fastboot ROM with MiFlash using **"clean all", never "clean all and lock"**. Other regional variants are untested.
- An unlocked bootloader (Xiaomi's Mi Unlock); `fastboot getvar unlocked` must report `yes`.
- A Linux x86-64 computer with `git`, `python3`, `curl`, `fastboot`, `simg2img`, `lpunpack`, `debugfs` (e2fsprogs) and `sudo`, about 35 GB of free disk space and a network connection. Connect the phone directly to the computer: USB hubs may not work in fastboot mode.
- A battery charged above 50%.

### 2. Get the sources

```sh
git clone https://gitlab.postmarketos.org/postmarketOS/pmbootstrap.git
git -C pmbootstrap checkout 39e9c17c1439b25f7aced54e03f19b0515cdb029
git clone -b xiaomi-chopin https://github.com/hauau/pmaports-xiaomi-chopin.git pmaports
```

### 3. Extract the firmware

The firmware is proprietary and is not in this repository. Extract it from Xiaomi's official fastboot ROM (5.7 GB; its MD5 starts with `253b98dc56`, as the file name says). `bigota.d.miui.com` serves the same file, but very slowly.

```sh
ROM=chopin_global_images_V14.0.6.0.TKPMIXM_20240528.0000.00_13.0_global_253b98dc56.tgz
curl -O "https://bkt-sgp-miui-ota-update-alisgp.oss-ap-southeast-1.aliyuncs.com/V14.0.6.0.TKPMIXM/$ROM"
tar xzf "$ROM" --wildcards '*/images/super.img' '*/images/boot.img'
simg2img chopin_global_images_*/images/super.img super.raw
mkdir lp vendor
lpunpack -p vendor_a super.raw lp
debugfs -R "rdump /firmware vendor" lp/vendor_a.img   # ownership warnings are harmless
pmaports/device/downstream/firmware-xiaomi-chopin/extract-files.sh vendor
```

`extract-files.sh` copies 19 files into the firmware recipe and checks their SHA512 sums; it refuses files from any other vendor release. Keep `images/boot.img` (SHA256 `29bc389287c1b37a21c1b5d0f213bfc8740dae8d3e9887e270eba1d695db766f`): it is the stock boot image. You can delete the rest of the ROM.

### 4. Build

Point pmbootstrap at the cloned branch for every command. In `pmbootstrap init`, choose vendor `xiaomi`, device `chopin`, user interface `phosh`, init system OpenRC, PipeWire as the audio backend and iwd for Wi-Fi. Enable SSH keys if you want your public keys copied to the phone. Then build in exactly this order: each step builds what the next needs, and other orders fail.

```sh
PMB="./pmbootstrap/pmbootstrap.py -p $PWD/pmaports -t 3600"
$PMB init
$PMB build --arch aarch64 --lax gst-plugins-base polkit iio-sensor-proxy
$PMB --no-cross build --arch aarch64 --lax mesa-panfork-chopin phoc pipewire
$PMB build --arch aarch64 --lax bubblewrap postmarketos-ui-phosh firmware-xiaomi-chopin linux-xiaomi-chopin
$PMB build --arch aarch64 --lax device-xiaomi-chopin
$PMB install --no-sparse --sector-size 4096 --filesystem ext4
$PMB export --no-install /tmp/chopin-export
```

The builds took about 50 minutes on a desktop computer. `install` asks for your user's password. `export` links `boot.img` (134217728 bytes) and `xiaomi-chopin.img` (about 3 GB).

### 5. Flash

Turn the phone off, then hold **Power + Volume Down** to enter fastboot. Check that it is the right phone, then write exactly these two partitions:

```sh
fastboot getvar product                # chopin
fastboot getvar unlocked               # yes
fastboot getvar current-slot           # a
fastboot getvar partition-size:boot_a  # 0x8000000
fastboot flash userdata /tmp/chopin-export/xiaomi-chopin.img
fastboot flash boot_a /tmp/chopin-export/boot.img
fastboot reboot
```

Do not run `fastboot flashing lock`, `fastboot erase` or any command naming another partition. In our test, flashing took about two minutes and the phone booted in under 30 seconds.

### 6. First boot

The phone shows the Phosh login screen; log in with the password you gave `install`. Over USB it also appears as a network device. It is `172.16.42.1`; give your computer `172.16.42.2/24` and connect with `ssh <user>@172.16.42.1`. Check:

- `rc-status`: every `xiaomi-chopin-*` service is started (see the known issues for the expected `failed` entries).
- `/run/xiaomi-chopin-audio-calibrated` exists, `btmgmt info` shows your phone's factory Bluetooth address, and `nmcli dev wifi list` finds networks.
- `df -h /`: the root filesystem already fills `userdata`.

**Then back up your factory partitions** and keep the archive offline. They hold your phone's calibration and identifiers, which no ROM can restore:

```sh
ssh -t <user>@172.16.42.1 'sudo sh -s' < pmaports/xiaomi-chopin/backup-factory-partitions.sh > chopin-factory.tar
```

The archive contains seven partition images and their SHA256 sums. Never publish it. Also keep the exported `boot.img`: a boot image names the root filesystem it boots, so only images built for your installation can boot it.

### 7. Updates

Pull the branch, rebuild the changed packages with the commands above and install the APKs on the phone with `apk add ./package.apk`. A kernel update regenerates `/boot/boot.img` on the phone: copy it to your computer and write it with `fastboot flash boot_a`. A device-package update needs no flashing.

## Recovery

| Situation | What to do |
| --- | --- |
| Linux does not boot | Hold Power + Volume Down for fastboot, then flash the last working `boot.img` of **this** installation to `boot_a`. A boot image from another installation names a different root filesystem and will not boot. |
| A kernel update broke something | Install the previous `linux-xiaomi-chopin` APK and flash the `/boot/boot.img` it regenerates. |
| The root filesystem is broken | Flash `xiaomi-chopin.img` to `userdata` again (this erases it), then restore your data. |
| Disable hardware video decoding or GNSS | `rc-update del xiaomi-chopin-vpud default` or `rc-update del xiaomi-chopin-mnld default`. A device-package update enables them again. |
| Go back to Android | Flash the official ROM with MiFlash using **"clean all"**, never "clean all and lock". Flashing only the stock `boot.img` is not enough, because `userdata` holds Linux. |

The port writes only `userdata` and `boot_a`. The factory partitions (`nvram`, `nvdata`, `nvcfg`, `persist`, `proinfo`, `protect1`, `protect2`) are only ever mounted read-only, and every calibration value comes from your own phone at runtime.

## How it works

- **Calibration:** OpenRC services mount the factory partitions read-only and without journal replay, load the Wi-Fi, Bluetooth address, speaker and light-sensor calibration, and unmount them again.
- **Stock vendor daemons:** hardware video decoding (`vpud`) and GNSS (`mnld`) need MediaTek's proprietary daemons. They run unmodified from the phone's own stock partitions. The `system`, `vendor` and `system_ext` partitions are mapped read-only out of `super` with device-mapper. Each daemon runs as its own unprivileged user in a private mount namespace with only the capabilities its service names. A small resolver service answers the daemons' Android-style DNS requests. Nothing proprietary is packaged.
- **Graphics:** a fork of Mesa with a backend for MediaTek's vendor Mali driver, installed in `/usr/lib/chopin-gpu` and selected only for this device.

## Upstream status

This branch is a working port, not yet a merge request. Before Nura could accept it:

- The firmware recipe must not depend on a manual step. Either load the firmware from the phone's stock vendor partition at runtime, which the port already maps read-only, or download it from a public repository.
- The general fixes should go to their own projects: the GStreamer shader fix, Phoc's two fixes, PipeWire's launcher fix, starting polkit as its own user, and the Stevia schemas dependency. The bubblewrap build for Linux 4.14, the sensor-hub backend and the private Mesa build only serve downstream kernels; they need the maintainers' agreement, or they stay out of the official port.
- pmaports' CI currently reports:
  - `deviceinfo_kernel_cmdline` is obsolete; the boot command line moves to `kernel-cmdline.d`;
  - new packages must start at `pkgrel=0`;
  - five deviceinfo warnings about default values and the reference URL.
  Lint of the device, kernel and firmware recipes is otherwise clean, and all recipe checksums verify.

## Licensing

The recipes and patches on this branch follow pmaports (GPL-3.0); the kernel patches are under the kernel's GPL-2.0 and the Mesa patches under Mesa's MIT licence; the port's own scripts and programs are MIT. The firmware and the stock vendor daemons are Xiaomi's and MediaTek's proprietary software. They are not included: each user extracts or uses them from their own phone.
