# Xiaomi Mi A1 / tissot — Linux 6.12 port

## Status — 2026-10-25

The port boots Android with working display, touch, vendor GLES, tested media
paths and **two-way handset/speaker call audio**. Calls passed twice on ACK
6.12 #93 and then on slot-A 4.19 with the same ROM HAL setting enabled.

**Daily-driver readiness is not established.** The call setting is temporary;
suspend/USB/charging reliability, battery efficiency, service compatibility and
complete camera qualification remain open. Working features and untested modes
must not be conflated.

| Checkpoint | Verified observation |
|---|---|
| Device / ROM | Xiaomi Mi A1 (`tissot`, MSM8953), Evolution X / Android 17 |
| Latest ACK call-test image | `6.12.96-4k-gc9d35ed0d67f-dirty #93`; successful-test boot `a98ac416-a276-4ad7-b1e3-efa74f3ca13f` |
| Most recent phone test | Slot `_a`, `4.19.325-cip136-st20-perf-mglru-next-gdf4a1f863991 #5`, boot `cdea609a-193a-46ed-ae96-f271c55d6557`, Magisk root |
| Source / image boundary | Current `main` includes newer audio, USB and tuning changes; an old dirty build string does not attest all current source or qualify those newer changes |
| Camera evidence | #43 preview/video and #44 failing stills remain the retained camera checkpoints. JPEG compatibility fix is in source; no newer successful still-photo test is claimed here |

## Working functions and their limits

| Area | Verified progress | Remaining limit |
|---|---|---|
| Boot / display / input / root | Android UI, touch/navigation, USB/wireless ADB and root demonstrated on ACK | Suspend/wake and long-run stability |
| GPU | Adreno 506 vendor GLES through KGSL | Sustained performance, energy use and fault recovery |
| CPU / idle | Shared eight-core schedutil policy, 480–2784 MHz; per-core power-collapse residency increases | Endurance, transition overhead and sustained thermal behavior |
| Memory | Direct run403 memory measurements and functioning zram | Reclaim/exec-performance gap; matched same-tool donor comparison is incomplete |
| Hardware decode | Hardware AVC playback; user reports 1080p working | Retained measured AVC sample is 1920×816 at about 24 fps; broader codec/resolution coverage remains open |
| Screen recording | Recording and saving demonstrated after a one-directory permission repair | Software AVC; hardware encoding is not qualified |
| Camera preview / video | Front/rear preview and decodable MP4s | Correct still photos, orientation, telephoto/zoom, flash and repeated-session coverage |
| Voice calls | Two-way audio on handset and speaker: two ACK #93 calls plus a 4.19 comparison call | Temporary ROM property; persistence and other SIM/accessory/voice modes remain unqualified |
| Media audio | Earlier speaker playback and deep-buffer progress | EOS close timeouts, underruns and independent post-change media checks |
| Wi-Fi / Bluetooth / cellular | Basic connectivity and tested calls | Wi-Fi resume/wakes, Bluetooth call audio, modem recovery and complete data-service qualification |
| Battery / charging / USB | Interfaces report state; charging and ADB demonstrated | Current accuracy, charge rate, cable recovery and suspend reliability |

## Call-audio workaround

The final silence blocker was the ROM HAL selecting legacy voice while RIL
requested VoiceMMode1. The earlier kernel setup corrections remain required in
the tested configuration; the successful property test does not undo them.
See the [full investigation report](REPORT-call-audio-2026-10-25.md).

After boot, connect USB with ADB debugging enabled. With no call active, run
from the PC and allow Magisk root if prompted:

```sh
adb -s 2819f2320604 shell "su -c 'setprop vendor.audio.feature.multi_voice_session.enable true'"
adb -s 2819f2320604 shell "su -c 'setprop ctl.restart vendor.audio-hal'"
```

Wait a few seconds, then call. **Repeat after reboot** until the ROM sets
`vendor.audio.feature.multi_voice_session.enable=true` before audio-HAL startup.
No persistent override was installed or reboot-tested. This runtime workaround
needs no kernel rebuild.

## Known gaps and next priorities

1. Persist the ROM call setting and verify calls after reboot. Keep EOS/media
   completion errors separate from the now-working handset/speaker call path.
2. Qualify USB cable attachment, suspend/wake and charger removal on a known
   image. The older unresponsive-phone event has no established cause. New
   extcon and Wi-Fi suspend-filter source changes need their own runtime proof.
3. Recheck rear/front stills on a verified JPEG-fix image. The last retained
   #44 photos are corrupt; a later successful post-fix capture is unverified.
   Camera flash/torch integration is missing. Orientation, telephoto/third-camera
   enumeration, zoom and repeated camera sessions remain unqualified.
4. Resolve retained fingerprint HAL, FastRPC and peripheral-manager failures;
   qualify modem/data recovery. The dpmd mapping repair has only prior temporary
   service-start trials, not persistent functional data-service coverage.
5. Measure controlled screen-off drain, charging-current accuracy, sustained
   thermal mitigation and performance. Run403 reduced governor overhead and
   parked the snapshotter; it did not establish battery-life parity. Later
   AutoFDO/RCU source settings are not a verified runtime improvement.
6. Qualify hardware video encoding, GNSS, haptics, IR, remaining sensors and
   notification LED. Software AVC selection matches this ROM on donor 4.19;
   device enumeration alone is not proof of a hardware encode.
7. Qualify panic capture on the exact image with a separately approved recovery
   test. Console retention is not a guarantee of evidence from every freeze.

Current source has periodic ramoops disk snapshots disabled and releases the
camera gates at worker start. Older enabled-snapshot/overhead descriptions are
historical. Explicit panic/reset capture and its validation limits remain.

## Documentation and evidence

- [Current state](CURRENT-STATE.md): detailed component matrix, build/boot
  identities, call evidence, commands and dated subsystem checkpoints.
- [Failing components](FAILING-COMPONENTS.md): known failures, temporary repairs,
  missing integration and qualification work.
- [Call-audio report](REPORT-call-audio-2026-10-25.md): experiments, kernel fixes,
  final HAL blocker and two successful ACK calls. The subsequent 4.19 result is
  recorded in [current state](CURRENT-STATE.md).
- [Camera handoff](HANDOFF-camera-2026-10-24.md) and
  [software AVC comparison](findings/2026-10-24-software-avc-encoder-matches-stock.md):
  retained historical camera evidence and codec-selection limits.
- [Failure inventory](findings/2026-10-24-live-failure-inventory.md): dated raw
  evidence references and repair trials; not every old failure was retested.
- [Performance comparison](findings/2026-10-24-stock-baseline-perf-comparison.md):
  donor/port sampling and run403 follow-up, with workload/measurement limits.
- [Build and packaging](../BUILD_TISSOT.md),
  [boot evidence reference](REFERENCE-boot-testing.md) and
  [driver forward-porting reference](REFERENCE-driver-forward-porting.md).

Compact selected evidence is tracked with the call report. Large captures,
vendor binaries and boot images remain local under `porting/artifacts/`;
ignore rules preserve them on disk but do not make them recoverable from Git.
No new build, boot, flash, call or phone change was performed to refresh these
three documents. Dated reports describe their own runs; these live documents
provide the current overview and work queue.
