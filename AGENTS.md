<!--
SPDX-License-Identifier: CC0-1.0
SPDX-FileCopyrightText: none
-->

# AGENTS.md

Guidance for AI coding agents working on Karton.

## What Karton is

Karton is a Libvirt-based virtual machine manager for KDE. It is a C++23 / Qt 6 /
KF6 application with a Kirigami UI. It lists and manages local libvirt domains,
defines new domains from OS installation media, and embeds a SPICE display for
the VM console.

Karton supports several libvirt connections at once and shows one collapsible
group per connection in the VM list. A libvirt domain UUID is only unique
inside one connection, so the same UUID can legitimately appear in two groups
(for example a session copy of a system VM); these are independent
definitions, share per-UUID Karton settings, and must not run at the same
time if they point at the same disk. The **primary** connection is
auto-detected in `Karton::init()`: without an explicit setting it tries
`qemu:///system` first, then `qemu:///session`, and selects the first
connection that actually has domains. The primary URI can be changed in
Application Settings → Connections (requires a restart) or with
`KARTON_LIBVIRT_URI`, which takes precedence. Additional connections are kept
in `connections/extraUris` and are added, edited and removed live on the same
settings page; on a fresh profile Karton offers the other local URI as the
first additional connection. The primary URI is stored under `connection/uri`
in `$XDG_CONFIG_HOME/karton/karton.conf`; collapsed groups are stored under
`connections/collapsed`. Any `virsh` command used during development or testing
must pass `-c <same URI>` (usually `-c qemu:///system`).

## Repository layout

| Path | Purpose |
| --- | --- |
| `main.cpp` | Entry point; sets up `KAboutData` and loads the QML module. |
| `core/app/karton.{h,cpp}` | `Karton` QML singleton: libvirt connections, domain list, VM lifecycle operations, paths. |
| `core/app/libvirtconnection.{h,cpp}` | Additional libvirt connection group: its handle, reader handle, reconnect loop, lifecycle monitor and VM list. |
| `core/domain/domain.{h,cpp}` | QML-exposed `Domain` wrapper around a `virDomainPtr`. |
| `core/domain/domainconfig.{h,cpp}` | QML-exposed per-domain configuration/state model. |
| `core/app/vmlistmodel.{h,cpp}` | `VMModel`, the QML singleton list model shown on the main page. |
| `core/domain/domainxmlbuilder.{h,cpp}` | Generates and edits libvirt domain XML. |
| `core/domain/domainxmlreader.{h,cpp}` | Parses libvirt's domain XML for Karton metadata and disk paths. |
| `core/os/osinfoconfig.{h,cpp}` | libosinfo wrapper (OS list, media detection, architecture). |
| `core/os/osiconmapper.{h,cpp}` | Maps libosinfo OS IDs to flat family icons and display names. |
| `img/os/` | Bundled flat OS family icons installed into hicolor. |
| `locales/` | Qt `.ts` translation sources (`en`, `ru`), key-based. |
| `core/viewer/domainviewer.{h,cpp}` | SPICE client `QQuickItem` used by the VM viewer window. |
| `core/viewer/previewitem.{h,cpp}` | Painted item that displays the latest in-memory preview frame. |
| `core/viewer/glscanoutrenderer.*`, `core/viewer/primarysurfacerenderer.*` | SPICE display rendering backends. |
| `core/viewer/terminalview.*` | VT100/xterm terminal widget for the libvirt serial console. |
| `core/libvirt/libvirtmonitor.*`, `core/libvirt/libvirteventloop.*` | libvirt lifecycle-event monitoring. |
| `core/util/commandrunner.*` | Small `QProcess` helper used for `qemu-img`. |
| `core/util/xmlhighlighter.*` | Theme-coloured XML syntax highlighter for the VM XML editor. |
| `qml/` | Kirigami QML pages and dialogs (including the per-connection `ConnectionSettingsDialog`/`NetworksPage`). |
| `tests/` | Opt-in QtTest unit tests; never linked into the application. |
| `scripts/guests/` | Standalone guest installers (`linux/install.sh`, `windows/install.cmd`) that download and install the guest agents themselves. |
| `src/` | Reserved for makepkg's `$srcdir`; it is not used for source files. |

C++ sources are grouped by responsibility under `core/`:

- `core/app/` — `Karton` singleton and the VM list model. The application entry
  point lives in the repository-root `main.cpp`, matching Luma.
- `core/domain/` — domain state, configuration, XML builder and reader.
- `core/libvirt/` — libvirt event loop and lifecycle monitor.
- `core/viewer/` — SPICE viewer and display renderers.
- `core/os/` — libosinfo integration.
- `core/util/` — small helpers such as the command runner and DPI/keyboard utilities.

New C++ files must go into the matching subsystem directory; create a new
subsystem only when none of the existing groups fits. The one exception is the
application entry point, which stays in the repository-root `main.cpp`. Include headers with the
subsystem prefix, for example `#include "domain/domain.h"`; `core/` is an
include root. QML stays in the top-level `qml/` directory.

QML sources are compiled into the binary by `ecm_add_qml_module()` under the URI
`org.kde.karton`. When adding a QML file, add it to the `ecm_target_qml_sources()`
list in the root `CMakeLists.txt`, otherwise it will not be bundled.

## Build

Required: CMake >= 3.20, a C++23 compiler, Qt 6 (including Linguist tools `qt6-tools`), KF6 (Kirigami,
CoreAddons, QQC2DesktopStyle, IconThemes), Kirigami Addons >= 1.11, libvirt,
libosinfo, GLib, spice-client-glib, phodav, PipeWire, OpenGL/EGL (scene graph and
SPICE scanout), and Extra CMake Modules (ECM) >= 6.0.

On Arch Linux, the relevant packages are roughly:

```sh
sudo pacman -S --needed base-devel cmake ninja extra-cmake-modules \
    qt6-base qt6-declarative qt6-multimedia qt6-tools \
    kirigami kirigami-addons kcoreaddons kiconthemes qqc2-desktop-style \
    libvirt libosinfo spice-gtk phodav pipewire mesa acl wayland wayland-protocols
```

Configure and build out-of-tree:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
./build/bin/karton
```

For an AddressSanitizer/UndefinedBehaviorSanitizer build, configure a
separate tree with `-DKARTON_SANITIZE=ON` (for example `build-asan`) and run
the tests with `ASAN_OPTIONS=detect_leaks=0` (Qt and libvirt produce a lot of
intentional process-lifetime allocations). The `build-*` trees are ignored.

The `build/` directory is git-ignored. If CMake cannot find ECM, either install
`extra-cmake-modules` or point CMake at a local installation, e.g.
`-DCMAKE_PREFIX_PATH="$HOME/.local"`.

Run the app in a graphical session. For automated GUI testing under Wayland it
is often easier to start it on XWayland so that `xdotool` can drive it:

```sh
QT_QPA_PLATFORM=xcb ./build/bin/karton
```

Screenshots can be taken with `spectacle -b -n -f -o /tmp/karton.png`.

## Tests and manual smoke test

Autotests are opt-in, like Luma: they are separate executables built from a
private `karton_testcore` archive, are never linked into `karton`, and are
never installed with the package.

```sh
cmake -S . -B build -G Ninja -DKARTON_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Static analysis is opt-in and runs in a separate build tree:

```sh
cmake -S . -B build-analyze -G Ninja \
    -DKARTON_BUILD_TESTS=ON \
    -DKARTON_CLANG_TIDY=ON \
    -DKARTON_SANITIZE=ON
cmake --build build-analyze -j"$(nproc)"
```

`KARTON_CPPCHECK=ON` adds cppcheck if it is installed. `scripts/analyze.sh`
does the same in one step. The clang-tidy wrapper removes the GCC-only
`-mno-direct-extern-access` flag; keep analyzer builds out of the normal
`build/` tree.

