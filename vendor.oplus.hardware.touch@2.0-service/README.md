# Source OPLUS touch service

This module replaces the prebuilt OPLUS touch AIDL service and the stock
`touchDaemon` with source code.

## What is implemented

- `vendor.oplus.hardware.touch.IOplusTouch/default` V2.
- The complete 96-entry node table recovered from the stock service.
- Direct access to `/proc/touchpanel` and the other `/proc` and `/dev` nodes
  used by the table.
- `initialize`, `isTouchNodeSupport`, node read/write, one-way write,
  callback registration, callback removal, and `touchNotifyClient`.
- A software gesture-mask compatibility node for the HBP driver, which does not
  expose the legacy `double_tap_enable_indep` proc node. The Lineage touch HAL
  can therefore read and update its gesture mask without crashing.
- A source `touchDaemon` that translates the HBP `KEY_F4` gesture notification
  and `/proc/touchpanel/coordinate` payload into the legacy
  `KEY_GESTURE_START + id` scan codes. The daemon creates a small uinput
  keyboard for those scan codes and falls back to evdev injection on the touch
  device when uinput is unavailable.
- The legacy source `touchDaemon` edge path is still applied when
  `/proc/touchpanel/kernel_grip_handle` exists.
- The stock `task_profiles ProcessCapacityHigh MaxPerformance` setting remains
  on the source daemon.
- XML parsing uses the platform `libtinyxml2`. The stock
  `libtinyxml2_stock.so` dependency is intentionally not used.

## HBP boundary

The HBP driver owns normal touch reporting and firmware grip. The source
`touchDaemon` currently owns the userspace gesture translation; it does not
reimplement the vendor per-frame AFE/ALGO pipeline. The vendor
`LIB_S3910_*_TS_HAL.so` and `LIB_S3910_*_TS_ALGO.so` libraries remain available
for a future source-side AFE/ALGO bridge, but they are not loaded by this
daemon.

## XML behavior

`sys_edge_touch_config.xml` contains package and activity-specific overrides.
Only direct `<default>` children of the default device are applied. Nested
`<packages>` settings need the OPLUS framework's package/activity context,
which does not exist in this tree, so applying them unconditionally would be
incorrect.

`sys_touch_function_feature_config.xml` is parsed for diagnostics, but its
per-panel values are not copied into procfs. The kernel driver already consumes
the equivalent values from the waffle DTS, and the XML keys use a different
grammar than the kernel grip command parser.

## Runtime control

The daemon applies the configuration at boot and then waits. Send it
`SIGUSR1` to re-read the XML:

```sh
kill -USR1 $(pidof touchDaemon)
```

## Files

- `OplusTouch.cpp/.h` — Binder service and callback registry.
- `TouchNodeMap.cpp/.h` — recovered node flag table and path resolution.
- `main.cpp` — service registration.
- `touchDaemon/TouchConfigDaemon.cpp` — source edge configuration daemon.
- `etc/` — init rc files and VINTF manifest.
- `docs/RE-NOTES.md` — reverse-engineering notes and source boundaries.
