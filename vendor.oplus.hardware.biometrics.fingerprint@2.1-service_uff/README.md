# vendor.oplus.hardware.biometrics.fingerprint@2.1-service_uff (open-source reimplementation)

Clean-room, dependency-free reimplementation of the OPLUS under-display
fingerprint HAL shipped as
`odm/bin/hw/vendor.oplus.hardware.biometrics.fingerprint@2.1-service_uff`
for the OPLUS sm8650 platform (OnePlus 12 / `waffle` and its siblings).

It implements the AOSP AIDL v3 interface
`android.hardware.biometrics.fingerprint.IFingerprint/default` and drives the
sensor through `/dev/fingerprint_dev`. The TEE is reached through the platform
QSEECom API (`libQSEEComAPI`, TZCom/Mink) — the one proprietary dependency, and
one the ROM already ships; everything above it is this HAL. The kernel driver
ABI is re-declared as plain constants, and the Goodix algorithm still runs
inside the TEE TA.

---

## 1. Target identification

| Property | Value |
| --- | --- |
| File | `proprietary/odm/bin/hw/vendor.oplus.hardware.biometrics.fingerprint@2.1-service_uff` |
| SHA-256 | `18136902f3b02603e259d6c2291c7c36dc8d74436244431e41dce560a6482ae4` |
| Build ID | `6947ad59d18e72455d090a1ab11eb42e` |
| Format | ELF64 PIE, AArch64, Android 34, stripped |

The binary is stripped but its `.gnu_debugdata` section (an XZ-compressed mini
symbol table) decompresses to **1196 defined symbols** (844 OPLUS methods). All
class/method names here come from that table.

> `objcopy --dump-section .gnu_debugdata=dd.bin <blob> && xz -dc dd.bin > mini.elf`

The VINTF manifest declares AOSP `android.hardware.biometrics.fingerprint` **v3**
unmodified; the `BnFingerprint`/`BnSession` hashes are imported from the AOSP
ndk library. The extra `sendFingerprintCmd`/`handleRusCmd` methods on the OPLUS
classes are appended virtuals that AOSP never dispatches.

---

## 2. Architecture

```
main()                                   registration + retry loop
 └─ BiometricsFingerprintAidl             IFingerprint v3 (getSensorProps, createSession)
     └─ BiometricsFpSession               ISession v3
         └─ IFingerprintCore              TA command engine
             └─ OpticalCore               UDFPS capture/enroll/authenticate sequence
                 ├─ FingerprintNotify     result -> FingerprintMsg
                 ├─ FingerprintCommon     IRQ / UI-ready synchronisation
                 ├─ FingerprintConfig     properties + TA config fields
                 ├─ FpCa / QseeCa         TA transport (framing + TEE)
                 │   └─ QseeComApi       platform QSEECom (TZCom/Mink)
                 └─ Device                /dev/fingerprint_dev + netlink events
```

| Vendor class | This module |
| --- | --- |
| `BiometricsFingerprintAidl` | `BiometricsFingerprintAidl.{h,cpp}` |
| `BiometricsFpSession` | `BiometricsFpSession.{h,cpp}` |
| `ICancellationSignal` impl | `CancellationSignal.{h,cpp}` |
| `IFingerprintCore` | `IFingerprintCore.{h,cpp}` |
| `OpticalCore` | `OpticalCore.{h,cpp}` |
| `FingerprintNotify` | `FingerprintNotify.{h,cpp}` |
| `FingerprintCommon` | `FingerprintCommon.{h,cpp}` |
| `FingerprintConfig` + `FingerprintTaConfig` | `FingerprintConfig.{h,cpp}` |
| `HalContext` | `HalContext.{h,cpp}` |
| `Device` | `Device.{h,cpp}` |
| `FpCa` / `CaFactory` | `FpCa.{h,cpp}` |
| `QseeCa` | `QseeCa.{h,cpp}` |
| `libQSEEComAPI` | `QseeComApi.h` (declarations) |
| `Utils` | `Utils.{h,cpp}` |

---

## 3. TA wire protocol

Every request is one buffer. The fixed header (`FpCa::sendCommand` fills the
transport fields):