**Always run `ctest` after QML, CMake or C++ changes.** The opt-in suite
includes `tst_qml_startup`, which runs the real `karton --check-qml` path
offscreen to load and instantiate the complete QML module plus `QmlSmokeTest`,
which creates the runtime-only viewer and dialogs with a null domain. Missing
imports, invalid properties and unavailable component types therefore fail the
build/test gate.
Without `-DKARTON_BUILD_TESTS=ON` no test target is created. The unit tests run
without a display or libvirt connection, use `QT_QPA_PLATFORM=offscreen`, and
redirect `XDG_CONFIG_HOME`/`XDG_CACHE_HOME`/`XDG_STATE_HOME` into the build
tree. They cover the domain XML reader/builder (including malformed XML,
metadata preservation and disk escaping), `DomainConfig` state/signals and the
asynchronous/blocking `CommandRunner`. A build plus a manual smoke test is
still the baseline for UI changes. Use a throwaway domain so nothing important
is touched:

```sh
qemu-img create -f qcow2 ~/.local/share/libvirt/images/agent-test.qcow2 2G
virsh -c qemu:///session define /tmp/agent-test.xml   # minimal domain XML
virsh -c qemu:///session list --all
```

Start Karton, confirm the VM appears, open it, and exercise start, stop, config
edit, eject, and delete. Clean up afterwards:

```sh
virsh -c qemu:///session undefine agent-test
rm -f ~/.local/share/libvirt/images/agent-test.qcow2
```

When testing destructive operations, prefer domains whose names start with
`agent-` and never operate on the user's real VMs.

## Settings layout conventions

Every settings page must look and behave the same:

- The page title is rendered once by `SettingsDialog.qml` from the category
  list. Do not repeat it as a content heading.
- Content is wrapped in `ScrollableSettingsView`; do not use a bare
  `ColumnLayout` for a settings page.
- Page spacing is 16 px. Sub-sections use a 15 px bold label, followed by a
  `Kirigami.Separator` or a 1 px rule.
- Controls use two-column `GridLayout` rows: label on the left with
  `Layout.fillWidth`, control on the right with `Layout.preferredWidth: 260`.
- Checkboxes belonging to a setting stay on the right of their label row.
- Device lists fill the available width; avoid tiny fixed heights. Give them a
  sensible minimum and let them grow with their content.
- Optional/singleton devices are toggles in Settings, not Add Hardware
  actions. Only multi-instance devices belong in `AddHardwareDialog.qml`.
- New pages must be added to `ecm_target_qml_sources()` and must pass
  `tst_qml_startup`; run `ctest` after every QML change.

## UI style consistency

Every dialog, settings page, wizard page and device form must follow the same
Karton style; do not invent a separate layout for a new screen.

- Use the shared patterns: `ScrollableSettingsView` for scrollable content,
  16 px spacing, 15 px bold section labels with a 1 px rule, 24 px page margins.
- Use two-column `GridLayout` rows: label on the left with `Layout.fillWidth`,
  control on the right with `Layout.preferredWidth: 260`. Text values in that
  right column must use `horizontalAlignment: Text.AlignRight`; do not leave a
  value left-aligned inside its 260 px slot.
- Checkboxes belong on the right of their label row. Buttons that belong to a
  form are right-aligned and use symbolic icons.
- Keep dialog widths consistent between related dialogs. Long notes must wrap
  inside the dialog width instead of resizing the dialog; use an explicit
  `width` when `Kirigami.Dialog` would otherwise size itself to a long label.
- Every dialog derives from `qml/KartonDialog.qml`, the shared shell that owns
  the translucent background, 8 px radius, `largeSpacing` padding and the
  window-relative width. Do not redeclare `background` in a dialog. Override
  `preferredDialogWidth` when the content truly needs more room. Card-style
  screens that own a fixed header and their own margins (`SettingsDialog`,
  `AddHardwareDialog`, `ApplicationSettingsDialog`) may keep `padding: 0` and
  their `cardWidth`/`cardHeight`; every other dialog uses the shell defaults.
  The shell sets an explicit `width`, so a long path or libvirt message wraps
  inside the dialog instead of stretching it. Never use `preferredWidth` for
  this purpose: `Kirigami.Dialog` then grows to the content's unwrapped width.
- Content that can outgrow the window scrolls with `ScrollableSettingsView`,
  the same wrapper the Settings pages use, inside a custom `contentItem`; give
  the dialog an explicit `height` so its header and footer stay fixed and the
  page scrolls under them. Do not rely on the default Kirigami content
  ScrollView for a tall form. Short forms stay non-scrollable, and fixed-height
  lists (snapshots, USB, error log) keep scrolling internally.
- Footer actions are `customFooterActions`: Cancel/Close first, the primary
  action last, both with symbolic icons. Inline buttons stay in the content only
  for per-row actions such as Revert/Delete on a list entry.
- Long values (paths, UUIDs, disk targets) are elided or wrapped deliberately;
  never let them push the layout or clip controls.
- After UI changes, compare the result with the existing Settings/Add Hardware
  screens and run `ctest`.

## Code conventions

- C++23, Qt 6 / KF6 APIs. Prefer Qt/KF6 classes and `QStringLiteral` over
  `const char *` where practical.
- Every source file starts with SPDX headers. Most C++/QML sources are
  `GPL-3.0-or-later`; CMake files are `BSD-2-Clause`; documentation is
  `CC0-1.0`. A few files (e.g. `devicepixelratiohelper.*`) use
  `GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL`. Match the
  header of the file you are editing, and do not remove existing copyright
  lines.
- Formatting is enforced by the KDE clang-format configuration. Run
  `clang-format -i` on touched C++ files (the build wires up
  `kde_clang_format`).
- User-visible strings are key-based and translatable: use `qsTr("key")` in
  QML and `QCoreApplication::translate("Context", "key")` in C++. Add every new
  key to both `locales/en.ts` and `locales/ru.ts`; do not put English source
  text in the code as a translation key. Keep the TS list in
  `qt_add_translations()` and the `install(FILES ...)` list in `CMakeLists.txt`
  in sync when adding a language.
- Use the `KARTON_DEBUG` logging category (`qCInfo(KARTON_DEBUG)`,
  `qCWarning(KARTON_DEBUG)`, ...) rather than raw `qDebug()`.
- Use monochrome `*-symbolic` icon names in QML actions and buttons so the UI
  follows the theme; avoid colored icon names and duplicate icons for different
  actions.
- Keep changes focused. Do not reformat unrelated code or check in `build/`,
  editor files, or generated artifacts.

## Architecture notes and pitfalls

- **`Domain` objects are owned by `Karton`** (QObject children). They own their
  `virDomainPtr` and free it in `~Domain()`. QML pages keep raw `Domain *`
  pointers, so do not delete `Domain` objects while a page may still reference
  them; use `deleteLater()` and clear model/page references first.
- **Use `Domain::isActive` for running/stopped logic, not the `state` string.**
  `DomainConfig::state` is a localized display string (e.g. "running" in
  English, "работает" in Russian), so comparing it to an English literal breaks
  in other locales.
- `virConnectListAllDomains()` returns referenced `virDomainPtr`s. Karton's
  `Domain` objects take ownership of those references; don't add extra refs or
  frees without checking the surrounding code.
- `virDomainGetXMLDesc()` returns a `char *` that must be freed with `free()`.
- libvirt lifecycle events are delivered through `LibvirtEventLoop`, which
  registers a Qt implementation of the libvirt event API (`QSocketNotifier` for
  handles, `QTimer` for timeouts) before the hypervisor connection is opened.
  The event callback emits synchronously on the GUI thread; keep handlers cheap
  and do not block. libvirt timeouts are one-shot by contract, so the timers are
  `setSingleShot(true)` and are re-armed from `updateTimeout()`; never make them
  repeating.
