<!--
SPDX-License-Identifier: CC0-1.0
SPDX-FileCopyrightText: none
-->

# AGENTS.md

Guidance for AI coding agents working on Karton. This file is deliberately
short; the detailed reference lives in `docs/`.

## What Karton is

Karton is a Libvirt-based virtual machine manager for KDE: C++23, Qt 6/KF6 and
a Kirigami QML UI. It lists local libvirt domains (one collapsible group per
connection), defines new domains from installation media and embeds a SPICE
display for the VM console.

The primary libvirt URI is auto-detected (`qemu:///system`, then
`qemu:///session`) or set with `connection/uri` / `KARTON_LIBVIRT_URI`.
Additional connections live in `connections/extraUris`. A domain UUID is unique
only inside one connection, so never route operations by UUID alone. Always
pass `-c <the same URI>` to `virsh` when developing or testing.

## Where things live

- `main.cpp` — entry point, `KAboutData`, QML module loading.
- `core/app/` — `Karton` singleton, `VMModel`, additional connections.
- `core/domain/` — `Domain`, `DomainConfig`, XML builder/reader.
- `core/libvirt/` — libvirt event loop, reader thread, lifecycle monitor.
- `core/viewer/` — SPICE viewer, renderers, terminal, clipboard, audio.
- `core/os/`, `core/util/` — libosinfo and small helpers.
- `qml/` — Kirigami pages and dialogs.
- `tests/`, `scripts/guests/`, `locales/`, `img/os/`.

New C++ files go into the matching `core/` subsystem; includes use the
subsystem prefix (`#include "domain/domain.h"`, `core/` is an include root).
New QML files must be added to `ecm_target_qml_sources()` in `CMakeLists.txt`.
See [docs/architecture.md](docs/architecture.md#repository-layout) for the full
table.

## Build and tests

Required: CMake >= 3.20, C++23, Qt 6 (with Linguist tools), KF6 (Kirigami,
CoreAddons, QQC2DesktopStyle, IconThemes), Kirigami Addons >= 1.11, libvirt,
libosinfo, GLib, spice-client-glib, phodav, PipeWire, OpenGL/EGL and ECM >= 6.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
./build/bin/karton
```

Autotests are opt-in and must be run after QML, CMake or C++ changes:

```sh
cmake -S . -B build -G Ninja -DKARTON_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

`tst_qml_startup` loads the whole QML module offscreen, so it catches missing
imports and invalid properties. A manual smoke test is still required for UI
changes; use a throwaway `agent-*` domain and clean it up. Static analysis,
sanitizer builds and the full smoke-test recipe are in
[docs/development.md](docs/development.md).

## Code conventions

- C++23, Qt 6/KF6; prefer Qt/KF6 classes and `QStringLiteral` over `const char *`.
- Keep the SPDX header of the file you edit; do not remove copyright lines.
- Run `clang-format -i` on touched C++ files (KDE style is enforced).
- User-visible strings are translation keys: `qsTr("key")` in QML and
  `QCoreApplication::translate("Context", "key")` in C++. Add every key to both
  `locales/en.ts` and `locales/ru.ts`.
- Use `KARTON_DEBUG` (`qCInfo`/`qCWarning`) instead of raw `qDebug()`.
- Use monochrome `*-symbolic` icons in actions and buttons.
- Keep changes focused: no unrelated reformatting, no `build/` artifacts.

## UI conventions

Every settings page, dialog, wizard page and device form follows the same
Karton style; do not invent a separate layout.

- Page content is wrapped in `ScrollableSettingsView`; 16 px spacing, 24 px
  page margins, 15 px bold section labels with a 1 px rule.
- Two-column `GridLayout` rows: label left with `Layout.fillWidth`, control
  right with `Layout.preferredWidth: 260`. Right-column text is right-aligned.
- Checkboxes sit on the right of their label row; form buttons are right-aligned
  with symbolic icons.
- Dialogs derive from `qml/KartonDialog.qml` and do not redeclare `background`.
  Long content scrolls inside a fixed-height dialog; footer actions are
  `customFooterActions` (Cancel/Close first, primary last).
- Optional/singleton devices are Settings toggles; only multi-instance devices
  belong in `AddHardwareDialog.qml`. The Settings "Add…" button is the only add
  path.
- After UI changes, compare with the existing Settings/Add Hardware screens and
  run `ctest`.

Full layout rules and page inventory:
[docs/ui-guidelines.md](docs/ui-guidelines.md).

## Architecture: the rules that bite

- **`Domain` objects are owned by `Karton`** (QObject children) and own their
  `virDomainPtr`. QML keeps raw pointers: never delete a `Domain` while a page
  may still use it; use `deleteLater()` and clear references first.
- Use `Domain::isActive` / `Domain::isPaused`, not the localized
  `DomainConfig::state` string.
- `virConnectListAllDomains()` returns referenced `virDomainPtr`s; Karton's
  `Domain` objects own those references. `virDomainGetXMLDesc()` must be freed
  with `free()`.
- Blocking libvirt reads run on the `LibvirtReader` thread; only plain data may
  cross the boundary — never `Domain *` or `virDomainPtr`. `refreshDomainStates()`
  heals missed events; never put a full `refreshDomainList()` on a timer.
- The libvirt connection opens after the main window's first frame. Never call
  `init()` from the constructor.
- Do not block the GUI thread (libvirt, `QProcess`, ACL/stat). Use
  `ScopedCallTimer` for known blockers and `CommandRunner` timeouts.
- Preserve unknown domain XML and metadata; `DomainXmlBuilder::updateXML()` edits
  only specific fields. Disk shrinking is unsupported; use `qemu-img` with `-U`
  while a VM may hold the image.
- The viewer owns exactly one SPICE connection per VM: the hidden preview window
  is promoted to a real viewer, never duplicated or destroyed at runtime.
- The primary renderer keeps one persistent RHI texture, uploads only damaged
  rectangles, and never calls `createTextureFromImage()` per frame.
- SPICE cursor: client mode installs the guest shape as the host cursor and
  keeps the floating toolbar usable. Windows back/forward buttons are translated
  to Alt+Left/Alt+Right in the viewer because the agent mouse path may drop
  XBUTTON1/2; do not switch Windows domains to server mouse mode. External
  server-mode domains use the overlay fallback, but Karton never generates it.
  Input is sent only when the inputs channel is ready.
- Request the full viewer area from the guest agent, not the aspect-fitted item
  size; fixed per-VM resolutions are windowed-only. Console streams use
  `VIR_STREAM_NONBLOCK` and deferred teardown.
- While the pointer is over a running guest, keyboard shortcuts are inhibited
  (Wayland protocol/X11 grab plus kglobalaccel). Release them on deactivation,
  stop, close and exit; never SIGKILL a test instance with a viewer open.
- Remote connections keep disks on the remote host; clone/OVF import targets are
  local-only. Validate per-connection networks and pools on the selected URI.
- QML dialogs stay open while async work is in flight; never close them
  optimistically after calling `Karton`.
- Suspend/undefine/define events can arrive while a page is open; check domain
  pointers and missing files gracefully.

The full list, including device hotplug, storage pools, OVF, clone, DHCP,
networks, settings pages, clipboard, audio and the scene graph, is in
[docs/architecture.md](docs/architecture.md).

## Git workflow

- `origin` is the personal fork `https://github.com/B612S/karton`; `main` is the
  working branch. `upstream` is `https://github.com/kenoi1/Karton`.
- Push work to `origin/main` only; never push to `upstream`.
- Write imperative, descriptive commit messages.
- Before committing, build the project; for UI changes also run the manual
  smoke test.