| Offset | Field |
| --- | --- |
| `+0x00` | `u32 module` (`2` = fingerprint) |
| `+0x04` | `u32 cmd` |
| `+0x10` | `u64 timestamp` (ms since local midnight, filled by `FpCa`) |
| `+0x18` | `u32 cmd_len` (filled by `FpCa`) |
| `+0x1c` | `u32 extra_len` (TA log buffer size, from TA config; capped unless `talog` is enabled, filled by `FpCa`) |
| `+0x30` | command parameters |

Command ids (all module 2 unless noted), with buffer size and the response
fields the HAL reads:

| cmd | value | size | request params | response fields |
| --- | --- | --- | --- | --- |
| `Init` | 2000 | 0x1c0 | – | `+0xdc` cali state, `+0xe0` algo version, `+0x120` qrcode |
| `SetActiveGroup` | 2002 | 0x100 | `+0x30` gid, `+0x34` path | – |
| `GetFeature` | 2003 | 0xa0 | `+0x30` arg, `+0x34` mode | `+0x58` result |
| `Capture` | 2004 | 0xb8 | `+0x30` retry, `+0x34` mode, `+0x38` state, `+0x4c` date, `+0x51` rf, `+0x52` fold | `+0x6c` result, `+0x70` value, `+0x94` ignore_too_fast, `+0x95` need_touch_up |
| `PreEnroll` | 2005 | 0x80 | – | `+0x78` (u64) |
| `Enroll` | 2006 | 0xc8 | `+0x30` gid, `+0x35` auth token | – |
| `EnrollTemperature` | 2007 | 0x90 | `+0x44` temperature | – |
| `EnrollImage` | 2008 | 0xb0 | – | `+0x54` acquired, `+0x84` fid, `+0x8c` remaining |
| `EnrollSaveTemplate` | 2009 | 0x80 | – | – |
| `GetAuthenticatorId` | 2011 | 0x80 | – | `+0x78` id (u64) |
| `Authenticate` | 2012 | 0x88 | `+0x30` gid, `+0x38` operation id | – |
| `AuthenticateStart` | 2013 | 0xa0 | `+0x44` flag, `+0x48` temperature, `+0x58` brightness | – |
| `AuthCompare` | 2014 | 0xf0 | `+0x30` auth type | `+0x54` result, `+0x80` finger id, `+0x88` packed `hw_auth_token_t` |
| `AuthStudy` | 2015 | 0x80 | `+0x50` finger id | – |
| `AuthFinish` | 2016 | 0x78 | – | – |
| `RemoveFinger` | 2017 | 0x80 | `+0x30` gid, `+0x34` fid | – |
| `Enumerate` | 2018 | 0x98 | – | `+0x80` count, `+0x84` fids |
| `Cancel` | 2019 | 0x30 | – | – |
| `HmacKey` | 2020 | 0x130 | `+0x18` cmd_len, `+0x30` keymaster response (key at `+0x30+key_offset`) | – |
| `TouchUpStatus` | 2209 | 0x188 | `+0x6c` queued-touch flag (always 0 here) | `+0x10d` re-capture requested |
| `CalibrateWrite` | 0x15 | len+0x38 | `+0x20` total, `+0x24` offset, `+0x30` chunk, `+0x34` data | – (module 5) |
| `SetBaseConfig` | 2100 | 0xb0 | `+0x20` state, `+0x30` project, `+0x50` lcd, `+0x70` reserved, `+0x90` lock state | – |
| `GetConfigField` | 2101 | 0x240 | `+0x30` selector, `+0x238` len | `+0x38` value |
| `EnrollFinish` | 2102 | 0x30 | – | – |
| `SetTaLogLevel` | 2110 | 0xa0 | `+0x50` level | – |

`GetConfigField` selectors used: `0x0a` fp id, `0x0b` icon size, `0x0c` icon
location, `0x0d` icon number, `0x0e` sensor location, `0x0f` sensor rotation,
`0x10` sensor type, `0x11` circle number, `0x31` TA log buffer length, `0x32`
netlink enable, `0x3f9`/`100`/`0x65` brightness strategy/thresholds, `0x66`
HMAC-key gate, `0x78` smart-button.