- Events are the fast path, not the only source of truth. `Karton` registers a
  libvirt close callback (`virConnectRegisterCloseCallback`), and when the
  connection drops it tears the connection down (`detachConnection()`), retries
  every 3 seconds and rebuilds the model after reconnecting. In addition,
  `refreshDomainStates()` runs every 5 seconds and on application activation: it
  compares `virConnectListAllDomains()` state with the model and emits
  `domainsChanged` only for domains that actually drifted, so a missed
  lifecycle event heals within seconds instead of leaving a stopped VM shown as
  running. Never put a full `refreshDomainList()` on a timer — it recreates
  rows. `connected` is a `NOTIFY` property for the same reason.
- Blocking libvirt reads that are polled or can be slow run on the
  `LibvirtReader` thread (`core/libvirt/libvirtreader.*`), which owns its own
  connection opened and closed inside that thread. `Karton::postReaderTask()`
  queues `std::function` work; results come back through
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` and are applied on the
  GUI thread. Only plain data crosses the boundary — never `Domain *` or
  `virDomainPtr` owned by the model. `refreshDomainStates()` uses this path and
  falls back to the synchronous call when the reader is unavailable.
  `LibvirtEventLoop` is thread-safe: libvirt registers handles/timeouts from the
  thread that opens the connection, so the notifiers and timers are created
  parentless in that thread and the maps are mutex-protected. Host lists
  (`networks()`, `pciDevices()`, `usbDevices()`) are cached on the reader too and
  pre-warmed after connecting; `networksUpdated`/`pciDevicesUpdated`/
  `usbDevicesUpdated` refresh the dialogs. `SettingsDialog` calls
  `prefetchDomainXml()` first; `cachedDomainXml()` serves the live/inactive XML
  from the reader cache to `domainHardwareInfo`, `domainDisks`,
  `domainUsbDevices`, `domainPciDevices`, `domainXmlDevices` and
  `domainInputDevices`, falling back to one synchronous fetch only when the
  cache is empty. `refreshDomainList()` itself is collected on the reader and
  applied on the GUI (`applyDomainList()`), with a synchronous fallback only if
  the reader cannot list domains. `KARTON_NO_READER=1` disables the reader for
  debugging.
- The libvirt connection is opened by `Karton::start()` after the main window's
  first frame: `main.cpp` hooks `QQuickWindow::frameSwapped` (with a 2 s
  fallback) and `--check-qml` skips it. Never call `init()` from the
  constructor — a slow or unresponsive libvirt must not keep the window from
  appearing. `connecting`/`reconnecting` drive the connection group headers, so
  the first attempt and the retry loop are not shown as a connection error. A
  failed first connect starts the same 3 s reconnect timer as a dropped
  connection. Additional connections (`LibvirtConnection`) have their own
  probe, reader handle and reconnect loop.
- `refreshDomainList()` must not spawn `qemu-img` synchronously: a stuck image
  would freeze the GUI on every full refresh. Sizes come from the cache or from
  `scheduleDiskSizeQuery()`, which updates `DomainConfig` when the answer
  arrives.
- `DomainConfig::cpus`/`maxRam` are updated by both `refreshDomainList()` and
  `refreshDomain()`, so the details page follows lifecycle events and edits.
  `domainStats()` reports guest memory only while the domain is running, and
  `forgetDomainStats()` drops the cached CPU and memory samples on STOPPED —
  otherwise the VM page keeps showing the last RAM percentage of a domain that
  is no longer running.
- `CommandRunner::runCommand()` starts a program with an explicit argument list
  (no shell) and invokes a callback with the exit status and output. It does not
  support pipes, globbing, or shell escape sequences. Both `runCommand()` and
  `runCommandAndWait()` take `timeoutMs` (`0` means no limit): pass a timeout for
  short metadata commands (`qemu-img info` 10 s, `qemu-img create` 60 s) and `0`
  for long conversions. A timed-out command is killed and reported as a failure;
  a dialog that waits for the callback must not stay open forever.
- GUI-thread calls that can block (libvirt, `QProcess`, ACL/`stat` helpers) get a
  `ScopedCallTimer` from `util/calltimer.h`. `--verbose` logs every call, a call
  that takes 100 ms or more is logged as a warning: use those warnings to find
  the operation that freezes the window.
- Localization follows Luma: translations are stored as keys in
  `locales/en.ts`/`locales/ru.ts`, compiled by `qt_add_translations()` into
  embedded `:/i18n/<lang>.qm` resources and installed to
  `share/karton/translations`. The language is stored as `ui/language`
  (`auto`/`en`/`ru`); `auto` follows `QLocale::system().name().left(2)` and
  falls back to English. `Karton::language`/`setLanguage` drive
  `Karton::resolveLanguage()` and a `QTranslator`; `main.cpp` reloads it and
  calls `QQmlApplicationEngine::retranslate()` on change. QML must use `qsTr`
  and C++ `QCoreApplication::translate`, never KDE `i18n`/`i18nc`.
- The QML dialogs keep themselves open while an asynchronous create/edit is in
  flight; do not close them optimistically after calling `Karton`.
- `ApplicationSettingsDialog.qml` is the window-attached Luma-style global
  settings overlay. Its Application page selects the UI language and the SPICE
  defaults; its Console page sets the default serial-console font family,
  size and color scheme (`console/fontFamily`, `console/fontSize`,
  `console/theme`); its
  Editor page selects the XML editor font, font size and color-scheme preset
  (`editor/fontSize`, `editor/fontFamily`, `editor/theme`); its
  Connections page lists the primary and additional libvirt connections
  (edit/remove additional addresses, add new ones); its About page shows the
  application version and links. Host libvirt networks are per connection and
  live in `ConnectionSettingsDialog`, not in Application Settings. Its
  Maintenance page can run the stale-data cleanup for deleted VMs. Data for deleted VMs is cleaned automatically: runtime
  removals call `removeDomainCache()`, and a per-connection
  `knownDomains/<hash>` UUID list cleans VMs deleted while Karton was closed
  without affecting other libvirt connections.
- Libvirt network management lives on `Karton` (`networks(uri)`,
  `startNetwork(name, uri)`, `stopNetwork()`, `createNatNetwork()`,
  `attachNetwork()`, `detachNetwork()`). Host networks are per libvirt
  connection: the connection context menu opens
  `qml/ConnectionSettingsDialog.qml`, whose `qml/NetworksPage.qml` lists and
  edits exactly that connection's networks (`Add network`, DHCP, Edit,
  Start/Stop, Delete). `qml/SettingsDialog.qml` only manages the VM's own
  interfaces (attach/edit/detach); it lists the networks of the VM's
  connection via `Karton::domainConnectionUri()`. NIC attach uses separate LIVE and
  CONFIG libvirt calls so a running VM keeps the interface after shutdown; NIC
  detach removes the CONFIG device even if the guest cannot hot-unplug the live
  device. Interface rows show IP addresses obtained from the guest agent/lease
  and provide an Edit dialog for MAC/model changes.
- DHCP reservations are managed in `NetworkDhcpDialog.qml` and are bound to a
  VM interface: the VM picker lists only VMs that still have an unreserved
  interface in this network, and the interface picker then shows those
  interfaces by MAC. A VM with two interfaces stays listed until both are
  reserved. The IP field shows the fixed network prefix (`192.168.122.` for
  /24, `10.7.` for /16) and accepts only the host octets; `addNetworkDhcpHost()`
  validates subnet membership, rejects the network/broadcast/gateway addresses
  and duplicate MAC/IP entries, and derives the libvirt host name from the VM:
  the name is sanitized to ASCII (dnsmasq cannot use spaces) and gets a numeric
  suffix when one VM has several reservations, because libvirt requires unique
  host names. Reservation times are stored in
  `karton.conf` under `dhcpReservation/<network>/<mac>/addedAt` for the primary
  connection (additional connections add a per-URI scope directory) because
  libvirt does not record them, and are removed with the reservation. Note that
  `virNetworkUpdate()` takes the command before the section (see
  `libvirt-network.h`); passing the section first silently updates the wrong
  `<ip>` section.
- Attaching an already-present USB or PCI host device is rejected instead of
  creating duplicate `<hostdev>` entries. Device lists use the inactive XML so
  persistent changes are visible immediately on running domains.
- Runtime USB passthrough is available through `Karton::usbDevices()`,
  `domainUsbDevices()`, `attachUsbDevice()` and `detachUsbDevice()`, with the
  UI in `qml/UsbDevicesDialog.qml`. Host devices come from libvirt node devices
  (`usb_device` capability) and are attached as USB hostdevs.
- Snapshots are exposed through `Karton::domainSnapshots()` (reader-cached,
  `snapshotsUpdated`), `createSnapshot()`, `revertSnapshot()` and
  `deleteSnapshot()`, with the UI in
  `qml/SnapshotsDialog.qml`. A running VM with "Include memory" uses an internal
  snapshot with memory; without it, the backend creates an external disk-only
  snapshot with one qcow2 overlay per local data disk. External snapshots must
  be deleted newest-first: `deleteSnapshot()` omits the children flag for them so
  libvirt merges the overlay back instead of failing with "deletion of external
  disk snapshots with children not supported".
- Runtime PCI passthrough is exposed through `Karton::pciDevices()`,
  `domainPciDevices()`, `attachPciDevice()` and `detachPciDevice()`, with the
  UI inline in the USB & PCI page of `qml/SettingsDialog.qml`. Attach is only
  enabled for devices with an IOMMU group and the page warns that the device is
  detached from the host.
- VM cloning is implemented by `Karton::cloneDomain()`, with the UI in
  `qml/CloneDialog.qml`. It copies local file-backed disks with `qemu-img
  convert -U`, lets the user choose the destination folder, strips the UUID,
  MAC addresses, hostdevs, NVRAM and Karton metadata (but preserves
  libosinfo/OS metadata so the clone keeps its OS icon), and defines a stopped
  clone. A clone of a BitLocker VM gets a fresh TPM state and will require the
  BitLocker recovery key, so decrypt/suspend BitLocker before cloning. The copy runs asynchronously through
  `CommandRunner`; partial destination files are removed on failure.
- OVF/OVA export and import live in `core/app/karton_ovf.cpp` with
  `qml/OvfExportDialog.qml` and `qml/OvfImportDialog.qml`. Both write as the
  Karton user; the host QEMU permissions for the imported disks are applied
  automatically when the VM is defined/started. Export refuses to start when a
  disk image is not readable
  by Karton — a running system VM's image can still be owned by libvirt-qemu —
  and reports the exact image instead of qemu-img's duplicated error. Import
  always defines the VM stopped. Import can also target a file-backed storage
  pool (`dir`, `fs`, `netfs`): Karton creates a qcow2 volume through libvirt,
  converts each disk to a temporary local qcow2 and streams it into the volume,
  so system pools that are not writable by the user work too. OVA extraction runs on `QThreadPool`
  (`app/ovfextract.*`) and polls an atomic cancel flag, so a multi-gigabyte
  archive never blocks the GUI and can be stopped; parsing, disk conversion and
  the domain definition stay on the GUI thread. `tar --checkpoint-action=echo=%T`
  reports bytes read, matched against the total entry size from the earlier
  `-tvf` listing, so the dialog shows a real percentage (indeterminate when the
  listing has no sizes). `m_ovfActive` serialises imports, and
  `cancelOvfImport()` kills either tar or the current `qemu-img` through
  `CommandRunner::cancelCommand()`. Temporary directories are removed with a
  `PoolTask`, not `removeRecursively()` on the GUI thread.
- Disk/ISO hotplug is exposed through `Karton::domainDisks()`, `addDisk()`,
  `attachDiskImage()`, `attachIso()`, `setDiskMedia()` and `detachDisk()`, with
  the UI inline in the Disks page of `qml/SettingsDialog.qml`. New disks are
  created with `qemu-img create` and attached as virtio devices; ISO images are
  attached as read-only SATA cdroms. An empty CD-ROM row has an Insert… action
  (Change… when media is present); `attachIso()` reuses an existing empty
  CD-ROM instead of creating a second one. The Add Hardware Storage page starts
  with a Storage type selector (Existing image, CD-ROM, New disk) and shows
  only the matching form. Existing image/CD-ROM forms contain only a path
  picker; their primary Attach action lives in the fixed footer. New disk shows
  the folder/name/size/format/bus/cache/discard form and a Create disk footer
  action, so the action is always visible without scrolling.
  Disk edit options include an editable Path row (Media for CD-ROM) with a
  Change… picker, so an existing disk can be repointed without detaching and
  re-adding it. Local and remote disks can also be created in an active
  libvirt storage pool (`createStorageVolume` → `virStorageVolCreateXML`) from
  the wizard and Add Hardware, and repointed to an existing pool volume with
  the "From pool…" picker; the folder/file flow remains the default and its
  host permissions are applied automatically. Pool volumes skip the local
  file checks and are deleted through libvirt
  when the VM is removed with "Delete disk". For a running VM the path change is saved to the inactive
  configuration and applies on the next start; CD-ROM media changes are applied
  live. CD-ROM edit options are limited to Media and the always-enabled
  read-only flag; shareable, serial, cache and discard are not shown and
  `editDomainDiskAdvanced()` ignores them for an optical drive. The page uses
  FileDialog pickers for images/ISOs.
- The viewer discovers the SPICE URI from the domain XML
  (`virDomainGetXMLDesc()`), not by spawning `virsh`. Application Settings has a
  global SPICE TLS preference (`spice/tls`, default enabled) and a CA
  certificate path (`spice/caCertificate`). When the domain exposes `tlsPort`,
  the viewer connects with `tls_port`, sets SpiceSession `tls-port`, `ca-file`
  and the graphics password, and falls back to the plain port when TLS is
  disabled. If an external `virsh`
  call is ever needed, pass `-c <the same URI>` to match the application's
  active connection.
- Karton stores its own data inside domain XML metadata:
  `<karton:data maxDiskStorage="…"/>`. `maxDiskStorage` is stored in MiB.
  Unknown metadata attributes are preserved by `updateXML()` but ignored by
  Karton. Preview frames are memory-only and Karton never writes preview files.
  `DomainXmlReader` gets the XML from libvirt
  (`virDomainGetXMLDesc()`) rather than reading the config file on disk.
  Preserve unknown metadata when rewriting XML —
  `DomainXmlBuilder::updateXML()` intentionally edits only specific fields.
- Disk shrinking is not supported; `qemu-img resize` is only safe to grow.
- Local and remote storage pools are listed per connection
  (`storagePools(uri, includeInactive)`/`storageVolumes(pool, uri)`); the
  create wizard, Add Hardware (new disk, existing image and CD-ROM) and the
  disk edit dialog offer pool selection via "Use a storage pool" / "From
  pool…". Inactive pools are included and started automatically before a
  volume is created/listed. `startStoragePool()`, `stopStoragePool()`,
  `createStoragePool()` (directory pool) and `deleteStoragePool()` back the
  per-connection StoragePoolsPage in ConnectionSettingsDialog. Pool volumes
  are resolved with `virStorageVolLookupByPath`, so local file/ACL checks are
  skipped and "Delete disk" removes them with `virStorageVolDelete` on both
  local and remote connections. The volume picker shows capacity/format and
  filters ISO/image volumes for CD-ROM use. Clone/OVF/image-folder destinations
  are preflighted with `checkDirectoryWritable()` (current-user write access,
  no ACL); remote USB/PCI passthrough is rejected because host devices belong
  to the local host.
- `Karton::deleteDomain()` undefines with
  `VIR_DOMAIN_UNDEFINE_NVRAM | SNAPSHOTS_METADATA | CHECKPOINTS_METADATA`
  (plus `MANAGED_SAVE` when needed), so UEFI VMs with an NVRAM/varstore can be
  removed; older libvirt falls back to NVRAM-only and then legacy undefine.
  When the NVRAM file itself cannot be removed — it may be owned by
  libvirt-qemu or shared with a domain on another connection, e.g. a session
  copy of a system VM — `deleteDomain()` retries with
  `VIR_DOMAIN_UNDEFINE_KEEP_NVRAM` and keeps the file instead of failing; the
  definition is still removed. Model removal routes by the domain's own
  libvirt handle (`connectionForDomain()`), not by UUID, because a UUID is
  unique only within one connection.
  `DomainConfig::virtualDiskPaths` contains every `<disk device='disk'>` path
  found in the domain XML; deleting a VM with "Delete Disk" removes all of
  them. Creating a new VM still creates a single data disk, and size editing
  operates on the first disk (`virtualDiskPath`).
- The default VM image folder is `storage/imagesDirectory` in `karton.conf`,
  falling back to `$XDG_DATA_HOME/libvirt/images`. Application Settings →
  Storage changes it, the create-VM wizard can override the folder/name for a
  single VM, and Add Hardware's storage page can choose a folder/name for a new
  disk. Host permissions for local `qemu:///system` disks are applied
  automatically by `Karton::ensureDiskAccess(path, uri, readOnly)`: it grants
  the libvirt QEMU user (`libvirt-qemu`, falling back to `qemu`) traverse-only
  `x` on every parent directory and `rw` on the disk file (`r` for ISO/media),
  never write access to a directory, chmod or chown. Storage-pool volumes are
  skipped because libvirt manages their ownership. The ACL is applied on disk
  creation, attach and disk-path edit, and before every VM start (which also
  covers cloned/imported disks); changing the path to a moved image applies the
  same ACL to the new location.
  There is no permission dialog and no stored grant list; failures are reported
  as normal operation errors. Read-only disks receive `r` only. Session
  connections need no ACL (QEMU runs as the user) and remote connections are
  managed on the remote host; cloned disks are ensured after each copy,
  imported disks at VM start.
