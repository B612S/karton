<!--
SPDX-License-Identifier: CC0-1.0
SPDX-FileCopyrightText: 2024 Aaron Rainbolt <arraybolt3@gmail.com>
SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
SPDX-FileCopyrightText: 2026 Karton contributors
SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>
-->

# Karton

A libvirt-based virtual machine manager for KDE.

Karton is built with C++23, Qt 6, KF6 and Kirigami. It manages local libvirt
domains, creates new VMs from installation media, and embeds a SPICE console
with a desktop-oriented viewer.

> **Early development:** Karton is still evolving and may change domain XML or
> break existing VMs. Back up important disks before using it with production
> machines.

## Features

### Virtual machines

- List local libvirt domains with their OS icon and human-readable OS name.
- Search and filter the list by name, OS, UUID or running state.
- Import a VM from libvirt domain XML or from an OVF/OVA package (into a
  folder or a storage pool); export it as domain XML, an OVF folder or an OVA
  archive.
- Create VMs from an ISO or disk image.
- Start, shut down, power off, pause, resume, reboot and reset.
- Autostart on boot.
- Hibernate and restore a running VM through libvirt managed save.
- Clone a VM, copy its disks and regenerate UUID/MAC addresses.
- Forward TCP ports from the host to a guest, with rules stored per VM.
- Take snapshots, revert to them and delete them.
- Delete a VM together with its local disks when requested.
- Live CPU and memory usage on the VM page.
- A New Year easter egg between 25 December and 7 January: Tux stands in a
  snow drift.

### SPICE viewer

- Embedded SPICE display with keyboard and pointer capture.
- Split power actions: Shut Down/Start plus a separate menu for Power Off,
  Reboot plus Reset, and Pause as a direct action.
- Fullscreen mode and a More menu for display, USB, snapshots and Settings.
- Live VM previews on the VM page.
- Per-VM fixed display resolution, including custom resolutions.
- Optional guest resolution auto-fit while the VM is running.
- Separate per-VM sound output and sound input controls.
- Runtime USB and PCI passthrough from the viewer.
- Opening the same VM twice focuses the existing viewer instead of creating a
  duplicate.

### Settings

Settings is a Luma-style overlay attached to the window that owns the VM.

VM pages include:

- Overview
- CPUs & Memory
- Boot Options
- Disks and CD-ROMs
- Network interfaces
- Display and video
- Sound
- USB
- PCI
- TPM
- Filesystem shares
- Input devices
- Controllers
- Serial & Console
- Remaining XML devices
- Clipboard
- Domain XML editor

The XML editor is read-only by default, has an explicit Edit switch, syntax
highlighting and scrolls independently of the page. Long lines wrap visually
without changing the XML text. Its header contains an inline XML search field
with a match counter: Ctrl+F focuses it, Enter/Shift+Enter jump between matches
and Esc clears it.

**Add…** opens a grouped Add Hardware dialog for:

- storage, disk images and ISO images;
- network interfaces;
- input devices;
- USB and PCI passthrough;
- controllers, serial ports and consoles;
- VirtIO channels;
- USB redirection;
- filesystem shares (9p/virtiofs).

TPM is configured on the VM's TPM Settings page. Watchdog, RNG, panic,
VSOCK and shared memory (IVSHMEM) are enabled from the VM's Other devices page.
Filesystem shares are added from Add Hardware and listed on the Filesystem page.
Clipboard text and file synchronization modes are configured on the Clipboard
page; text sync needs spice-vdagent in the guest. File clipboard from host to
guest additionally needs the `org.spice-space.webdav.0` SPICE channel.

### Mounting filesystem shares

Use the mount tag configured for the share (for example `share`).

Linux with 9p:

```sh
sudo mkdir -p /mnt/share
sudo mount -t 9p -o trans=virtio,version=9p2000.L share /mnt/share
```

Linux with virtiofs:

```sh
sudo mkdir -p /mnt/share
sudo mount -t virtiofs share /mnt/share
```

To mount at boot, add an `/etc/fstab` entry with the matching filesystem
type and the `_netdev` option.

Windows has no native 9p support. For virtiofs, install the VirtIO FS driver
from virtio-win and WinFsp, then use the mount tag in the VirtIO FS client.
Alternatively use a Windows/SMB share. macOS and BSD guests need a matching
guest driver; 9p support depends on the guest kernel.

Global Application Settings provide:

- the libvirt connection URI;
- SPICE TLS: prefer TLS when the VM exposes a TLS port, and the CA certificate path;
- shared libvirt networks (start/stop, autostart, create NAT, delete);
- DHCP leases and static reservations for libvirt networks;
- the default VM image folder and per-connection storage pools;
- serial-console font, font size, color scheme and cursor shape;
- XML editor font, font size and color-scheme presets;
- a Maintenance page that cleans stale data for deleted VMs;
- About information.

"Start on boot" marks the VM for libvirt autostart and also enables autostart
for the VM's networks and storage pools. On a system connection, enable the
libvirt service at boot once (for example
`sudo systemctl enable --now libvirtd.service` or the matching
`virtqemud.service`); Karton shows this command in the VM settings when the
service is not enabled yet.

## Guest tools

Karton never downloads or runs anything inside a guest. The guest installers
live in this repository and install the recommended packages themselves from
inside the guest, so the guest needs internet access:

```sh
# Linux guest
sh scripts/guests/linux/install.sh
```

```bat
rem Windows guest (asks for administrator rights)
scripts\guests\windows\install.cmd
```

`install.sh` uses the guest's package manager (apt, dnf, yum, pacman, zypper or
apk) to install the QEMU guest agent, spice-vdagent, spice-webdavd, the Mesa
virtio-gpu drivers and a serial console getty. `install.cmd` downloads the
latest virtio-win guest tools (drivers, QEMU guest agent, SPICE agent), falls
back to the VirtIO driver MSI, installs WinFsp for VirtIO FS shares and offers
to restart Windows.

## Requirements

- Linux with KVM and libvirt
- C++23 compiler
- CMake >= 3.20
- Qt 6
- KF6: Kirigami, Kirigami Addons, CoreAddons, QQC2 Desktop Style,
  Icon Themes
- Qt Linguist tools (`qt6-tools` on Arch) for building the translations
- libvirt
- libosinfo
- GLib
- spice-client-glib
- phodav
- PipeWire
- OpenGL (EGL) for the scene graph and the SPICE display
- Extra CMake Modules

### Arch Linux

The repository contains a `PKGBUILD`:

```sh
cd ~/Projects/karton
makepkg -si
```

Development packages:

```sh
sudo pacman -S --needed base-devel cmake ninja extra-cmake-modules \
    qt6-base qt6-declarative qt6-multimedia qt6-tools \
    kirigami kirigami-addons kcoreaddons kiconthemes qqc2-desktop-style \
    libvirt libosinfo spice-gtk phodav pipewire mesa
```

## Build from source

```sh
git clone https://github.com/B612S/karton.git
cd karton
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Run the development build:

```sh
./build/bin/karton
```

Install system-wide:

```sh
sudo cmake --install build
```

The application icon is installed as `org.kde.karton.svg` under
`hicolor/scalable/apps`.

## Configuration

Karton auto-detects the libvirt connection:

1. a stored URI is tried first;
2. otherwise `qemu:///system` is tried;
3. then `qemu:///session`.

If the selected URI has no domains, Karton tries the other local URI. The active
URI is shown on the empty VM list.

The URI can be changed in **Application Settings**; a restart is required.
`KARTON_LIBVIRT_URI` overrides both the stored value and auto-detection:

```sh
KARTON_LIBVIRT_URI=qemu:///system ./build/bin/karton
```

Karton stores its settings in:

```text
$XDG_CONFIG_HOME/karton/karton.conf
```

That includes the connection URI, main-window size, per-VM viewer size,
per-VM display resolution and audio preferences.

### Rendering

The scene graph uses OpenGL (EGL) with a 1-frame swap interval; on Wayland the
compositor provides the presentation sync. Vulkan is intentionally not used:
Qt 6.12's Vulkan/Wayland swapchain stops presenting after the first fullscreen
transition on NVIDIA, which left the viewer window without updates.

The software renderer remains available for headless/offscreen use:

```sh
KARTON_GRAPHICS_API=software ./build/bin/karton
```

## Testing

Tests are opt-in, modeled on Luma: they build as separate executables, are never
linked into the application and are never installed with the package.

```sh
cmake -S . -B build -G Ninja -DKARTON_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The unit tests run offscreen and do not require libvirt. For UI changes also run
the application manually and exercise the affected pages with a throwaway VM.

## Project layout

| Path | Purpose |
| --- | --- |
| `main.cpp` | Application entry point. |
| `core/app/` | `Karton` singleton, VM list model and application services. |
| `core/domain/` | Domain wrapper, configuration and XML read/write. |
| `core/viewer/` | SPICE viewer and display renderers. |
| `core/libvirt/` | libvirt event loop and lifecycle monitor. |
| `core/os/` | libosinfo integration and OS icon mapping. |
| `core/util/` | Small helpers, including XML syntax highlighting. |
| `qml/` | Kirigami pages and dialogs. |
| `img/os/` | Bundled flat OS family icons. |
| `locales/` | Qt `.ts` translation sources (English and Russian). |
| `tests/` | Optional QtTest unit tests. |
| `scripts/guests/` | Standalone guest installers (`linux/install.sh`, `windows/install.cmd`). |

## Contributing

- Use C++23 and Qt/KF6 APIs.
- Keep UI strings translatable: use `qsTr("key")` in QML and
  `QCoreApplication::translate("Context", "key")` in C++, then add the key to
  both `locales/en.ts` and `locales/ru.ts`.
- Run `clang-format` on touched C++ files.
- Keep user-visible changes tested with a disposable VM.
- Never test destructive operations on the user's production VMs.

See `AGENTS.md` for the short engineering guide and `docs/` for the detailed architecture, development and UI notes.

## Localization

UI strings are stored as translation keys, not English source text. The sources
are `locales/en.ts` and `locales/ru.ts`; `qt_add_translations()` compiles them to
`en.qm`/`ru.qm`, embeds them in the binary and installs them to
`share/karton/translations` for distribution packages.

The language is stored as `ui/language` in `karton.conf`:

- `auto` follows the first two letters of the system locale (`ru` selects
  Russian, every other locale falls back to English);
- `en` and `ru` force the language.

It can be changed at runtime in **Application Settings → Application →
Interface → Language**; Karton reloads the translator and retranslates the QML
without a restart. The translator is looked up in `:/i18n/<lang>.qm` first, then
in `share/karton/translations`, then next to the executable.

To add a language:

1. create `locales/<lang>.ts` (use `en.ts` as the key template);
2. add it to `qt_add_translations(karton TS_FILES ...)` and the `install(FILES
   ...)` list in `CMakeLists.txt`;
3. add its native name to the language combo box in
   `qml/ApplicationSettingsDialog.qml`;
4. extend `Karton::resolveLanguage()` and `Karton::setLanguage()` for the new
   code.

## Links

- Repository: <https://github.com/B612S/karton>
- Upstream: <https://github.com/kenoi1/Karton>
- Matrix: <https://matrix.to/#/#karton:kde.org>

## License

Karton is licensed under **GPL-3.0-or-later**. The icon and documentation are
covered by the licenses declared in their SPDX headers. Bundled OS family icons
are derived from Papirus and keep their original GPL-3.0-or-later licensing.