The fingerprint mode carried by the capture and get-feature commands is 2 for
enroll and 3 for authenticate; the algorithm rejects anything else.

The requested length is the field's own size; the TA answers with a non-zero
status if it is larger. The geometry fields are 32 bytes, the log buffer length
and HMAC-key gate are 4, and the brightness strategy is 32.

The sensor location is reported as a pair with an empty middle field, e.g.
`720::2353` — X, then Y. Parse it the way `android::base::Tokenize` does, which
skips empty fields; a naive split yields `["720", "", "2353"]` and puts Y at 0.

The base configuration carries the identifiers the TA keys its per-panel data
on, each as a fixed 32-byte field: `+0x30` the project (`ro.separate.soft`,
falling back to `ro.boot.prjname`), `+0x50` the panel (`/proc/devinfo/lcd` from
the first recognised vendor keyword) and `+0x90` `ro.boot.vbmeta.device_state`.
`+0x20` is a state byte the vendor only fills from a `GetConfigField` query that
runs after this command, and `+0x70` is never written, so both are sent zero.

The proprietary service ran with `BiometricsFingerprintShim` interposed, which
overrode the property reads process-wide. That is reproduced here: the HAL reads
`ro.boot.vbmeta.device_state` as `unlocked`, `ro.boot.verifiedbootstate` as
`orange` and `ro.boot.mode` as `normal`. The base configuration embeds the
device state, so the TA has to be given the values it saw there.

### Capture result -> acquired info

`FingerprintNotify::checkAcquiredInfo` maps the raw capture code:
`0x14b6`→good, `0x14b7/0x14b8/0x15e4`→image-dirty, `0x14b9/0x15e0/0x277c`→too-fast,
`0x14bc`→sensor-dirty, `0x15e1/0x15e2/0x15e3/0x15e6`→vendor codes, `0x1452`→error.

---

## 4. TEE transport (QSEECom)

On this platform the TEE is reached through Qualcomm's **TZCom/Mink** stack over
`/dev/smcinvoke`; `libQSEEComAPI` is the supported userspace entry point and is
provided by the device's vendor partition. `QseeCa` links it directly
(`//vendor/oneplus/sm8650-common:libQSEEComAPI`); the declarations it uses live
in `include/QseeComApi.h`.

`QseeCa::doSendCommand` reproduces the vendor flow: it allocates a dma-buf on
the `qcom,qseecom` heap (`libdmabufheap`), copies the command into it, and calls
`QSEECom_send_modified_cmd` with the 0x40-byte shared header as both the command
and the response. The header carries the total payload length at offset 0 and
the TA status at `0x10`; the payload dma-buf is referenced through
`qseecom_ion_fd_info{fd, cmd_buf_offset = 4}`.

The dma-buf is rounded up to a page even though the command is smaller. The TA
writes its response and log past the command itself, and the TEE rejects the
invoke with `-EMSGSIZE` once that output outgrows the buffer — which is why the
vendor's allocator applies `(size + 0xfff) & ~0xfff` in both of its send paths.
The buffer is kept on `QseeCa` and only grows when a command needs more room,
the way the vendor's `QseeCa_dmabuf` does, so a command does not pay for an
allocation, an mmap and their teardown.

On a successful send the TA's log lines are printed from that same area
(tagged `[talog]`, debug and verbose dropped), exactly like the vendor does.
A rejected command always prints them, so it comes with the TA's own reason
for it; on success they are opt-in through `persist.vendor.fingerprint.talog`,
because printing them is a logd write per line on the capture path. Unless
that property is enabled, the HAL caps the trailing log area at 256 bytes;
enabling it restores the TA's configured log-buffer length.

> The TEE transport is the one piece not reimplemented: `libQSEEComAPI` is a
> proprietary blob already present in the ROM. Everything above it — framing,
> the TA command protocol, the state machine — is this HAL. `FpCa` is the seam,
> so a from-scratch smcinvoke backend can replace it without touching the rest.