- New virtual disks can be created as `qcow2` (default), `raw` or `vmdk`.
  The format is stored in the domain XML `<driver type="…"/>` and exposed as
  `DomainConfig::diskFormat`; editing an existing domain does not convert its
  format.
- New domains can use BIOS or UEFI firmware (`DomainConfig::firmware`).
  UEFI sets `<os firmware='efi'>` and selects the non-Secure-Boot firmware
  variant via `<loader secure='no'/>` by default, because the default firmware
  descriptor otherwise rejects unsigned installers and guest drivers. An
  optional Secure Boot checkbox in the installer enables
  `<loader secure='yes'/>` instead (`DomainConfig::secureBoot`).
- For domains without Karton metadata, the disk size is queried with
  `qemu-img info -U --output=json` (cached per path; `-U` is required while a
  running VM holds the image lock) and written into the Karton metadata on the
  next configuration save.
- Autostart is toggled from the General configuration tab through
  `Karton::setAutostart()`; it is also read back into `DomainConfig::autostart`.
  `Karton::prepareAutostart()` additionally enables autostart on the storage
  pools and libvirt networks used by the VM, and `autostartProblems()` reports
  the remaining prerequisites (the libvirt systemd service for system
  connections, or the login-time limitation of session connections).
- Pause/resume/reboot are `Karton::pauseDomain()`, `resumeDomain()` and
  `rebootDomain()`. `Domain::isPaused` mirrors `VIR_DOMAIN_PAUSED`; never infer
  paused state from the localized `DomainConfig::state` string.
