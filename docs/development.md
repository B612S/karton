<!--
SPDX-License-Identifier: CC0-1.0
SPDX-FileCopyrightText: none
-->

# Development

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