TA identities: 16-entry `{name, variant}` table → `uff_spi, uff_gx, uff_jv,
uff_silead, uff_egis, uff_fpc, uff_qcom`. `waffle` ships `uff_gx.*` / `uff_spi.*`
firmware, so `uff_gx` is selected. Search paths: `/odm/vendor/firmware`,
`/odm/firmware`, `/vendor/firmware`.

### HMAC key (`QseeCa::sendHmacKeyToFpta`)

Loads `keymaster64` from `odm/vendor/firmware`, sends `{0x205, 0x2}` (8 bytes) with a
0x100-byte response, then forwards the key to the fingerprint TA with cmd `0x7e4`.
The keymaster response is copied verbatim to command offset `0x30`, so the key
(which the keymaster TA returns at `resp + key_offset`, `key_offset`/`key_len` at
`resp+4`/`+8`) lands at `0x30 + key_offset`. This command bypasses the normal
framing and sets `cmd_len` (`0x130`) itself. Gated by TA config selector `0x66`.

### Calibration

`IFingerprintCore::init` sends cmd `0x7d0`; if the cali state is 0 it reads
`/mnt/vendor/persist/fingerprint/fingerprint_cali.bin` and streams it to the TA
with cmd `0x15` (module 5) in 1 MB chunks. It does not bind a group itself:
each session binds its own user's directory (`/data/vendor_de/<userId>/fpdata`)
when it is created — the point where the vendor's `createSession` binds it.
One TA serves every user and the framework keeps a single active session per
sensor, replacing it on a user switch, so no later command has to rebind:
without the bind, a work-profile session would read and delete the primary
user's templates. The framework reconciles its own records against
that list and removes templates it does not know, which is how enrollments
left behind by another ROM are cleaned up.

---

## 5. Kernel driver ABI (`/dev/fingerprint_dev`)

Recovered from the binary and cross-checked against the GPL driver in
`kernel/oneplus/sm8650-modules/oplus/secure/biometrics/fingerprints/bsp/uff/driver/`.
Re-declared in `include/FingerprintDriverAbi.h`.

| ioctl | value |
| --- | --- |
| `FP_IOC_RESET` | `0x4f02` |
| `FP_IOC_ENABLE_IRQ` / `DISABLE_IRQ` | `0x4f03` / `0x4f04` |
| `FP_IOC_ENABLE_POWER` / `DISABLE_POWER` | `0x4f07` / `0x4f08` |
| `FP_IOC_CLEAN_TOUCH_FLAG` | `0x4f14` |
| `FP_IOC_RESET_GPIO_CTL_HIGH/LOW` | `0x4f1a` / `0x4f19` |
| `FP_IOC_IRQ_GPIO_CTL_HIGH/LOW` | `0x4f1b` / `0x4f1c` |
| `FP_IOC_DISABLE` | `0x4f01` |
| `FP_IOC_STOP_WAIT_FINGERPRINT` | `0x4f17` |
| `FP_IOC_NETLINK_INIT` | `0x4f1d` |
| `FP_IOC_RD_NETLINK_VALUE` | `0x4f1e` |
| `FP_IOC_LHBM_TEMPERATURE` | `0x4f1f` |
| `FP_IOC_INTR3_ENABLE` / `DISABLE` | `0x4f22` / `0x4f23` |

Touch interrupts are armed once at init with `FP_IOC_ENABLE_IRQ` (`0x4f03`)
and `FP_IOC_ENABLE_INTR3` (`0x4f22`), and disarmed with `0x4f04` / `0x4f23`.
Both are in/out: the argument is zero on entry and the driver writes its state
back into it. The driver reports nothing until both lines are enabled.

The Synaptics HBP stack exposes `/proc/touchpanel/fingerprint_hal`, a bitmask
owned by the fingerprint HAL. Bit 0 arms the low-power path and is written once
at HAL startup, before the first screen-off. Bit 1 enables fingerprint down/up
reporting from the normal screen-on touch path and is written when an
enrollment, authentication or interaction-detect operation starts. The driver
holds the touch path with `FP_IOC_WAKE_LOCK_HOLD` / `RELEASE` (`0x4f12` /
`0x4f13`) and clears the pending touch flag with `FP_IOC_CLEAN_TOUCH_STATUS`
(`0x4f14`) around mode changes. The proc node is opened once and kept open;
the first open retries briefly because the driver may publish it just after
boot. The last applied mode is remembered, so repeating the current state is a
no-op.