- `Karton::domainStats()` returns CPU percentage, CPU time, current/max memory
  and vCPU count for the VM page. With the reader active it reads a cache filled
  by `requestDomainStats()` on the reader thread and returns the last sample
  immediately (the first call can be empty; `domainStatsUpdated` refreshes the
  UI), so the 2-second `VMPage.qml` timer never blocks the GUI. The CPU
  percentage is computed from the `virDomainInfo` CPU-time delta in
  `buildStatsMap()` and averaged over the vCPUs. The synchronous
  `domainStatsSync()` stays as the fallback when the reader is unavailable.
- `domainDiskStats()` and `domainInterfaces()` use the same cache pattern
  (`m_diskStatsCache`/`m_interfaceCache`, refreshed on the reader thread, with
  `domainDiskStatsUpdated`/`domainInterfacesUpdated`). The QML timers keep
  calling them, but they only read the cache; a reader-less build falls back to
  the synchronous helpers.
- Managed save is exposed through `Karton::saveDomainState()` and
  `deleteSavedState()`, with `Domain::hasSavedState` driving the VM page
  actions. `startDomain()` restores a saved state automatically through
  `virDomainCreate()`; never call `virDomainManagedSaveRemove()` while the
  domain is running. A managed-save image also restores the old device set:
  after editing XML/adding devices, use Delete Hibernation before starting if
  the new hardware must be present immediately.
- Hibernate goes through `saveDomainStateAsync()`: the blocking
  `virDomainManagedSave()` runs on `QThreadPool` with a dedicated connection
  opened and closed on the GUI thread, while `updateBusyProgress()` polls
  `virDomainGetJobInfo()` and `cancelBusyOperation()` aborts the job. QML uses
  the async call and `BusyOperationDialog`; do not call the blocking
  `saveDomainState()` from QML.
- Snapshot create/revert/delete from QML use `createSnapshotAsync()`,
  `revertSnapshotAsync()` and `deleteSnapshotAsync()`: the blocking libvirt
  call runs in the same `startBusyOperation()` worker as hibernate, and
  `SnapshotsDialog` refreshes on `snapshotOperationFinished()`. The synchronous
  variants remain for non-UI callers.
- Application icons are installed under the reverse-DNS name
  `org.kde.karton`, matching `org.kde.karton.desktop` and `QGuiApplication`'s
  window icon lookup.
- `InstallationDialog.qml` is a four-step wizard with a step indicator:
  installation source, resources (memory/storage/format), system (firmware/
  network/CPU/acceleration) and a read-only summary. Back/Next live in the
  dialog footer and the final step creates the domain. Its pages use the same
  `ScrollableSettingsView`, 15 px section headings, 1 px rules and two-column
  label/260 px control rows as the settings pages. The installation image and
  OS variant are optional: leaving them empty creates a generic VM with an
  empty CD-ROM drive and a data disk, and `DomainXmlBuilder` falls back to the
  host architecture (with no libosinfo metadata) instead of refusing to define
  the domain.
- Window sizes are persisted in the same `karton.conf`: one main-window size and
  a separate viewer size keyed by each VM UUID, saved after resize and on close.
  Closing the main window while VM viewers are open only hides it, so the VMs
  and their viewer windows stay alive; the application quits when the last
  viewer is closed. Relaunching Karton raises the hidden main window through
  the single-instance socket.
  The Sound settings page stores per-VM playback and microphone enable flags
  (`audio/<uuid>/playback|microphone`); the viewer applies them to SPICE
  playback and record channels. The Display settings page also stores an optional fixed guest resolution per
  VM (`display/<uuid>/width|height`), with presets and a Custom width/height
  pair. When set, the viewer requests that exact
  SPICE resolution, opens at the matching window size and scales the content to
  fit the screen; Auto restores normal auto-resize. With a fixed resolution the
  viewer window is pinned to the matching client size plus the toolbar in
  windowed mode; the min/max limits are released before entering fullscreen and
  re-applied only after the compositor has restored the window, so KWin does
  not keep a windowed maximum for the fullscreen surface. In fullscreen the
  fixed resolution is ignored; `updateDisplayResolution()` uses the full
  `QScreen` geometry rather than the page size so the guest follows the output
  even if the page has not been resized yet. Minimizing and restoring a viewer
  is left to the compositor, including its window effects: only
  `showMinimized()` is called and the windowed size/limits are not touched
  while it is minimized. Never call `showNormal()` to make a fullscreen
  minimize work; that drops the fullscreen state and makes the compositor
  restore a windowed window in the screen corner. Viewers are created hidden
  and map once in their final mode, so opening one from the VM preview does
  not flash a windowed window first. The Display page also exposes QXL
  video memory (`vgamem`, 16–256 MiB) for higher guest resolutions; libvirt
  requires `vgamem` to be a power of two, so the page offers only those preset
  sizes and the backend rounds defensively. The change
  is written to the inactive domain XML and applies after a VM restart. Its
  Serial console section overrides the application console font size for this
  VM (`console/<uuid>/fontSize`); the per-VM value takes priority over the
  global `console/fontSize`. Console output is processed in bounded 8 KiB
  chunks per event-loop turn (`TerminalView::feed()`), so a burst cannot block
  painting or input.
