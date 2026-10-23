# OPLUS touch reverse-engineering notes

## Inputs

- `vendor/oneplus/sm8650-common/proprietary/odm/bin/hw/vendor-oplus-hardware-touch-V2-service`
  - SHA-256 `219d1b5aed650dfb7f1fd99c3869f26aad1aa3ef4920a500ef8cca975cd82e3f`
  - 1328 Ghidra procedures, 2035 defined strings.
- `vendor/oneplus/sm8650-common/proprietary/odm/bin/touchDaemon`
  - SHA-256 `f84be749e8ad8a15e5667b3e3dc367edfdb95b335c04d267e1be93de8790943b`
  - 1070 Ghidra procedures, 2538 defined strings.
- `vendor/oneplus/waffle/proprietary/odm/firmware/tp/22825/`
  - S3910 HAL and ALGO libraries and scene XML.

## Service behavior

The stock service has two personalities selected in its constructor:

- `/proc/touchpanel/framework_mode` exists: `BINDER_MODE`.
- otherwise: direct proc mode.
- The legacy no-`/proc/touchpanel` entry point is `HBPService`.

In `BINDER_MODE` the service forwards node operations to the daemon's native
Binder service named `touchDeamonBinder` with a raw Parcel payload:

```text
int32 deviceId
int32 nodeFlag
int32 operation   # 0 write, 1 read, 2 support, 4 BT info
[optional String16 BT/pen payload for operation 4]
```

The daemon's `MyBinderService::onTransact` performs the operation and returns a
String16 result.

The source service does not use this transport. It owns the nodes directly.
`/proc/touchpanel/framework_mode` is deliberately ignored.

## Recovered node table

The table is at `0x15c5f8` in the stock service, 96 entries of `0x5c` bytes:

```c
struct entry {
    int32_t flag;
    int32_t devicePosition;
    char directory[0x28];
    char name[0x28];
    int32_t readSize;
};
```

- `devicePosition == 1` produces `directory + deviceId + "/" + name`.
- `devicePosition == 2` produces `directory + "/" + name + deviceId`.
- `deviceId == 0` uses `directory + "/" + name`.
- `flag == 0xa8` is the special "list all nodes" operation.

The full table is preserved in `TouchNodeMap.cpp`.

Important flags:

- `1` `double_tap_enable`
- `21` `double_tap_enable_indep`
- `26` `game_switch_enable`
- `35` `fp_enable`
- `192` `glove_mode_enable`

`TouchscreenGesture.cpp` uses `(0, 21)` for the gesture mask and `(0, 1)` to
keep gestures enabled.

## AIDL contract

The shipped V2 contract contains:

- `initialize()`
- `isTouchNodeSupport()`
- `touchReadNodeFile()`
- `touchWriteNodeFile()`
- `touchWriteBtInfo()`
- one-way `touchWriteNodeFileOneWay()`
- `touchNotifyClient()`
- `registerEventCallback()`
- `unregisterEventCallback()`

`OplusTouchStatus` is an `@Backing(type="int")` enum.
`IOplusTouchEventCallback` has `touchSendCommand()` and one-way
`touchSendCommandOneWay()`.

The stock `touchNotifyClient` dispatch rule:

- `clientFlag < 0x10000`: one-way callback.
- `clientFlag >= 0x10000`: synchronous callback.

The source service implements the same rule. Its unregister path removes only
the matching callback; the stock implementation had a list-walk bug that could
remove unrelated entries.

`touchWriteBtInfo` was forwarded to the daemon as the BT pen message type. There
is no in-tree BT pen consumer, so the source implementation logs the request and
returns `-1` rather than writing a pressure string to a proc node.

## Daemon behavior and source boundary

The stock daemon:

- opens `/dev/tp_misc`, `/dev/tp_misc1`, and `/dev/tcm_hbp0`;
- loads `LIB_<panel>_TS_HAL.so` and `LIB_<panel>_TS_ALGO.so` via `dlopen`;
- processes HBP frames in the ALGO library;
- registers `touchDeamonBinder`;
- parses touch scene/edge XML;
- sends `touchNotifyClient` notifications to the AIDL service.

The source daemon owns the userspace gesture hand-off. It watches the HBP
input device for `KEY_F4`, reads the decoded gesture from
`/proc/touchpanel/coordinate`, maps the legacy gesture id to
`KEY_GESTURE_START + id`, and creates a uinput keyboard for those scan codes.
The source service exposes a software gesture mask for the legacy AIDL node 21
because HBP does not create `double_tap_enable_indep`.

The legacy edge path is still applied when
`/proc/touchpanel/kernel_grip_handle` exists. The HBP AFE/ALGO frame pipeline
and its AFT prevention algorithm are not part of the current source daemon;
HBP normal touch and firmware grip remain authoritative in the kernel.

## Prior blob fixes

The old extraction script inserted this into the stock daemon rc:

```text
task_profiles ProcessCapacityHigh MaxPerformance
```

It is preserved in the source daemon rc.

The old extraction script replaced `libtinyxml2.so` with
`libtinyxml2_stock.so` for the prebuilt service and daemon. The source daemon
uses the platform `libtinyxml2` directly; the source service has no XML
dependency at all. No touch entry needs the `_stock` workaround.

## Stage-three limitation

`sys_touch_function_feature_config.xml` is parsed for panel/version
diagnostics. Its values are not blindly applied because the kernel DTS already
owns them and the XML keys are not in the kernel grip command grammar.
`sys_edge_touch_config.xml` global defaults are applied because their values are
already in the kernel parser's `mod para_V2 ...` grammar.