The older `oplus_touchscreen_v2` stack exposes `/proc/touchpanel/fp_enable`.
If `fingerprint_hal` is absent, the HAL falls back to
`fingerprint_active`/`fp_enable` for the low-power path, but those legacy nodes
cannot generate screen-on fingerprint touches. The fingerprint HAL has no
dependency on the OPLUS touch AIDL service or its `touchDaemon` trigger path.

Event transport: `FP_IOC_NETLINK_INIT` (`0x4f1d`) switches the driver to
netlink, and `FP_IOC_RD_NETLINK_VALUE` (`0x4f1e`) reports the transport it ended
up on — `1` netlink, `2` the internal queue drained with `read()`. The netlink
path is a protocol 25 (`NETLINKROUTE`) socket: bind it, then send one
registration message, because the driver unicasts nothing until its receive
callback has recorded a sender port id. Each event arrives as a 16-byte
`nlmsghdr` followed by `fingerprint_message_t` (`module`, `event`, `in_size`,
`in_buf[128]`, `out_size`, `out_buf[128]`); the event code is at `+0x04` and
touch coordinates land in `out_buf`. Events the optical path uses:
`TP_TOUCHDOWN=4`, `TP_TOUCHUP=5`, `UI_READY=6`, `UI_DISAPPEAR=7`. The reader
blocks on the socket plus an eventfd, which teardown signals to break it out
of the wait.

Instrumentation: setting `persist.vendor.fingerprint.perf=1` emits one
`Perf:` line per operation. It records the accepted touch source, kernel and
AOSP pointer post counts, dropped posts, AIDL setup and pre-arm time, worker,
UI-ready and post-UI delay, each core stage and each TA command. `result_us` is
the cumulative time to the authentication result; `total_us` also includes the
TA `AuthenticateFinish` that runs after the result is published. With the
property unset the trace is limited to a few atomic counters.

Touch flow: kernel `TP_TOUCHDOWN` → `HalContext::onDeviceEvent` → the touch
worker (the vendor's EventThd/WorkThd split) → `IFingerprintCore::doTouchDownEvent`
→ `OpticalCore::authenticateDown` / `enrollDown` → `captureImg`. A new
touch-down drops any UI-ready post left by screen-off, wake or the previous
operation, so the capture cannot consume an overlay event that does not belong
to it. Authentication is the exception: AOSP can start it while the display
is dozing, so a later `onContextChanged` with `DisplayState::LOCKSCREEN`
promotes the operation to reuse the `UI_READY` posted when its overlay and HBM
were configured. If the display is still dozing when the touch arrives, the
reset stays in place and the worker waits for the panel after the wake.
`onContextChanged` drops back to the reset if the display goes away, and
`UI_DISAPPEAR=7` drops a latch left by an older overlay.
The worker waits for the panel driver's `UI_READY` event (posted by the
reader) for up to 1000/500 ms; keeping the capture off the reader is what lets
that event arrive while it waits. This event is the capture gate because it
reports that the pressed icon and HBM are scanning. The 15 ms post-UI settle
delay is reduced by the event's age, so a latch posted before the touch only
waits for the remaining part. The framework's `onUiReady` callback only says
that its overlay view is configured, so it does not post the latch. A timeout
or cancel aborts the operation instead of capturing outside the overlay
window. In the
keyguard's interaction-detect mode the touch only reports
`onInteractionDetected` and lets the framework start the real authentication,
which is what the vendor does with work mode 17. `enrollDown` first re-reads
the panel temperature from the driver
(`FP_IOC_LHBM_TEMPERATURE`) and forwards it with `EnrollTemperature` (2007),
which is what the vendor sends before every enroll capture; the authenticate
start carries the panel's maximum brightness, read from
`/sys/kernel/oplus_display/max_brightness` the way the vendor's factory-test
state is.