- The VM list is grouped per libvirt connection: `VMModel` emits one collapsible
  header row per connection (status and VM count) followed by that connection's
  VMs; headers have a connection context menu (Reconnect/Disconnect/Settings)
  and VM rows a VM context menu (Open viewer, lifecycle actions, Snapshots,
  Clone, Settings, Delete). Row order is stored per connection. The main window
  always uses the fixed two-column layout (list on the left, details on the
  right); opening or switching a VM replaces only the details page and never
  hides or replaces the list. `VMListPage::openDomain()` must set
  `pageStack.currentIndex` to `depth - 1` before `pageStack.replace()`: clicking
  in the list column makes that column current, and `PageRow.replace()` pops
  the pages above the current one and replaces the new topmost page, so it
  would otherwise replace the VM list itself (this made the list disappear
  when a collapsed connection was expanded after a restart).
- The VM list uses `Karton::domainOsIcon()`/`domainOsName()` and flat
  `karton-os-*` family icons (Windows modern/legacy, Linux Tux, BSD family,
  macOS and DOS), installed into hicolor. Unknown guests use Karton's own
  `karton-os-generic` layered-window icon, not the plain symbolic monitor. The
  VM page uses Luma-style transparent details rows and a rounded preview frame.
  Around New Year (`Karton::holidayMode`, 25 December through 7 January) Linux
  guests use `karton-os-linux-newyear`, which adds a static snow drift under
  Tux. There is no snowfall animation and no window-wide snow. For testing,
  `KARTON_HOLIDAY_MODE=1` forces the seasonal icon on and `KARTON_HOLIDAY_MODE=0`
  forces it off; without the variable the current local date decides.
- `ClipboardManager` (`core/viewer/clipboardmanager.*`) owns the Qt/SPICE
  clipboard bridge: host clipboard events, guest clipboard requests, ownership
  state and clipboard modes. `DomainViewer` only attaches it to the main
  channel and forwards the text/files modes. Host-to-guest file lists use the
  SPICE WebDAV channel and phodav virtual directories; the VM must have the
  `org.spice-space.webdav.0` channel.
- Clipboard synchronization uses the SPICE main channel and the guest agent.
  Host clipboard reads are skipped unless the viewer item has focus
  (`ClipboardManager::setActive()`), because a Wayland clipboard read can block
  while another client owns the selection.
  Application Settings has separate text/file modes: 0 disabled, 1 host to
  guest, 2 guest to host, 3 bidirectional (`clipboard/textMode` and
  `clipboard/filesMode`). Text uses `main-clipboard-selection-*` and is
  guarded against host/guest echo loops; Wayland requires the viewer window to
  have focus. File clipboard depends on guest support and is not a substitute
  for filesystem shares.
- `SettingsDialog.qml` is a window-modal overlay dialog attached to the
  owning window. It is capped at 1000×680, has a shared page header, a fixed
  navigation column, one scrollable content page and a fixed footer, and never
  opens as an independent top-level window. Pages: Overview, CPUs & Memory,
  Boot Options, Disks, Network, Display, Sound, Input, USB, PCI, TPM,
  Filesystem, Controllers, Serial & Console, Other devices, Clipboard and XML.
  Each page uses
  Luma-style transparent rounded window, 200/160 px category sidebar with a
  translucent highlight and accent bar, 24 px content margins and two-column
  label/control rows. The Add/Save/Close actions are `customFooterActions` on
  the dialog itself, outside the scrollable page, so they stay visible. The
  selected sidebar category maps to its StackLayout page through
  `root.categoryPages`; keep that array in sync when reordering pages. Disks are grouped into attached devices, create, attach
  media and advanced options. Other Karton dialogs share the same translucent
  rounded background. Disks, networks, USB and PCI devices are managed inline
  (lists, add/attach/detach controls) instead of separate dialogs. Settings USB
  attach/detach is persistent (`attachUsbDevicePersistent` /
  `detachUsbDevicePersistent`); the viewer's USB dialog remains the runtime
  path. Disks expose advanced options (read-only, shareable, serial, cache and
  discard). It reads `Karton::domainHardwareInfo()` and writes firmware, boot
  order, video, sound, TPM and description through `editDomainHardware()`.
  The footer button "Add…" opens the grouped `AddHardwareDialog.qml` wizard,
  which is the only place where devices are added: storage, network, input,
  controller, serial, console, channel, USB/PCI passthrough, redirection and
  filesystem shares. Filesystem entries are called shares in the UI; their
  target directory is the name/mount tag shown on the Filesystem page. Its forms use the same two-column label/control rows
  as the settings pages, and its content scrolls with the mouse wheel/touchpad. Settings pages list and edit attached devices but do
  not duplicate add controls. Input devices (`<input>`) have their own page,
  and Controllers, Serial & Console and Other devices pages surface the
  remaining device categories from the domain XML using human-readable labels,
  with detach actions for non-system devices. All pages are refreshed after XML
  edits or device hotplug. The XML page is read-only by default with
  an Edit switch; it uses `XmlHighlighter` for theme-coloured XML syntax
  highlighting, does not accept mouse drags so selecting text does not pan the
  XML (the wheel and scrollbar still scroll, and dragging a selection
  auto-scrolls in steps; long lines wrap visually without altering the saved
  XML), has an inline find field in the page header
  (Ctrl+F focuses it, Enter/Shift+Enter move between matches, Esc clears it,
  with a match counter), preserves its scroll position when toggling Edit, and
  saves
  through `saveDomainXml()`, which validates the complete document against
  libvirt's domain schema (`VIR_DOMAIN_DEFINE_VALIDATE`). Invalid or unsupported
  XML is rejected, reported, and the editor reverts to the last valid version.
  There are no separate Storage/Networks/PCI/USB menu actions.
- Remote connections (a URI with a host, such as `qemu+ssh://`, `qemu+tcp://`
  or `qemu+tls://`) keep their disks on the remote host. `connectionIsRemote()`
  distinguishes them (local `qemu:///` and `qemu+unix:///` URIs have no host);
  `KARTON_FORCE_REMOTE=1` forces the remote code paths for testing. Storage
  pools and volumes are managed through `storagePools(uri)`,
  `storageVolumes(pool, uri)`, `createStorageVolume()` and
  `deleteStorageVolume()` (`core/app/karton_storage.cpp`); the create wizard
  and Add Hardware's "New disk" create the volume in the selected remote pool
  through `virStorageVolCreateXML` and pass `diskPrecreated`, so Karton never
  runs local `qemu-img`, `mkpath` or ACL checks for it. Remote Add Hardware
  and Settings use editable remote paths (existing image/ISO) and hide or
  disable the local file pickers and local `qemu-img` disk tools. Disk sizes
  come from the domain metadata or `virDomainGetBlockInfo`, never local
  `qemu-img`. `deleteDomain` reads every `<disk device='disk'>` source from the
  inactive XML before undefining and deletes the matching remote volumes with
  `virStorageVolLookupByPath`/`virStorageVolDelete`; disk-folder permission
  checks are skipped for remote domains. `cloneDomain()` and `importOvf()`
  refuse remote targets with a clear error; `importDomainXml()` defines on the
  selected connection.
