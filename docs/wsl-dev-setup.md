# Linux testing in WSL2 on Windows 10

How the Arch and Ubuntu test distros on the development PC are set up, and the problems that shaped it. WSL 3.0.3 or newer is needed: version 2.1.5 couldn't start a VM at all here (`HNS 0x800706ba`).

## Importing the distros

The official images are imported under their own names, so Docker Desktop stays the default distro. Check each image against its published SHA-256 first.

```bash
wsl --import Papertrail-Arch %USERPROFILE%\WSL\Papertrail-Arch archlinux.wsl --version 2
```

```bash
wsl --import Papertrail-Ubuntu %USERPROFILE%\WSL\Papertrail-Ubuntu ubuntu-noble.rootfs.tar.gz --version 2
```

- **Arch image:** `https://fastly.mirror.pkgbuild.com/wsl/latest/archlinux.wsl`. Run `pacman-key --init` and `pacman-key --populate archlinux` before the first install.
- **Ubuntu image:** `https://cloud-images.ubuntu.com/wsl/releases/24.04/current/ubuntu-noble-wsl-amd64-wsl.rootfs.tar.gz`.

## Networking problems on this PC

NAT networking fails, so WSL falls back to its `Consomme` mode. A Private Internet Access VPN (`wgpia0`) is active on this PC and may be related, but that's untested.

- **Port 53 to public DNS servers is blocked**, but HTTPS works.
- **UDP replies to `127.0.0.1` get lost.** WSL's routing sends that address through `loopback0`. TCP to `127.0.0.1` works.
- **All WSL distros share one network**, so two can't bind the same address and port.

### The fix: a small DNS over HTTPS resolver

Each distro runs `tools/wsl/papertrail-doh.py` as a systemd service, on its own loopback address with its own route, so DNS traffic stays on the real `lo`. It forwards lookups to Cloudflare over HTTPS and verifies the certificate.

| Distro | Address |
|---|---|
| Arch | `127.0.53.1` |
| Ubuntu | `127.0.53.2` |

`/etc/resolv.conf` contains `nameserver <address>`, and `/etc/wsl.conf` sets `generateResolvConf=false` under `[network]`.

The service unit:

```ini
[Service]
ExecStartPre=-/usr/sbin/ip addr add <address>/32 dev lo
ExecStartPre=/usr/sbin/ip route replace local <address>/32 dev lo table local src <address>
ExecStart=/usr/bin/python3 -I /usr/local/bin/papertrail-doh.py <address>
Restart=on-failure
```

`dnscrypt-proxy` was tried first but looked broken only because of the UDP problem above. It isn't needed.

## Packages

Downloads can stall on these mirrors, so use `--disable-download-timeout` and `ParallelDownloads = 2` with pacman.

- **Arch:** `base-devel cmake git ninja python qt6-base qt6-wayland libmupdf djvulibre xcb-util-cursor`
- **Ubuntu:**
  - **Qt 6.12:** install with aqtinstall into `/opt/Qt`, using `aqt install-qt linux desktop 6.12.0 linux_gcc_64 --outputdir /opt/Qt`.
  - **MuPDF 1.28.5:** build from source with `make build=release shared=yes prefix=/usr/local HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no HAVE_LEPTONICA=no HAVE_TESSERACT=no libs install-libs`.
  - **Everything else:** from apt, including `libdjvulibre-dev`, `djvulibre-bin` and Qt's runtime libraries (`libegl1`, `libxkbcommon0`, `libxcb-cursor0` and the other `libxcb-*` packages).

## Building

Keep build output in the Linux filesystem rather than on `/mnt/c`:

```bash
cmake --preset linux-debug -B ~/papertrail-build -DPAPERTRAIL_BUILD_FORMAT_PROBE=ON
```

On Ubuntu, add `-DCMAKE_PREFIX_PATH=/opt/Qt/6.12.0/gcc_64`. Calling this through `wsl.exe` from Windows PowerShell splits that argument at the dot, so run it from a script or a Linux shell.