A completed enrollment saves the template (`EnrollSaveTemplate`, 2009) before
the framework is told it finished, then refreshes the enrollment list, exactly
where the vendor does both. A failed save fails the enrollment instead of being
reported as done, and `EnrollFinish` (2102) stays on the failure paths, which
is the only place the vendor sends it.

A tap that ends while the TA is capturing makes the capture response set its
`need_touch_up` flag; the HAL then sends `TouchUpStatus` (2209) and runs the
capture once more if the TA says to — the exchange short taps need to resolve
into a result instead of a timeout.

An enroll session opens in the TA before the first touch: `Enroll` (2006)
carries the framework's auth token serialized the way the TA expects (leading
zero byte, identifiers raw, authenticator type and timestamp byte swapped,
MAC last; `Enroll` is also what puts the algorithm into enroll mode — without
it every touch is rejected with "invalid mode 0").
`Authenticate` (2012) opens the authenticate session the same way with the
operation id. The optical core also sends `AuthenticateStart` (2013) when the
framework starts authentication, because AOSP requests authentication before
the finger arrives. The touch path only has to capture; if the pre-arm failed,
or the touch arrives after the ten-second pre-arm window, it sends
`AuthenticateStart` itself. Cancelling a session forwards `Cancel` (2019),
which aborts the operation inside the algorithm; a failed enroll ends the
session with `EnrollFinish` (2102) and then cancels, a failed authenticate
ends with `AuthenticateFinish` (2071) — the vendor's failure sequences.

A successful compare returns the packed `hw_auth_token_t` at `+0x88`. The
session converts it to the AIDL token (a fingerprint token is host ordered,
other types are byte swapped) and hands it to the framework before the HAL
sends `AuthenticateFinish`, which Keystore needs for strong, key-bound app
authentication. `invalidateAuthenticatorId`
reports the last id read by `GetAuthenticatorId`, as the vendor does, instead
of zero, so the framework's authenticator-id map stays in sync.

Touch down and touch up stay inside the HAL: the vendor's service reports them
with its own `FingerprintNotify::notifyTouch`, but the AIDL v3 session callback
carries no pointer methods and the OPLUS layer that consumed those messages
has no counterpart here. Illumination is the framework's, exactly as it is for
the vendor service on this ROM: SystemUI adds a UDFPS dim layer for the
duration of the operation, the composer maps that layer (and the pressed-icon
layer) to the display's `fingerprint_mask` / `hbm_enable` connector property,
and the panel driver turns HBM on and off from those bits. The HAL never
writes `/sys/kernel/oplus_display/hbm`; the vendor only does so from its
`vnd_code` display-daemon path (`do_set_hbm`, mode 3) or factory test, and that
daemon has no channel here, so a second writer would only put the light out of
step with the dim layer. The `vnd_code` handlers (`do_set_hbm`,
`do_set_power_status`, `do_setbrightness`) and the display-daemon link are
among the pieces not reimplemented.

An operation ends where the framework says it does: when it cancels the
cancellation signal it was handed — screen off, a higher-priority client,
dismissal — and it never closes the session in between. The session stops
accepting results, drops the work mode, aborts the operation in the TA, wakes
the worker if it is waiting for the UI and releases the touch routing. It then
reports CANCELED immediately instead of waiting for the worker to finish, so
the framework's screen-off/wake handoff is not delayed. The next operation
waits for the worker before it touches the TA, and late results from the
aborted operation are discarded, so cleanup is overlapped without racing the
new operation.

No session call surfaces a TA failure as a service-specific error: the
framework's clients only catch `RemoteException`, so an error thrown from, for
example, `removeEnrollments` takes the system server down with it. Failures are
logged and reported through the session callbacks instead, matching the
vendor's asynchronous sessions.

Deliberately not ported, because none are needed for enroll or auth: the
proprietary service also runs DCS telemetry, a performance hint
(`prepareImprovePerf`), a display refresh-rate recovery (`recoveryRefreshRate`,
empty in this build) and a conditional GPIO-idle reset around the capture.

---

## 6. Configuration