- The create wizard, the domain XML import dialog and the OVF/OVA import
  dialog all ask for the **target libvirt connection**. `createDomain()`,
  `importDomainXml()` and `importOvf()` take a `connectionUri` (empty means
  the primary); the automatic disk ACL uses that URI (a system target needs the
  libvirt-qemu ACL, a session target does not), and the selected connection's
  group is refreshed after the operation instead of only the primary list. The create wizard's network picker lists the networks of the
  selected connection.
- New domains choose a named libvirt host network such as
  `br0-network`/`default` (`<interface type='network'><source network='…'/>`)
  or no network; only active libvirt networks of the selected connection are
  listed, and the wizard does not expose a separate QEMU user-mode option
  because networks are managed per connection in `ConnectionSettingsDialog`. The NIC driver is a
  separate choice (`virtio` default or `e1000`), stored in
  `DomainConfig::networkModel`; the selected libvirt network is passed as
  `networkName` and validated with `virNetworkLookupByName()` before defining
  the domain.
- Karton does not take VM screenshots and never writes a preview file:
  `virDomainScreenshot()` is not used, because libvirt's QEMU driver would
  create a short-lived per-domain screenshot file anyway. Live previews come
  from SPICE instead. Each VM page creates a hidden `VMViewerWindow` with
  `previewMode: true`; it owns the single SPICE connection for that VM and
  registers through `Karton::registerPreviewViewer()` without increasing
  `viewerCount`. The hidden viewer's `DomainViewer` has `previewOnly: true`,
  connects only the SPICE display and main channels (no input, cursor, audio,
  clipboard or serial fallback), and `updateLivePreview()` throttles frames to
  200 ms, storing them through `Domain::updateLivePreviewFrame()`; a retry
  timer and a 500 ms frame pump keep the preview alive after reconnects.
  Opening the viewer promotes that same window with `openAsViewer()` and
  switches `DomainViewer::previewOnly` off, so no second SPICE client is ever
  created. Closing the viewer demotes the window back to preview mode with
  `enterPreviewMode()`; `Karton::viewerForDomain()` also returns preview
  windows so list/detail open actions reuse them. Preview windows are never
  destroyed while the application runs: on page close/VM stop they are
  disconnected and hidden (`suspendPreview()`) and resumed on the next page
  open, because destroying a QQuickWindow at runtime leaves QQC2/Breeze style
  timers pointing at freed objects. `PreviewItem` paints the
  in-memory `QImage` directly, so the QML never reloads an image and the
  preview does not blink. A stopped VM shows no preview.
- SPICE cursor shapes are installed as the actual host cursor by
  `DomainViewer::applyGuestCursor()`; no scene-graph cursor overlay is painted.
  There is one display node per renderer and one root node that owns it. Client
  mouse mode sends no SPICE `cursor-move` events, so the host cursor follows the
  local pointer instead of the initial `(0, 0)` cursor position. New domains
  must therefore keep the default client mouse mode and the USB tablet; never
  emit `<mouse mode='server'/>`. Server mode makes the guest track a separate
  relative cursor while the viewer shows the guest cursor shape at the host
  pointer, so clicks land at a different place than the visible pointer. The
  tablet already advertises the side/extra buttons, so back/forward do not need
  server mode.
- The viewer's `DomainViewer` must receive keyboard focus after the window
  opens (`forceActiveFocus` in `VMViewerWindow.qml` and on click), and after
  toolbar/menu/dialog interaction. Scancodes are normalized to evdev by
  subtracting the X11/xkb keycode offset of 8 on both `xcb` and `wayland`;
  subtracting it only on Wayland made keys map to unrelated PC XT scancodes
  under XWayland. Extended keys must keep the SPICE 0x100 prefix (Up is
  `0x148`); truncating the converted scancode to 8 bits turned arrows, Home,
  Delete and right-side modifiers into keypad keys. Ctrl+Alt releases the
  pointer so Escape reaches the guest. While a running viewer is active,
  `ShortcutInhibitor` inhibits desktop shortcuts through the Wayland
  `zwp_keyboard_shortcuts_inhibit_manager_v1` protocol, or an X11 keyboard
  grab on `xcb`, so application and desktop shortcuts such as Ctrl+Q reach the
  guest instead of closing the viewer. Capture is tied to the active running
  viewer while the pointer is over the guest area (not to the pointer grab),
  and it disables Kirigami's application-wide quit action as well as the
  compositor shortcuts. Moving the pointer to the toolbar or title bar restores
  the normal shortcuts. It is released on
  deactivation, stop, close and app exit. A stopped viewer keeps normal Karton
  shortcuts. The Wayland inhibitor keeps its registry/manager for the window
  lifetime, so only the first inhibit does `wl_display_roundtrip()`; hovering
  the guest does not block again. Seats
  and compositor grabs that never reach the client (for example some
  Meta/Super combinations) still cannot be intercepted.
- KWin still eats some window shortcuts such as Alt+F4 even with the inhibitor
  protocol, so `Karton::updateGlobalShortcutsBlock()` also calls kglobalaccel
  `blockGlobalShortcuts(true)` while the pointer is over the guest. That state
  is session-global: `Karton::releaseGlobalShortcuts()` clears it from
  `aboutToQuit`/the destructor, `Karton::init()` clears a stale block, and
  `main.cpp` handles SIGTERM/SIGINT/SIGHUP in a watchdog thread that unblocks
  shortcuts and exits if the GUI thread is stuck. Never SIGKILL a test instance
  with a viewer open; use SIGTERM.
- `DomainViewer::updateDisplayResolution()` calls
  `spice_main_channel_update_display()` with the complete viewer area in
  device pixels on page resize and when the SPICE agent connects; it enables
  display 0 and sends the monitor configuration immediately when the agent is
  present. Requesting the full area (rather than preserving the current guest
  aspect) makes the agent resize the guest so it fills the window instead of
  leaving black bars. If the agent is absent, QML fits the window once to the
  guest aspect instead.
  It deliberately does not use the aspect-fitted `DomainViewer` item size: the
  item changes size when the guest resolution changes, which caused a resize
  feedback loop while Windows was booting. The guest adapts its resolution
  through the agent (spice-vdagent / Windows guest tools); without an agent the
  request is a no-op. `fitGuestToWindow()` forces a fresh request even if the
  target size did not change.
- The "Guest agent is not connected" hint is delayed by a 60-second grace
  period so it does not appear while a VM boots or reboots. Once the agent
  connects for a VM, `guestAgent/<uuid>/seen` is stored in `karton.conf` and
  the hint is not shown while that image is in use. Showing the hint also
  stores `guestAgent/<uuid>/warningShown`, so it appears at most once per
  image. Both flags are tied to a fingerprint of the UUID, OS metadata, first
  data disk path, disk format and firmware; changing the image or OS through
  Karton invalidates them and re-enables the hint. If the agent was seen but
  stays absent for three minutes (removed guest tools, replaced image at the
  same path, slow boot), the cached state is cleared and the hint can appear
  again after the normal grace period.
- Guest tools are plain scripts in `scripts/guests/`: `linux/install.sh` and
  `windows/install.cmd`. Karton does not download, mount or execute anything in
  the guest and has no UI action for this; the user copies the script into the
  guest and runs it. Both scripts download and install the packages themselves,
  so the guest needs internet access: the Linux script uses the guest's package
  manager (apt, dnf, yum, pacman, zypper or apk) for qemu-guest-agent,
  spice-vdagent, spice-webdavd, Mesa virtio-gpu/Vulkan and a serial getty; the
  Windows script self-elevates, downloads the latest virtio-win guest tools
  bundle from the stable `latest-virtio` URL, falls back to the VirtIO driver
  MSI, installs WinFsp and offers a restart, logging to
  `%TEMP%\karton-guest-tools.log`.
- Karton logging is quiet by default: Karton info/debug and libvirt error
  messages are suppressed. Pass `--verbose` or set `KARTON_VERBOSE=1` to
  enable them.
