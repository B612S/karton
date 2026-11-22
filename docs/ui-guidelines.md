<!--
SPDX-License-Identifier: CC0-1.0
SPDX-FileCopyrightText: none
-->

# UI guidelines

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