`FingerprintConfig` reads `persist.vendor.fingerprint.sensor_type`,
`optical.sensorlocation`, `optical.iconsize` and `version`, plus the algorithm
component version derived from `/mnt/vendor/persist/fingerprint/`. At init it
sends the TA base config (`0x834`) and queries the TA config fields (`0x835`):
the log-buffer length, the HMAC-key gate, the brightness strategy, and the
sensor identity and geometry. The log buffer is capped at 256 bytes unless
`persist.vendor.fingerprint.talog` is enabled.

The geometry is authoritative from the TA (selectors `0x0a`–`0x11`) and is
published back to `persist.vendor.fingerprint.*` exactly as the vendor's
`FingerprintConfig::setIconType()` does, so the values are correct even on a
device whose persist partition holds no stock values. `getSensorProps()` then
applies the same mapping the LineageOS `libshims_aidl_fingerprint_v3.oplus` shim
used (`sensorlocation` -> x/y, `iconsize` / 2 -> radius, `sensor_type` -> the
AIDL enum) and sets `halHandlesDisplayTouches` for under-display sensors.
The sensor is published at `STRONG` strength, as the vendor does, so
applications can use it; `CONVENIENCE` is not exposed through the public
biometric API at all. `halControlsIllumination` stays false: the framework owns
the illumination and its UDFPS dim layer is what drives the panel's HBM.

---

## 7. Building

`cc_binary`, `device_specific: true` (installs to `/odm/bin/hw`), with `init_rc`
and `vintf_fragments`. AOSP-only dependencies:
`android.hardware.biometrics.fingerprint-V3-ndk`,
`android.hardware.biometrics.common-V3-ndk`,
`android.hardware.keymaster-V4-ndk`, `libbase`, `libbinder_ndk`, `libcutils`,
`libdmabufheap`, `liblog`.

> **Integration:** the module name matches the existing `cc_prebuilt_binary` in
> `vendor/oneplus/sm8650-common/Android.bp`. Remove the prebuilt (and its
> `proprietary-files.txt` entries for the binary, `.rc` and VINTF manifest) and
> re-run `device/oneplus/sm8650-common/setup-makefiles.sh`, then add the module
> to `PRODUCT_PACKAGES` in a hand-maintained makefile.

The build has **not** been compiled by the author.

---

## 8. Status

Implemented from the recovered protocol: the AIDL v3 surface, service
lifecycle, TA framing and all command ids/offsets above, TA config init,
calibration load, HMAC-key transfer, the UDFPS enroll/authenticate sequence,
touch/UI-ready coordination, the device ioctl/netlink ABI and the QSEECom
transport.

The residual-risk items from the first pass have been resolved by further
disassembly:

* `AuthenticateStart` (2013) parameters are `+0x44` (flag `1`), `+0x48`
  (temperature) and `+0x58` (brightness) — read directly from the `str`
  instructions.
* `HmacKey` (0x7e4) copies the 0x100-byte keymaster response to command offset
  `0x30` and sets `cmd_len` (`0x18`) itself; it does not go through the normal
  framing.
* Device sequencing around a capture is minimal: `FP_IOC_ENABLE_POWER` +
  `FP_IOC_RESET` at init, and `FP_IOC_RESET_GPIO_CTL_HIGH` per touch-down. IRQ
  and INTR3 ioctls are used only by broken-check / factory / deinit paths, not
  by the normal optical capture path, so `Device::enable/disable` are no-ops.
* `olc` (`reportFingerprintException`) is only reached from error and timeout
  paths (`waitUiready` timeout, `sendFingerprintCmd` exceptions), never on a
  successful unlock, so it is not required.
* `osense` is not required (confirmed against a working configuration).

What remains unverifiable without hardware is only the end-to-end behaviour of
the TA itself (the Goodix algorithm runs inside the TEE and cannot be exercised
here). Every userspace command, offset and sequence is evidence-based.

The vendor "value add" services (DCS telemetry, osense, performance core
pinning, touch HAL, statsd, factory/auto-smoking test centre) are intentionally
omitted to keep the module lean; they are not part of the AOSP biometrics
flow.

SPDX-License-Identifier: Apache-2.0