- The guest cursor is installed as the actual host cursor and no scene-graph
  cursor overlay is painted, so pointer motion must not call
  `QQuickItem::update()`; doing so repainted the whole viewer at pointer rate.
  A guest `cursor-hide` blanks the host cursor while the pointer is over the
  display instead of re-installing the last shape: the guest draws its own
  cursor into the framebuffer in that case (window dragging, full-screen video,
  xrdp/mstsc), and showing the stale host cursor produced two visible mice.
- `PrimarySurfaceRenderer` copies SPICE's primary framebuffer into an owned
  `QImage`. The original buffer is unmapped by spice-gtk when the guest changes
  resolution (notably while Windows boots); creating/using a texture from that
  pointer on the render thread used to crash `QSGBatchRenderer::prepareRenderPass`.
  It copies 32-bit xRGB/ARGB surfaces as BGRA rows into an `ARGB32` frame (the
  RHI texture is `BGRA8`, so the GPU does the byte order conversion while
  sampling) and expands 16-bit 555/565 surfaces through a 64K lookup table
  instead of per-pixel arithmetic. Backends that cannot sample `BGRA8` get an
  explicit `RGBA8888` conversion at upload time. The row stride is honoured for
  damage copies. The renderer keeps one
  persistent RHI texture (`SpiceFrameTexture`, owned by the scene-graph node)
  and uploads only the damaged rectangles recorded by the display-invalidate
  handler; overlapping rectangles are merged, and above three rectangles their
  bounding rectangle is uploaded so one frame does not issue a dozen
  `glTexSubImage2D` calls. A preview-only or minimized
  window defers conversion (`setFrameConversionEager(false)`) until
  `currentFrame()` consumes a frame. The whole surface is uploaded
  only on surface creation/resolution changes, when a full frame is flagged,
  when damage tracking overflows 16 rectangles, or when the damaged area is at
  least half of the surface. Do not go back to `createTextureFromImage()` per frame: it
  allocates and uploads the complete 8–20 MiB surface on every guest frame and
  was the boot-animation stall. `updatePaintNode()` must also clear its cached
  `m_displayNode`/`m_cursorNode` pointers when Qt passes a null root after
  deleting the scene graph. Renderers return `nullptr` instead of deleting a
  node that is already attached; `DomainViewer` detaches the old display node
  from its root and deletes it. Display textures use `QSGTexture::Linear`
  filtering so fonts stay readable when the guest resolution does not exactly
  match the viewer size.
- If a domain has no usable SPICE display (for example a VM with
  `<graphics>` removed and `-display none`), `DomainViewer` falls back to the
  first libvirt console stream (`virDomainOpenConsole()` +
  `virStreamEventAddCallback()`); `serialConsoleActive` switches the viewer to
  `TerminalView`, which implements the VT100 subset needed by a Linux text
  console (cursor addressing, scrolling, SGR colors, alternate screen and
  common key sequences). Text-only VMs therefore open their tty in Karton just
  like in virt-manager. `TerminalView::fontFamily`/`fontSize`/`colorScheme`
  are configurable globally in Application Settings → Console; the size can be
  overridden per VM in its Display settings. `colorScheme` carries the
  background, default foreground and 16-color ANSI palette, and changing it
  remaps existing cells and scrollback. The terminal never scales the font to
  fill the window. The console
  stream must be created with
  `VIR_STREAM_NONBLOCK`: a blocking `virStreamRecv()` runs a nested libvirt
  GLib main loop from inside the stream callback and aborts the process during
  a guest reboot. Stream teardown is deferred with `QTimer::singleShot(0)` and
  a generation check instead of aborting/freeing the stream from the callback.
- A dropped SPICE session must not leave the viewer on a dead frame. The
  session's `disconnected` signal is connected for every session, and
  `DomainViewer::sessionDisconnectedCallback()` clears `m_connected`, the
  channel pointers and `m_inputsChannelReady`, releases the pointer grab and
  reconnects: previews through the 1 s preview retry, real viewers through a
  1.5 s timer. Input is only sent when the inputs channel reported
  `SPICE_CHANNEL_OPENED` via `SpiceChannel::channel-event` (`inputsReady()`);
  spice-gtk 0.43 has no `spice_channel_get_state()`. The SPICE endpoint from
  the live XML is cached in `m_endpointValid`: re-reading it on every reconnect
  blocks the GUI thread while libvirt is busy during a guest reboot. The cache
  is dropped when the domain, preview mode or the TLS options change and after
  three failed viewer reconnects, because a restarted VM can change an autoport.
- `VMViewerWindow.qml` registers itself with `Karton::registerViewer()`.
  Opening the same VM again focuses/raises the existing window instead of
  creating a duplicate. Its toolbar has the same split actions as the VM page:
  Shut Down/Start with a separate Power Off arrow, Reboot with a separate Reset
  arrow, Pause, then the toolbar's overflow menu (Actual Size, Fit Guest to
  Window, USB Devices, Hibernate, Settings and Snapshots) and fullscreen. At
  narrow widths Pause moves into the overflow while the power/reboot groups
  stay; the fullscreen floating bar puts Power Off/Reset dropdowns directly
  next to Shut Down/Reboot. The fullscreen floating bar orders the window
  controls as More, minimize, restore/exit fullscreen and close (all monochrome
  symbolic icons). Custom menus also close when the window frame is clicked or the
  application loses focus. "Fit to Window" was deliberately removed. A stopped VM opens the
  viewer without starting it and shows only a large clickable start triangle;
  it is started only when the user asks. When a running VM stops, the viewer
  closes automatically. Opening the menu releases the pointer grab first. There
  is no explicit grab/release-pointer action: clicking the display grabs the
  pointer and Ctrl+Alt releases it (Esc is sent to the guest). A double-click
  on a VM list row opens that VM's
  viewer, whether it is running or stopped. UI icons use the monochrome
  `*-symbolic` variants. PCI and Storage have no separate menu actions; they are
  managed inline from SettingsDialog.
- `PipeWireAudioSink` runs the blocking PipeWire setup/teardown (`pw_context_connect`,
  `pw_thread_loop_stop`) on a dedicated worker thread; `start()`/`stop()` only
  post work, and the destructor flushes it with a blocking call before the
  thread is stopped. PCM data still arrives from the GUI thread and is queued
  under a mutex.
- The scene graph uses OpenGL (`QQuickWindow::setGraphicsApi()` in `main.cpp`)
  with a 1-frame swap interval, double buffering and no multisampling. On
  Wayland the compositor provides the presentation sync. Vulkan is not
  supported: Qt 6.12's Vulkan/Wayland swapchain stops presenting after the
  first fullscreen transition on NVIDIA, leaving the viewer window stale.
  `KARTON_GRAPHICS_API=software` forces the software renderer for
  headless/offscreen use. The renderer is selected from the domain XML:
  `GlScanoutRenderer` is used only for a local (unix socket) SPICE display with
  `<gl enable='yes'/>`, because GL scanout is not available over TCP. All other
  displays use `PrimarySurfaceRenderer`. `GlScanoutRenderer` imports the
  scanout dmabuf through EGL (including DRM modifiers from
  `spice_display_channel_get_gl_scanout2()`) and performs all EGL/GL teardown on
  the render thread (`releaseResources()`), never in `detach()`. It keeps the
  EGLImage while the guest draws into the same dma-buf (identity from `fstat`
  plus format, stride and offset).
- Suspend/undefine/define events can arrive while a QML page is open; check for
  valid domain pointers and handle missing files gracefully.

## Git workflow

- `origin` is the personal fork `https://github.com/B612S/karton` and `main` is
  the working branch. `upstream` is `https://github.com/kenoi1/Karton`.
- Push work to `origin/main` only. Do not push to `upstream`.
- Write imperative, descriptive commit messages (e.g. "Fix storage check when
  editing a domain").
- Before committing, build the project and, for UI changes, run the manual smoke
  test above.
