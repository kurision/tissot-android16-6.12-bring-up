# Xiaomi Mi A1 / tissot — Linux 6.12 port

## Status

Updated **2026-10-22**, after media testing and investigation of an unexpected
stop after charger removal. Target: responsive daily use, low battery drain,
and controlled temperatures on Evolution X / Android 17.

**Daily-driver readiness is not established.** Android boots and the major
interactive paths run, but the unplug-related failure, audio shutdown/calls,
and power-management validation remain open.

| Identity | Last verified state |
|---|---|
| Flashed kernel | `6.12.93-4k-gc23444468a04 #83`, slot b |
| Last inspected boot | `4aba5d4d-433d-4266-97cc-191fbf70f3b1` |
| Source at this update | `07ad1d695`; newer than the flashed image |
| Build/flash | User performed the observed builds/flashes; this documentation refresh did neither |

The source includes a revert of `c23444468` and diagnostic cleanup that are
**not verified in the last inspected image**. A committed change is not a
flashed-image result.

## Verified progress

| Area | Result and limit |
|---|---|
| Boot, display, touch, root | Android UI, USB/wireless ADB, touchscreen/navigation keys and KernelSU work. Suspend/wake reliability is still open. |
| GPU | Adreno 506 runs the vendor GLES driver through KGSL. Sustained performance and energy use are not qualified. |
| CPU | One shared `schedutil` policy, 480–2784 MHz. `psci_idle` / `menu`; standalone power-collapse residency increases on all eight CPUs. Shared policy matches the donor's clock arrangement. |
| Hardware decode | User reports 1080p playback working. A measured hardware AVC sample was **1920×816 at about 24 fps**, not a full 1920×1080 qualification. |
| Video crash fixes | `5cff6896e` fixes partial ION cache synchronization; `defe506a4` fixes VIDC debugfs naming/error handling. Old production code fails the focused host regressions; patched code passes. Later recording attempts did not repeat those oopses. |
| Screen recording | Recording and saving succeeded after a directory-permission repair. Capture used **`c2.android.avc.encoder` (software)**; hardware encode is not verified. |
| Audio | Speaker playback worked on earlier images; underruns and `CMD_EOS` timeouts remain. The last inspected #83 also contains the legacy-format experiment reverted in newer source. |
| Charging | Dedicated charger detected; battery reported charging, with sane voltage/temperature samples. Charging-rate, capacity accuracy and unplug reliability are not qualified. |
| Wi-Fi / Bluetooth / modem | Association and Bluetooth operation were demonstrated; modem comes up. This does not establish reliable voice calls or suspend recovery. |
| Crash evidence | Previous-boot ramoops console survived the latest event. Panic persistence on #83 is **unverified**, not guaranteed for every freeze. |

The saved recording was **26,553,766 bytes**, with **673 encoded frames** and
no new kernel oops in its capture. `Movies/ScreenRecords` was owned by
`media_rw:media_rw` but mode `2700`; changing only that directory to `2775`
allowed MediaProvider to save. The origin of the restrictive mode is unknown.
The donor used sdcardfs; the port gives MediaProvider a direct F2FS backing
view. Treat this as a storage-compatibility investigation, not a proven ROM
bug or a verified general migration fix.

## Open priorities

1. Reproduce charger-unplug / screen-off / wake failure with live evidence.
   The last event has no recorded panic or orderly shutdown and no confirmed
   cause. Do not attribute it to Wi-Fi, heat or battery loss from the log tail.
2. Verify the current audio-format revert on the next user-built image;
   investigate EOS timeouts, low-latency underruns and CVP voice-call setup.
3. Validate battery use after background compilation finishes, then screen-off
   idle, suspend/wake cycles, thermal throttling and sustained GPU work.
4. Validate hardware encode separately from the successful software recording.
   Camera support remains unported.
5. Validate panic capture on the exact image with a separately approved
   controlled-panic/recovery test. No such test was run in this session.

## Documentation and evidence

* [Current state](CURRENT-STATE.md): verified behavior, source/image boundary,
  important commits, measurements and evidence limitations.
* [Failing components](FAILING-COMPONENTS.md): current blockers and next checks.
* [Build and packaging](../BUILD_TISSOT.md): established build/flash procedure.
* [Boot evidence reference](REFERENCE-boot-testing.md): capture/recovery tooling
  and persistence limitations; dated operational claims need current checks.
* [Driver forward-porting reference](REFERENCE-driver-forward-porting.md):
  donor/API/lifetime review requirements.
* Selected session captures are preserved under
  `porting/artifacts/snapshot60/status-20261022/`, with `SHA256SUMS`.
  They are **local evidence: untracked text and gitignored binary captures**,
  not committed documentation.

Existing dated bring-up reports and plans are historical material. They are
not the current status or the current work queue; use these three live docs.
