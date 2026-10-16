# Failing components tracker (tissot 6.12 bringup)

Living list. Every entry names its evidence. Updated as work lands.
Last update: 2026-10-16 (snapshot60 run149 booted: session allocated, adsprpc now fails on unimplemented ioctl 12; run150 re-applies the orphaned backlight labels, untested).

## Current evidence — snapshot60 run149 (session allocated; adsprpc fails at ioctl 12; boot still stalls)

Last confirmed-good image: **run149**. run150 is packaged, **not booted**.

### FastRPC/adsprpc: three separate defects, all now closed except the last

Each run moved the failure one layer down, which is the only reason the chain is
known at all:

| run | errno | Meaning | Fixed by |
|---|---|---|---|
| run147 | 2 `ENOENT` | node was `/dev/fastrpc-adsprpc-smd`, nothing opens it | run148 — drop the redundant `fastrpc-` prefix (`devnames[]`) |
| run148 | 16 `EBUSY` | node correct, but `fastrpc_session_alloc()` on an empty table | run149 — add `cb@0`, one session per cb child |
| run149 | `ENOTTY` | open succeeds; ioctl 12 is not implemented | **open — see the row below** |

Two earlier conclusions were wrong and are retracted: that adsprpc needed a
reserved-memory **DMA pool** (it does not — that message is `dev_info` and the
probe completes fine without one, reverted in `a7fe6fb5a`), and that **audio
was fixed** (it is not; the error count only tracks how far adsprpcd gets before
failing — see the Audio row).

Boot reaches `boot_progress_enable_screen` and stops; `sys.boot_completed`
unset at 310 s, `init.svc.bootanim` still running, ActivityManager drops
`system_server_pre_watchdog` and goes silent. **Still unexplained.**

| Component | Evidence | Status |
|---|---|---|
| **SystemServer hang (current boot blocker)** | `DropBoxManagerService: add tag=system_server_pre_watchdog`; boot never passes `enable_screen` | **Open, not yet diagnosed.** The extcon denial was a *real* cause of `PhoneWindowManager.init` trouble but fixing it (run144) did **not** clear the hang. At least one further cause remains unidentified. |
| **Audio** | run144: 308 `AudioSystem: listAudioPorts failed` + 134 `loadHwModule error -19`, at 10 Hz. run147: 0. run148: **381** | **NOT cleared — my earlier claim was wrong.** Both logcat dumps span the whole boot (checked first/last timestamps, not a rolled-over window), so run147's zero was real *for that run* but does not hold generally. The count tracks how far `adsprpcd` gets before failing: at ENOENT it dies early and audio errors never accumulate; at EBUSY it gets further and then fails differently. Do not record audio as fixed until adsprpcd actually connects. |
| **ADSP / FastRPC `FASTRPC_IOCTL_CONTROL` (12)** | run149: errno moved 2 → 16 → **"Inappropriate ioctl for device"**, i.e. open succeeds and the failure is at ioctl dispatch. `adsprpcd: apps_dev_init: failed to setup fastrpc session in kernel`; `remote_get_info failed to get attribute 257`. Our `include/uapi/misc/fastrpc.h` **has no definition for ioctl 12**, and `fastrpc_device_ioctl()` ends `default: return -ENOTTY`. CAF issues it: `oc-test/drivers/char/adsprpc_shared.h:24 #define FASTRPC_IOCTL_CONTROL _IOWR('R', 12, struct fastrpc_ioctl_control)`, handled at `oc-test/drivers/char/adsprpc.c:4755` | **The real remaining FastRPC blocker, and it is a porting task not a cycle.** `fastrpc_control()` → `fastrpc_internal_control()` dispatches on `req`: `KALLOC` is one flag, `WAKELOCK`/`PM` are small, but `LATENCY` needs PM QoS **plus** `fastrpc_send_cpuinfo_to_dsp()` — a message to the remote DSP that mainline fastrpc has no infrastructure for. `FREE_DMA_BUFF` (2) is also unimplemented. Not blocking `boot_completed`. |
| ADSP / FastRPC DMA pool | `no reserved DMA memory for FASTRPC` is `dev_info`; probe completes and registers a device with **no pool at all** | **Not a defect.** The reserved-memory pool added for it in `5080f3fab` was reverted (`a7fe6fb5a`) — the diagnosis was wrong and it added risk. |
| GPU | `/dev/kgsl-3d0` **exists**; SurfaceFlinger registered; kgsl NOMMU by deliberate design; preemption WARN is a consequence of NOMMU (`adreno_a5xx_preempt.c:598`), not a bug | The tracker's "GPU is the confirmed blocker" is **not supported** by evidence. Do not re-litigate. |
| WLAN | `wcnss.mdt ... error -2` | Not staged. Same mechanism as ADSP: `wcnss.mdt` + 4.2 MB of segments on `/firmware/image`. |
| Display composer / Dpps | `Dpps: main():33 Failed to initialize DPPS`; needs `/sys/class/leds/lcd-backlight/brightness` (**no leds class device exists on this port**), `GetDriverProbe failed to get display 1` (EACCES), and an AD interface | run142 labelled only the backlight **directory**; `genfscon` does not cascade to attribute files, so the `brightness` denial survived into run144. run146's fix (`harness-genfs-backlight-attrs.cil`) is written and **not yet in any booted image**. |
| Charging control | **CLEARED in run141** — `IChargingControl` registers, `dumpsys battery` reports real values | Done. |
| Charging rate | Same cable, same moment: stock `Max charging current: 500000`, port `100000` µA. Port DT sets `constant-charge-current-max` 1 A, so the observed value is neither the DT nor the stock figure | Unexplained. Which property the health HAL reads (usb `INPUT_CURRENT_LIMIT` vs bms `CONSTANT_CHARGE_CURRENT_MAX`) has not been checked. |

## Process notes — faults of mine that produced false evidence

Recorded because each one cost a boot cycle or nearly caused a wrong conclusion:

1. **A background capture script was left running across a later boot**
   (`sh_1421b9a0f001FCyq40J3ZHp0Wx`, run146). It kept polling for a diag device
   while run147 was being booted, so two capture processes were competing.
   Anything concluded from run146's outcome is confounded. **Always confirm
   `pgrep -af usb-boot-capture` is empty before booting.**
2. **Clearing the logdump before a boot with no capture path.** run147's first
   attempt died at the splash with 0 bytes of kernel log and the persistent
   evidence had already been zeroed. When a boot is going to be run blind,
   leave the logdump intact.
3. **`service list | grep composer` is the wrong test** for a composer2 HAL —
   it registers with hwservicemanager, not the legacy service manager. The
   composer was running all along (`init: starting service
   'vendor.hwcomposer-2-1' ... pid 670`).
4. **"The GPU is the blocker" was asserted without checking for
   `/dev/kgsl-3d0`.** It exists.
5. **The splash hang and a later successful boot of the same image are the same
   event at different times.** run147 went `18d1:d00d` (EDL) at 06:54:00, then
   came up on the port kernel minutes later by itself. Do not record an image
   as determinately bad from one splash.

## Superseded — snapshot60 run144 (extcon denial removed; audio still broken)

run144 booted and proved the extcon labelling correct and effective --
`grep -c ExtconUEventObserver` is 0 where run143 logged the failure plus the
`PhoneWindowManager.init` frames. It did **not** clear the SystemServer hang:
`system_server_pre_watchdog` still fires and `boot_completed` stays unset.

Also settled here, and previously wrong: the fingerprint SIGSEGV **does**
reproduce -- `Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0xc0
in tid 881 (android.hardware.biometrics.fingerprint...)`. The earlier "no
SIGSEGV in run137" came from a logcat window that had already rolled over.

## Superseded — snapshot60 run143 (ADSP firmware up; boot stalls after enable_screen)

Boot reached `boot_progress_ams_ready` and `boot_progress_enable_screen`, then
stopped; `sys.boot_completed` unset at 289 s. ADSP firmware was harvested off
the device itself (`/firmware/image`, 16 files, MD5-verified) and staged with
`--firmware-dir`, and the DSP came up for the first time:

    remoteproc0: Booting fw image adsp.mdt, size 7516
    remoteproc0: remote processor c200000.remoteproc is now up
    qcom,apr remoteproc0:smd-edge.apr_audio_svc: Adding APR/GPR dev: service:4:3

`firmware_class.path` had to become `/lib/firmware,/vendor/firmware` rather than
being dropped -- comma-delimited paths are supported and take priority
(`drivers/base/firmware_loader/main.c:541`), and kgsl probes at 1.4 s needing
`/vendor/firmware` for `qcom/a530_pfp.fw`.

## Superseded — snapshot60 run141 (charging-control blocker cleared)

**The charging-control HAL now registers and the battery is charging.** The
blocker was SELinux *labelling*, not permission: `vendor_sepolicy.cil` labels
the power_supply directories with `genfscon` using literal CAF 4.19 sysfs
paths (`/devices/platform/soc/200f000.qcom,spmi/spmi-0/spmi0-02/...`), which
no mainline device path can match (`soc@0`, `200f000.spmi`, `spmi-0/0-02`,
`pmic@2`). The nodes kept the generic `sysfs` label, the HAL's
`sysfs_battery_supply` allow (vendor_sepolicy.cil:17841) never applied, and
SystemServer blocked in `ChargingControlController.waitForDeclaredService`
forever. Fix: three `genfscon` statements in
`porting/policy/harness-genfs.cil`, appended at packaging time.

| | run137/run140 | run141 |
|---|---|---|
| health HAL denials | 17086 | **0** |
| `IChargingControl` | never registered | `288 ... : []` |
| `dumpsys battery` | all zeros | `USB powered: true`, `status: 2`, `level: 92`, `voltage: 4145`, `temp: 277`, `health: 2` |
| SystemServer | blocked, no return | no watchdog kill, no blocked handler |

Charge counter moved 1959547 → 1957591 over 45 s with voltage rising
4145 → 4150 mV. **Charging is confirmed** — the first verified battery
reading of any kind on this port.

Two r140-style dead ends are now closed as **not causes**, checked against the
live stock device: `/sys/class/qcom-battery` does not exist on stock either,
nor does `power_supply/battery/input_suspend`, and the HAL tolerates both — so
the donor's 2147-line `battery.c` port is unnecessary. `selinux=0` put the
phone into recovery 2/2 and must not be retried; `--debuggable` never yielded
`adb root`.

Still open, in priority order:

| Component | Evidence | Next check |
|---|---|---|
| **Boot still does not complete** | run141: `sys.boot_completed` unset, `init.svc.bootanim=running` at 254 s, no SurfaceFlinger errors | The GPU blocker is untested again now that SystemServer is unblocked. Re-examine with fresh evidence before trusting the stale track claim. |
| Charging current looks low | `dumpsys battery` reports `Max charging current: 100000` µA (100 mA) vs DT `constant-charge-current-max` 1 A | Find which property feeds that field and why it reports 100 mA; charging is real but may be trickling. |
| ADSP / FastRPC | `open_device_node failed for domain ID 0 (errno 2)` ×21147; firmware is present on the device at `/firmware/image/adsp.mdt` + b00-b14 (10.5 MB), WLAN 4.2 MB, not staged | Stage into the ramdisk with `--firmware-dir`; needs the `firmware_class.path=/vendor/firmware` token dropped. No boot needed to prepare. |
| GPU | `kgsl-iommu` probe returns early at its clock lookup **on purpose** (`msm8953-xiaomi-tissot.dts`: "DO NOT fix these") → NOMMU → preemption disabled `WARN` at `adreno_ringbuffer.c:396` is a consequence, not a bug | Both are deliberate and documented; do not re-litigate. 5 past boots died when that early return was removed. |
| Thermal | `StartThermalManager` **succeeds** in run137 — changed since run130 | No longer a failure; verify sensor monitoring separately. |
| Fingerprint | **No SIGSEGV in run137 logcat**; the run130 `0xc0` crash does not reproduce | Re-check before porting anything; identify the fitted sensor first. |
| mm-pp-dpps backlight | 135 `avc` denials on `name="brightness"` from `mm-pp-daemon` | Same genfscon mechanism should fix it; add to `porting/policy/` when convenient. |
| Display helper / camera / vibrator / sensors | unchanged from run130 | Revisit after the boot-completion blocker. |

## Current evidence — snapshot60 run137/run139 (both QPNP drivers bound)

The VADC reference-channel gap named in run136 is fixed; the charger now
probes and the port exposes the full donor supply set.

| Observation | Evidence | Reading |
|---|---|---|
| Charger probes | `run137-usb.log:396`, `run139-usb.log:405`: `SMBCHG successfully probe Charger version=SCHG_LITE Revision DIG:0.0 ANA:0.1 batt=1 dc=0 usb=1` | Reproduced on two runs. `SCHG_LITE` is the branch the subtype fix reaches via PMI8940. |
| Gauge probes | `run139-usb.log:409`: `FG Probe success - FG Revision DIG:3.1 ANA:1.2 PMIC subtype=64` | Byte-identical to stock `run135-stock-kernel-dmesg.log:769`. |
| Both bound, three supplies | live `/sys/bus/platform/devices/...:qcom,fg/driver` and `...:qcom,qpnp-smbcharger/driver` both populated; `/sys/class/power_supply/` = `battery`, `bms`, `usb` | The kernel-side objective is met. |
| Subtype fix works on hardware | `run139-usb.log:404`: `smbchg_hvdcp_enable_cb: HVDCP disabled` | Only reachable when `hvdcp_not_supported` is set, i.e. when the subtype reads PMI8940. |
| **Charging never measured** | `dumpsys battery` all zeros; every read channel closed | See the table below. |

**`selinux=0` put the phone into recovery, twice, and must not be retried.**
`package-run138.json` records kernel payload `dabc20b9...` and ramdisk
`d12d1d73...`, both **identical to run137**, base `boot-run137.img`; the only
difference in the whole image is one appended cmdline token. run138 and
run138b both returned `fastboot boot ... OKAY` and then landed in recovery
with a 0-byte capture, against 0/2 for every image without the token.
`CONFIG_SECURITY_SELINUX_BOOTPARAM=y` is set and `hooks.c:148` consumes
`selinux=`, but `selinux_enabled_boot=0` also gates `selinuxfs.c:2148`, so
`/sys/fs/selinux` never appears. That last step is **unverified**; it is a
hypothesis consistent with two failures, not a read init source.

Why charging could not be measured, all channels and their evidence:

| Channel | Outcome |
|---|---|
| `shell` read of `/sys/class/power_supply/*` | SELinux denied, `avc ... permissive=0`, `getenforce` → `Enforcing` |
| `dumpsys battery` | all zeros — the Lineage health HAL is the denied party |
| `adb root` | `uid=2000(shell)` after run139 even with `ro.debuggable=1` |
| `--debuggable` (`ro.secure=0`, `ro.debuggable=1`) | `ro.debuggable` took (`1` at runtime, late), but init restores `ro.secure=1`; adbd stayed shell |
| `selinux=0` cmdline | 2/2 recovery, see above |
| SD mirror `/sdlog/ack-kmsg.log` | no SD card present |
| `pstore` / `logdump` partition | port kernel cannot open `PARTLABEL=logdump`; no `su` |

`usb=1` at probe means the charger IC saw VBUS, and the phone is cabled, so
charging is plausible. **Plausible is not measured**, and no live
`status`, `current_now`, `charge_counter` or `voltage_now` has been read on
this port.

## Current evidence — snapshot60 run136 (first boot with both new initcalls enabled)

Run136 reached sustained ADB on `6.12.93-4k-ga54b131a885c-dirty` for the
whole 150 s poll (`run136-adb-timeline.log:12-37`), init starting services by
device t=42 s, with **neither** QPNP initcall blacklisted. Runs 131/132
(same payloads, both drivers enabled) and 135 (`initcall_blacklist=
smbchg_init`, gauge alone) all stalled with no ADB and no USB capture; run134
(`initcall_blacklist=smbchg_init,fg_init`) reached ADB at 175.79 s.

| Observation | Evidence | Reading |
|---|---|---|
| Fuel gauge bound and probing | `run136-usb.log:408` `FG: fg_probe: FG Probe success - FG Revision DIG:3.1 ANA:1.2 PMIC subtype=64`; live `/sys/bus/platform/devices/200f000.spmi:pmic@2:qcom,fg/driver` present | Byte-identical to stock `run135-stock-kernel-dmesg.log:769`. `PMIC subtype=64` is what the new `qcom-spmi-pmic.c` translation produces; the raw `PMIC_SUBTYPE` register reads `0x11`. |
| Charger **not** bound | `run136-usb.log:453` `qcom,qpnp-smbcharger: deferred probe pending: ... adc@3100 not ready`; no `driver` symlink on the live device | Blocked one level down, not by the charger itself. |
| VADC refuses to probe | `run136-usb.log:158` `qcom-spmi-vadc 200f000.spmi:pmic@2:adc@3100: Please define VDD channel` | `drivers/iio/adc/qcom-spmi-vadc.c:810-815` makes `VADC_VDD_VADC`/`VADC_GND_REF` mandatory. The ported `arch/arm64/boot/dts/qcom/pmi8950.dtsi` defined neither. Upstream mainline omits them only because its driver dropped the check; `oc-test/arch/arm64/boot/dts/vendor/qcom/pmi8950.dtsi:84-92` carries both. |
| Only `bms` supply existed | live `/sys/class/power_supply/` = `bms` only; `H136` health HAL loops on `/sys/class/power_supply/battery/charging_enabled` | No `battery` and no `usb` supply, because the charger registers both. The run130 charging-HAL blocker is therefore **not** resolved by run136. |

**Attribution is not established.** Run136 changed two things at once: the
`qcom-spmi-pmic.c` PMI8950 subtype translation and `module_init` →
`late_initcall` in both drivers. The subtype change does not alter the FG
path (its `case PMI8950:`/`case PMI8940:` arms at `qpnp-fg.c:8318-8320` and
`:8620-8624` share one `fg_8950_hw_init` call), so the initcall level is the
likely cause — but there is no oops capture to confirm it.
`pull-run-evidence.sh` returned 0 bytes for run136 (no `su` on the port
kernel, `/sys/fs/pstore` denied), which is the documented "no record does not
exonerate" case. The `late_initcall` move is retained: it is what would make a
recurrence land in ramoops instead of vanishing.

Provenance: `package-run136.json` (image `5deea6e457ae4ad1b55fdf25c17bf6967ae8e7d8268adca6190c47b320a827cd`,
ramdisk `d12d1d733d69eb08783b463dbd12b7196d3531fbdff31ef1d4a6d3033c45a6e8` —
the untouched run130 ramdisk), `run136-build.log`, `run136-adb-timeline.log`,
`run136-usb.log`, `run136-host-logcat.log`. `boot-run130.img` verified
unchanged at `30bacf46ecf72dd6ca71de3f83601b7e1e71f8173b59c59326af2eb5f4d1b7f1`.

## Current evidence — snapshot60 run130

Run130 reached sustained ADB on `6.12.93-4k-ga54b131a885c`, still responsive
at 495.89 s uptime, with a backlit black screen and no `sys.boot_completed`.
The return to rooted stock was a host-requested normal reboot, not a silent
reset. This is an ADB milestone; it does not close graphics or Android boot.
Build/image provenance and persistence checks are in
`TRACKER-kgsl-port.md`, "Run130: sustained ADB milestone".

Evidence below is under `artifacts/snapshot60/`:
`L130` = `run130-logdump.log`, `H130` = `run130-host-logcat.log`,
`D130` = `run130-logcat-deduplicated.log`. D130 reduces 225051 raw lines
(39166738 bytes) to 8911 entries (2104969 bytes), grouping exact
level/tag/message matches while retaining counts, first/last timestamps,
original line numbers and every unparsed line. Counts account for every
input line; the raw SHA256 is unchanged. Use raw logs for ordering and
thread/PID attribution; Android wall-clock jumps are preserved.

### Priority and evidence

| Component | Run130 observation | Interpretation / next check |
|---|---|---|
| Lineage charging-control HAL / SystemServer | L130:8536-8539: `vendor.lineage.health.IChargingControl/default` is declared but unavailable. H130:137675: watchdog kills SystemServer after a 189 s main-thread block; H130:137693-137697: stack waits in `waitForDeclaredService` from `ChargingControlController`. PID 3400 subsequently waits for the same service (`run130-early-logcat.log.4:559`). | A concrete additional Android boot blocker, independent of the GPU error messages. Missing charging-node warnings repeat 38538 times across four paths (D130:1813-1956); L130:4532 says `No battery devices found`. Check charger/fuel-gauge support and HAL registration together. Fixing this alone has not been shown to complete boot. |
| GPU command processor / clock | L130:441 selects NOMMU; first `CP initialization failed to idle` at L130:3960, six occurrences. `gfx3d_clk_src` configuration-update WARN repeats eight times from L130:3993. H130:3838 and :6591 show unsignalled SurfaceFlinger/bootanimation KGSL fences. | GPU start remains broken. ME_INIT says `cb=unavailable` (L130:3491); its zero fields are not live SMMU readings. Clock warnings also occur in run128; neither their cause nor the black-screen cause is established by this run. |
| ADSP / FastRPC / WCNSS | L130:544-565: early `adsp.mdt` and `wcnss.mdt` loads fail `-2`. Modem mounts at L130:1645, but late firmware path setup fails `errno=13` at :2583. L130:13604: `fastrpc-adsp never appeared`. | The existing late-rebind helper exits when setting the path fails (`initramfs-diag/init.c:1479-1489`), before rebind. The same path-write failure and missing device occur in run128 (:2560, :4586). Restore the previously working late-firmware path before assuming a new FastRPC driver defect. |
| `adsprpcd` log storm | H130:1720 / D130:2697-2698: domain 0 device open fails ENOENT 24010 times; the retry message occurs 12005 times. | These are internal retries from daemon PID 801, not 12005 init service restarts. Missing FastRPC device is observed; live DSP recovery remains untested. |
| Fingerprint HAL | H130:27573-27603 identifies `android.hardware.biometrics.fingerprint-service.xiaomi`, SIGSEGV at address `0xc0`, in `Session::Session(fingerprint_device*, ...)`. H130:164175-164205 repeats the same fault address and top frames after SystemServer restarts. | Reproduced userspace null dereference. Missing donor fingerprint support is a candidate upstream dependency, not a proven explanation of this exact null pointer. Compare device nodes, HAL initialization and donor driver/DT before proposing a fix. |
| Vendor thermal service | H130:1625: CPU mitigation initialization fails; :1652: no monitoring sensor set up. D130:2574-2662 also reports missing legacy SoC/cpufreq/thermal paths and denied GPU/backlight access. | Vendor thermal monitoring/mitigation is incomplete. Overall hardware/kernel thermal protection is unverified; do not infer it is absent from these daemon errors alone. |
| Camera / sensors / vibrator | H130:973: camera `sensor_init` subdevice open fails; :1020: `sns_time_init` fails; :1104: vibrator `activate` path missing. | Additional interface gaps; end-to-end operation and exact causes remain unverified. Sensor failures coexist with absent ADSP; that alone does not prove every sensor error is DSP-caused. |
| Display policy / helper churn | L130:3571 onward: `mm-pp-dpps` denied backlight read/write and `ppd` exits 255 (:3575). Composer also receives `max_brightness` read denials (:3478). | Repeated permission failures need correct sysfs identities/policy. Do not attribute the whole black screen to these denials; GPU fences are separately unsignalled. |

### Donor and upstream cross-checks

The user-designated donor is **`/home/kurision/development/kernel/oc-test`** (inspected HEAD
`687ee893ae5d`). This is a source reference, not a claim that its current
HEAD is the exact stock boot artifact. Its `out-merge/.config` enables
`CONFIG_QPNP_SMBCHARGER`, `CONFIG_QPNP_FG`, `CONFIG_MSM_ADSPRPC`,
`CONFIG_FINGERPRINT_FPC1020` and `CONFIG_FINGERPRINT_GOODIX_GF3208`.

- Battery: `oc-test/arch/arm64/boot/dts/vendor/qcom/mi8953/tissot/battery.dtsi`
  enables `qpnp_smbcharger` and `qpnp_fg` and carries battery profiles,
  voltage/current limits and thermal coefficients. Donor
  `drivers/power/supply/qcom/qpnp-smbcharger.c:5703-5709` exports capacity
  and both charging-enabled properties; :8186 names the battery supply.
  Preserve these calibration inputs when scoping a port. Run130's DT has
  no matching charger/fuel-gauge nodes; including `pmi8950.dtsi` alone does
  not provide those drivers/interfaces.
- Fingerprint: donor `arch/arm64/configs/vendor/tissot.config:37-38` enables
  Goodix/FPC; `mi8953/tissot/fingerprint.dtsi` supplies both DT candidates.
  `techpack/xiaomi-msm8953/fingerprint/goodix/gf_spi.c:47` defines
  `/dev/goodix_fp`. Run130 has neither candidate node in its board DT.
  The actual fitted sensor and failing HAL initialization step still need
  evidence before choosing a port.
- [Lineage HAL constructor](https://github.com/LineageOS/android_hardware_lineage_interfaces/blob/lineage-24.0/health/aidl/default/ChargingControl.cpp#L60)
  loops over missing charging nodes; its
  [service entry point](https://github.com/LineageOS/android_hardware_lineage_interfaces/blob/lineage-24.0/health/aidl/default/service.cpp#L15)
  constructs that object before registering Binder. The
  [framework controller](https://github.com/LineageOS/android_lineage-sdk/blob/lineage-22.2/lineage/lib/main/java/org/lineageos/platform/internal/health/ChargingControlController.java#L72)
  waits for the declared service. This is a source-supported mechanism
  matching the log sequence; the exact vendor binary revision is unverified.
- [Upstream FastRPC](https://github.com/torvalds/linux/blob/master/drivers/misc/fastrpc.c)
  creates its misc device from RPMsg probe; this tree does so at
  `drivers/misc/fastrpc.c:2270-2275,2286`. Historical commits `1039261ce`,
  `ec6c8ea78`, `0f552dc87` and `7406c35ae` document working late firmware
  loading, channel creation, legacy device-name alias and stock labeling.
  Reuse that evidence rather than rewriting the already-solved alias.
- [Upstream RCG update](https://github.com/torvalds/linux/blob/master/drivers/clk/qcom/clk-rcg2.c#L106)
  waits for CMD_UPDATE to clear and returns `-EBUSY` on timeout, matching
  the local :113-134 path. This proves the warning is a clock-update
  failure, not why hardware failed to acknowledge it.

Next investigation order: charging HAL registration/battery support, GPU
start/fences, late firmware permissions/rebind, then fingerprint and the
remaining vendor interfaces. No new boot, rebuild or source fix was made
for that log analysis. The subsequent source port is recorded below.
Earlier sections below retain their historical scope;
the snapshot58 "single boot blocker" finding is not the run130 verdict.

### First repair — battery/charging source port prepared (unverified)

The working tree now ports `oc-test`'s QPNP SMBCHARGER and FG drivers,
PMIC voting and battery-data helpers into `drivers/power/supply/`, with
built-in Kconfig selections in `scripts/tissot-ack.config`. The board's new
`msm8953-xiaomi-tissot-battery.dtsi` carries the donor charger limits,
thermal coefficients and three byte-identical battery profiles. VCHG uses
PMI8950 ADC channel 3 (`SPARE1_03`), processed voltage with 1/3 prescaling,
matching the donor legacy channel. The ADC table now exposes that reading.
Power-supply properties and sysfs formatting include the donor charging
interface while preserving all existing enum values.

The drivers use the current PMIC, IIO, RTC and power-supply APIs. Probe
errors propagate, IRQ cleanup tracks successful requests, and work is
cancelled before resources are released. Asynchronous gauge-init failure
reports an error instead of freeing its resources from its own worker.

Static validation: all seven affected C units pass Clang syntax checking
against the existing run130 headers; temporary Kconfig resolution retains
FG, SMBCHARGER and both helpers as built-ins. DT preprocessing/dtc succeeds,
and decoded values match the donor's 4380 mV float limit, thermal table,
FG coefficients and channel-3 wiring. Profile bytes, existing enum values,
property/sysfs coverage and work-cancellation coverage were checked.
Host assertions exercise the actual voter functions and charger IRQ helper,
including error propagation, capacity limits and lock release. ASan/UBSan
pass for voting; leak sanitizer is unavailable under host ptrace.

**No kernel build, link or phone boot has tested this port.** Battery
readings, charging/protection behaviour and HAL registration remain
unverified. The donor's disabled BCL provider is still unavailable for
flash-current estimation. The downstream USB DPDM regulator bridge is not
ported; DPDM/HVDCP and OTG operation remain unverified. USB peripheral
configuration is unchanged.

The next authorized build/boot must verify real `battery`, `bms` and `usb`
supplies, plausible calibrated readings, writable charging-control nodes
with correct labels/permissions, charging HAL registration and absence of
the missing-node retry storm and charging-related SystemServer watchdog.
Graphics and the other run130 failures remain separate open work.

## Snapshot58 run1 — what changed

Build: `out-snapshot58` via `scripts/build-ack-tissot.sh`, `6.12.93-4k-gf6c975d4f141`.
Image `porting/artifacts/snapshot58/boot.img` (36763648 bytes, SHA256
`c24dcb4e…1099e`), base ramdisk read off the phone (`boot_a`, 661 cpio
records). RAM-only `fastboot boot`; nothing flashed. USB capture is empty
(host reader lost the port), logdump + SD agree on 39500 bytes.

**Two config gates were silently dropping the whole wireless stack.**
`scripts/tissot-ack.config` asked for `CFG80211/MAC80211/WCN36XX =y` but
`gki_defconfig` carries `CONFIG_RFKILL=m`, and `CFG80211` has
`depends on RFKILL || !RFKILL` — that evaluates to *m*, capping every
dependent at `=m`. Separately `# CONFIG_WLAN_VENDOR_ATH is not set` hides
the whole `if WLAN_VENDOR_ATH` block in `drivers/net/wireless/ath/Kconfig`,
so `WCN36XX` was not a visible symbol at all. Fixed by adding
`CONFIG_RFKILL=y` and `CONFIG_WLAN_VENDOR_ATH=y` to the fragment. This is
the same class of failure `BUILD-snapshot42.md` documents for the other 17
symbols, and `merge_config.sh` reports success while it happens.

## Snapshot58 runs 2–3 — adb over the 6.12 kernel

run2 (base: stock `boot_a` ramdisk + guard files staged by
`scripts/stage-guard-inputs.py`) recovered the guard chain and the USB
capture: `bootdevice guard installed`, `USB handoff rc appended to
/vendor/etc/init/hw/init.qcom.usb.rc errno=0`, 138256 bytes. Boot still
died at ~37 s, right after `init: Loading SELinux policy`.

run3 (base: `boot-ack-snapshot32-spill-carveout-tail-sd-screen.img` from
git, 665 cpio records, SHA256 `ef6c5400…9da06`) is the fix. The difference
is not any kernel change: the port's ramdisk carries a **patched SELinux
policy** — `userdebug_plat_sepolicy.cil`, 3304221 bytes — plus a
`force_debuggable` trigger. Neither exists in the stock ROM ramdisk and
nothing in the tree regenerates them.

**adb works.** Evidence, in order:
- init reaches `processing action (zygote-start)`; run1/2 never got past
  `post-fs`.
- `apexd: Parsing file /apex/com.android.adbd.capex/etc/init.rc` — the adbd
  APEX is installed and its rc parsed.
- The USB ACM stream ends at 463746 bytes with
  `reader-error: ... returned no data`. That is the handoff, not a fault:
  the diag ACM function releases the UDC and adbd claims it. The host then
  enumerates `18d1:4ee7 Google Inc. Nexus/Pixel Device (charging + debug)`
  and `adb devices` reports `device`.

Remaining crash is the one `86cf5e369` already predicted: `Service
'vendor.hwcomposer-2-1' received SIGSEGV` (6 occurrences). The vendor sdm
HWC needs the legacy MDSS fbdev interface; `init.c` has a
`-DANDROID_HEADLESS` stub for it that is not enabled in this build.

logdump/SD pulls for run3 returned 0 bytes: the collector ran before the
stock ROM finished coming back after the 360 s watchdog reboot. USB is the
better channel here anyway and captured 463746 bytes.

## Snapshot58 run4 — headless stubs remove the crash loop

Same base and kernel as run3; only the init changed. Built with
`-DANDROID_HEADLESS` alongside `-DANDROID_CAPTURE`
(`init_sha256 a2a6326e…`), which binds comment-only stubs over
`android.hardware.graphics.composer@2.1-service.rc` and
`vendor.lineage.livedisplay-service.sdm.rc` before policy load
(`init.c:820`). Both report `errno=0`.

| | run3 | run4 |
|---|---|---|
| SIGSEGV / SIGABRT | 6 (`vendor.hwcomposer-2-1`) | **0** |
| `vendor.hwcomposer-2-1` | started, crashed | never defined |
| services started | — | 73 |
| init stages | to `zygote-start` | to `zygote-start` |
| USB bytes | 463746 | 417507 |
| adb | `18d1:4ee7` | `18d1:4ee7` |

`sys.boot_completed=1` is still never logged, so the 360 s watchdog still
reboots. The USB stream ends at the adbd handoff, so everything after that
point is invisible to the current evidence channels — that, not a crash, is
the next thing to close. Non-fatal exits remaining: `vendor.qrtr-ns` status
1, `vendor.per_mgr` status 255, `misctrl` status 1, matching the QRTR
findings in `86cf5e369`.

## Snapshot58 run5 — adb on the port kernel; boot_completed cause found

`BOOT_DEADLINE_SECONDS` is `#ifndef`-guarded (`init.c:47`), so the watchdog
was raised to 1800 s with `-DBOOT_DEADLINE_SECONDS=1800`. Detection is
unchanged: the watchdog still reads `/dev/kmsg` for the
`processing action (sys.boot_completed=1` line (`init.c:1760`).

This is what closed the post-handoff evidence gap. With the phone held up,
adb is reachable **on the port kernel itself** — `uname -r` reports
`6.12.93-4k-gf6c975d4f141-dirty`, not the CAF 4.19 ROM. USB capture
440942 bytes, handoff at ~144 s.

**`sys.boot_completed` never fires because surfaceflinger aborts in a
loop**, visible only now that adb works on the port kernel:

```
Fatal signal 6 (SIGABRT) in tid 675 (surfaceflinger), pid 648
>>> /system/bin/surfaceflinger <<<
Abort message: 'no suitable EGLConfig found, giving up (format: 1, vendor:
Android, version: 1.4 Android META-EGL, ... Client API: OpenGL_ES)'
```

Four occurrences in the crash buffer, three distinct pids. SF never stays
up, so the boot never finishes and the property is never set. This is the
`kgsl` PARKED row below, now confirmed as the single blocker standing
between the port and a completed boot — the EGL blob needs
`/dev/kgsl-3d0`, which needs the kgsl rewrite.

Secondary: `dpmd` also aborts, in
`android::hardware::ProcessState::init` — it dies setting up its scheduler
state, not a display issue. Not yet triaged.

## Snapshot60 run87–90 — post-zygote reset attribution (run90 verdict: not a process death)

Runs87-89 all end in a reset roughly 4 s after zygote launch; the log tail
around the cutoff is missing, so no process-exit/signal evidence exists.
Display PLL (`pclk0 update_config`) and GPU mapping defects are real but
unresolved and do **not** explain the reset by themselves: run89 logdump
(`run89-logdump.log:1859-1885,1928`) shows MDP/DSI init plus successful
framebuffer registration around 76.8 s, i.e. deferred probing completes and
Android starts. "Deferred probe blocks userspace forever" is contradicted
by that chronology. Likewise `kgsl_mmu.c:730-761` falls back to NOMMU
after the legacy IOMMU probe fails (`kgsl_iommu.c:2734-2740`), which is not
proof the GPU platform probe failed.

Run90 adds opt-in kernel evidence, not a DT/firmware/display/GPU fix
(`a58d7a0`): `fs/pstore/ram.c` registers `signal_generate` (11 fatal-ish
signals, non-kthreads) and `sched_process_exit` (group leaders) probes
gated by the existing `ramoops.reset_capture=1`, emitting `DIAG
signal_generate` / `DIAG process_exit` records to kmsg (hence logdump).
No signal-delivery, exit-path, clock, GPU/MMU, firmware or display changes.
Build `6.12.93-4k-ga58d7a094211` (clang/LLD 22.1.8): `Image.gz`
`3ddcae2c…`, DTB unchanged `71951fb1…`, `Image.gz-dtb` `322b1149…`,
`boot-run90.img` `efaa1c91…` (665 CPIO records, init byte-identical to
run89 — only the kernel payload differs).

Boot method: RAM-only `fastboot boot`, 4 s ADB timeline with numbered
exclusive logcat segments; logdump precleared with verified zero readback
(`run90-preclear.status`). No 6.12 ADB window; stock 4.19 back ~25 s after
the 143.5 s USB capture (`run90-usb.log`, 476311 bytes).

Verdict (`run90-logdump.log`, `DIAG process capture active` at line 163):
84 records (39 `signal_generate`, 45 `process_exit`) show only routine init
lifecycle — SIGKILLs to one-shot services (`aconfigd`, `derive_*`,
`odsign`, `apexd`, `netd`, `recovery-persis`, mostly `result=1` as targets
already exited) and exits with ordinary one-shot codes (`0x100`, `0x500`,
`0x600`, `0xfe00`, `0x9`). Zero SIGSEGV/SIGABRT in the whole boot; zero
`Fatal signal` in early-logcat; no new tombstone. Init did start `zygote`
and `zygote_secondary` (services forked ok ~106.9 s), but neither zygote
process ever emitted output — the log runs on through HAL startup to
~112.0 s (`starting vendor.wifi_hal_legacy`, max timestamp at line 3748;
later file lines carry earlier stamps from decode interleave). No
`ACKRESET1` (no orderly reboot), no panic/oops, snapshot 2 s cadence held
to the tail (seq 53 @ 111.95 s). Two prior hypotheses closed: storage-flush
failure is out (dump → sync exit 0 → streaming reader all proven, /data
gained +720 logcat lines to ~1 s before death) and the run89 "dies during
first sync" correlation is refuted (sync exited 0 at 105.4 s, 6.6 s before
death). The reset is abrupt with healthy userspace progression — not
caused by any process death or signal. Remaining candidates: firmware /
bootloader watchdog, power, or traceless kernel halt. No DT/GPU/display
change is supported by this evidence; those defects stay open but
non-attributed. Next-run hygiene: pull postreset files/state for the
tombstone delta and delete the on-device early-logcat rotation pre-boot
(run90 appended at line 776/777). Next diagnostic if pursued: per-HAL
gating A/B of the usb-hal/wifi-hal bring-up window on an unused tag.

Run91 result (HAL gate, `init-run91` `6bc15d4a…`, same `322b1149…` kernel):
the five death-window HALs (sensors, usb, wifi, lights, memtrack) were
provably stubbed — zero mentions in `run91-usb.log`/`run91-logdump.log`
vs present in run90 — yet the boot died identically at 122.3–122.6 s,
mid-`modprobe rmnet_ctl`, silent and abrupt. 98 more DIAG records (45
signals, 53 exits; the one-time `capture active` line itself was lost to
the early kmsg overrun, the callbacks demonstrably ran), zero SIGSEGV,
zero `Fatal signal`, no `ACKRESET1`, snapshots on 2 s cadence to 121.8 s.
The gated set is exonerated and our init change is innocent (no
panic/oops/init failure). The resetter sits below the HAL layer:
firmware/bootloader watchdog, power, or traceless kernel halt.

Run92 result (cmdline A/B: run90's kernel `322b1149…` + init `639ae66c…`
plus only `pd_ignore_unused`): still no 6.12 ADB (6th miss) — the cmdline
is exonerated for the ADB loss, which now points at the run86→87 kernel /
init delta (source archaeology pending). But death moved 112 s → ~57 s
(tail mid-early-init, `qcom-post-boot`/atrace), same silent profile (9
between a power-domain-related trigger and an outlier.

Run93 result (byte-identical run92 image, new tag): death at ~136.0 s
(`run93-logdump.log` max timestamp; snapshots on cadence to 135.9 s),
silent and abrupt, no `ACKRESET1`. Run92's 57 s death was the outlier;
`pd_ignore_unused` has no consistent timing effect. Identical-image tails
now read 57 / 136 s, and the all-runs set spans 57–136 s with five deaths
clustered 107–123 s — the trigger is not deterministic in time. Seven
silent deaths, zero 6.12 ADB since run86, kernel scheduling to each tail.

Run94 result (old `1fb377fb` kernel + new `639ae66c…` init, std cmdline):
still no ADB (7th miss) — kernel-only ADB hypothesis dead. The log's last
line (122.564 s) is a `clk-rcg2.c:133 update_config` WARNING from
`kgsl_worker_thr`, with its `Call trace:` never printed: death mid-unwind
in the GPU worker's clock path (bus stall → watchdog bite fits). But the
signature is old-kernel-only (zero hits runs 90–93; the RESETSM revert
for the ADB loss — testing `init-run84` against the new kernel next.

Run95 result (new `322b1149…` kernel + old `8a878b34…` init): still no ADB
(8th miss) — init-only hypothesis dead. Longest survival yet, 156.8 s
(snapshots to 156.0 s), tail mid-`tui_comm-1-0`, same silent profile, zero
`clk-rcg2.c:133`, no `ACKRESET1`. Every variable swapped alone with no
ADB; only the full run86 combination
reconstructs it exactly to test determinism vs run86-luck.

Run96 result (bit-identical run86: kernel, init, cmdline, ramdisk all
equal): still no 6.12 ADB (9th miss) — run86's ADB was marginal luck, not
image content. The entire ADB-chase arc is closed: cmdline, kernel, init,
and full combination all fail to reproduce it. Death at ~76.6 s
(prng_seeder socket creation; SD pull failed this once, USB+raw retained),
same silent profile, no `ACKRESET1`. Tally: 57 / 77 / 107 / 111 / 112 /
122 / 123 / 123 / 136 / 157 s across ten boots. Pending: `1cd68d721`
prints raw `PON_REASON1` at qcom-pon probe (`DIAG pon_reason`); needs a
Linux rebuild — the next boot's line names run96's resetter.

Run97 PON result: `DIAG pon_reason reason1=0x21 err=0` at 13.7 s
(mechanism proven end to end), but bit0 is SOFT — our own `adb reboot
bootloader` sets it on the way in, so under last-cause semantics this
names the entry reboot, not run96's death. Under sticky-accumulate
semantics (nothing clears the register on probe), run96's death added no
watchdog/PON1 bit, which leans against a PMIC-watchdog death. Run98
(same image, new tag) decides: new bits beyond `0x21` means deaths leave
a PON signature; bare `0x21` again means deaths are PON-silent.

Run98 result: bare `0x21` again — run97's 74 s death added no PON bit.
Deaths are PON-silent; the PMIC watchdog path is exonerated (no PON1 bit
across two deaths), no bark traces in either log. Tail at 126.1 s shows
`mdss_fb_open by android.hardwar` — the display stack engaging fb, then
the same silent cut. Remaining: APPS hard hang with PS_HOLD drop (leaves
no PON trace), RPM/modem/XBL watchdog, or power transient.

Run99 result (headless `aa7317a1…` init, composer+livedisplay stubbed):
stubs held — bind errno=0, zero service definitions (the SELinux
`Could not set context` line is non-fatal noise; init parsed stub
content). Death at ~64.4 s during early init (`init.rc:116` action),
before zygote, HALs, or display engagement — same silent profile, no
`ACKRESET1`. Display engagement does not trip the reset; the earlier
the death phase, the further below Android the resetter sits. Tally:
57 / 64 / 77 / 107 / 111 / 112 / 122 / 123 / 123 / 136 / 157 s, eleven
boots. SD channel flaked twice (96, 99); USB+raw retained both times.

Run100 result (`68f757b9b` kernel, all three probes): qseecomd regressed
— `Service 'vendor.qseecomd' exited with status 255` 31 times from 242 s
onward (runs 86-99: zero occurrences). `qseecom_scm_call2 cmd 0xa ->
0` still succeeds at probe (14.5 s); the failures start at qseecomd
userspace. Nothing in the two new commits touches qseecom; both were
built into the same kernel, so attribution between `1cd68d721` and
`68f757b9b` needs a split build. GPU never probed (`gpu_ahb_clk`
missing as before), so `DIAG kgsl_pwrstate` stayed silent;
`DIAG pon_reason reason1=0x21` again. Boot survived to ~990 s then
reset; post-return ADB stalled ~5 min before recovering.

Run101 result (`58bcef215` = kgsl log reverted, PON kept): qseecomd
still exits `255`, 64 times from ~228 s — the revert did not fix it, so
both new commits are exonerated as code. The regression therefore comes
from the build/merge shape between run86's `gc0655a13e` and the
`b37ecaa78` merge line (snapshot44-46 qseecom fixes were already in
tree). Kernel side is clean: `cmd 0xa -> 0`, QSEE 0x1001000, CE clocks
ready at probe; qseecomd dies before any listener SMC appears. Death
log tail at ~549.7 s; no `ACKRESET1`; `pon_reason=0x21` again. GPU
still unprobed (`gpu_ahb_clk`), so no kgsl pwrstate lines. Next: diff
qseecom-relevant build inputs (fragment, .config delta) run86 vs now.

Root-cause chain for the qseecomd regression (all local evidence):
1. Embedded IKCONFIG identical across run84/87/101 (md5 `9e1fd9c8…`,
   2545 symbols incl. `CONFIG_QSEECOM=y`, `CONFIG_ION_LEGACY=y`) —
   config exonerated.
2. Run100/101 show `mmc0/1: Card stuck being busy! __mmc_poll_for_busy`
   + `error -110 whilst initialising SD card` at ~4.4-17 s (runs 86-99:
   zero occurrences). The SD card slot (7864900.mmc, the 32 GB logging
   card) wedges the MMC bus poll; eMMC (HS400) survives.
3. `init: wait for '/dev/block/platform/soc/..' timed out and took
   5009ms`, then five `stat failed: ...by-name/...: No such file` +
   20 s-per-partition `Skipping during mount_all` retries — mount_all
   --early ballooned to 100 s (run97: 1.07 s, run86: 0.52 s).
4. Everything downstream of `fs` shifted ~170 s; qseecomd started at
   242 s (run97: 70 s) into a storm of prior service deaths; its
   listener SMCs never registered (run97: five listeners at 70.5 s).
So: SD-card stuck-busy wedges early-mount, the 100 s stall cascades
into the HAL storm, and qseecomd is collateral damage — not a qseecom
regression at all. Run94's kgsl WARN at 122 s and run100's 990 s
survival both consistent: the SD poll wedge is timing-luck per boot.

Run102 result (same run101 kernel, init SD mirror disabled): silent cutoff
at 64.504747 s during deferred MDP re-probe (`run102-logdump.log:1498-1513`).
The SD controller still reports stuck-busy/-110 at 5.902 s (498-499):
removing userspace SD logging does not disable or repair kernel card
initialization. No keymaster/gatekeeper service starts, no ACKRESET1,
no observed port ADB; stock returned (`run102-adb-timeline.log:6-17`).
Thus these HALs cannot explain every early return.

Late-cutoff hypothesis: gatekeeper's first QSEE ioctl precedes most
107-157 s cutoffs by 0.9-3.3 s; the PID-attributed comparison is in
`TRACKER-kgsl-port.md`, "HAL timing comparison through run102".
Run97 instead ends during keymaster activity before gatekeeper starts.
Neither correlation establishes a trigger. Approved run103 probe gates
only keymaster+gatekeeper with the existing halgate memfd overlays;
same run102 kernel/DTB/cmdline, init rebuilt locally, RAM boot only.

Run103 result (paired HAL gate, init `8b07e948…`): both overlays held
(errno=0 at `run103-logdump.log:879-880`); neither HAL starts and no
`qsee_send_desc` appears. Kernel/DTB/cmdline unchanged. Kernel logging
continues to 226.511 s (3107), snapshots through 226.576 s. **User
forced the reboot because the same messages repeated; not a spontaneous
reset.** No ACKRESET1; no observed port ADB.

The repeated output is missing keymaster / keystore2 waits. Init blocks
in `vdc --wait cryptfs enablefilecrypto` during post-fs-data (2266-2279);
no zygote or late-HAL startup. Surviving the 107-157 s window therefore
does not confirm gatekeeper as trigger: this probe stops progression
before that path. The gatekeeper-only follow-on is run104 below;
run103 itself performed no second boot. Reset attribution stays WATCH.
SD stuck-busy persists but mount_all --early takes only 505 ms (1964)
and eight QSEE listeners register (2027-2104), so the card failure does
not invariably produce run100/101's downstream stall.

Run104 result (gatekeeper-only, init `d749d6fb…`): override errno=0
(`run104-logdump.log:880`), no gatekeeper start. Keymaster runs 35
send commands, fscrypt completes at 132.501 s (2555), zygote starts
156.342 s (3679). Same kernel/DTB/cmdline; one RAM-only boot.

USB captures a **synchronous external abort** at 161.390266 s, CPU 0,
PID 640, comm `cat`, ESR `0x96000010` (`run104-usb.log:3835-3839`).
No PC/FAR/abort stack survived; raw logdump stops before the header,
snapshots through 159.371 s, no ACKRESET1. Stock returns at host
elapsed 228.23 s; no host-initiated reboot after image entry, no
observed port ADB. Android early-logcat recovered; tombstones unchanged.
Gatekeeper is not necessary for this failure; no basis to confirm it
as the trigger or equate all prior silent returns with this abort.

[INFERENCE] The cat context could be ack-sync-loop's second uptime
read (first retained at 155.41 s, five-second sleep after sync), but
parent/argv and faulting PC were not captured. Do not suppress the
collector or alter drivers on that correlation. Next diagnostic todo:
obtain PC/FAR/stack and identify the read path. No further boot here.

Run105 probe: kernel-mode `do_sea` prints compact
ESR/FAR/FnV/PC/LR/PID/PPID/comm and the fault-context backtrace before
the verbose oops dump. No fixup, user-mode, reset or driver behavior
changes. Probe committed/pushed as `6b11c2e1c`; user rebuilt on Linux.
Pulled `snapshot60/run105/` hashes and embedded configuration verified.
Kernel banner matches the commit; compiled SEA diagnostic present.
Image packaged with run104's unchanged init/ramdisk/DTB/cmdline;
one RAM-only boot completed. Silent early return: raw log tail at
76.997781 s during MDP/SMMU/DSI probing (`run105-logdump.log:1489-1506`);
snapshots through 75.637 s. No keymaster/QSEE sends/zygote/mount_all,
no SEA or abort header in USB or full raw dump, no ACKRESET1.
Gatekeeper bind held (539), but this boot did not reach run104's
post-zygote abort path. On-device SEA emission and read-path attribution
remain unverified; no MDP/SMMU cause is established by the tail.
Early-logcat and sync probe are byte-identical run104 carry-over;
tombstone contents unchanged. Stock boot-completed/root/enforcing
confirmed after return, no observed port ADB. No second boot here.
Next: reproduce the SEA using the same image and obtain matching
vmlinux/System.map for offline symbolization. Keep early silent returns
separate from the confirmed run104 external abort.

Run106 result (byte-identical run105 image, fresh tag): gatekeeper bind
held (errno=0, `run106-logdump.log:878`), keymaster executes 35 sends,
zygote starts at 125.077 s (3712), then silent cutoff at 130.025225 s
(3837). No SEA/abort header in USB or full raw/snapshot dump, no ACKRESET1.
Valid snapshots through 127.977 s; no port ADB; stock returned without
a host-initiated reboot after image entry. Tombstone contents unchanged.
New Android logcat adds 88127 bytes/683 lines after the archived prefix;
sync probe adds a row at 123.64 s. Gatekeeper is not necessary for the
late silent profile either; external-abort read-path attribution is open.

MDSS error assessment: optional clock failures do not fail probe
(`mdss_mdp.c:1939-1958`), missing NRT VBIF mapping is tolerated
(3049-3054), missing reset offset/fudge factors retain disabled/default
settings, and absent min/max FPS defaults to panel frame rate
(`mdss_dsi_panel.c:2380-2418`). Run106 registers its framebuffer at
74.781 s (1875) after those same errors. Do not fabricate DT resources
or suppress messages as a reset fix. This does not exonerate every
later access: required resources, clock/power sequencing and splash
handoff need targeted review. Bootloader display is off in runs104/106
but on in early-return run105; that selects different handoff paths
(`mdss_mdp.c:3189-3215`), not yet a proven cause. No MDSS code change.

## Status key
- FIXING — patch in tree, awaiting build/boot verdict.
- BLOCKED — needs something external (rebuild, firmware harvest).
- SCOPED — cause + path known, work not started.
- PARKED — needs driver rewrite/port; see `BACKLOG-driver-rewrites.md`.
- WATCH — flaky/unexplained, monitoring across boots.
- CLOSED — not a bug / wontfix, with reason.
## In-tree (kernel) components

| Component | Symptom (evidence) | Status | Next action |
|---|---|---|---|
| ADSP FastRPC channel | run130: firmware load `-2`; late loader-path write EACCES; no `fastrpc-adsp` (L130:565,2583,13604), also observed in run128 | BLOCKED | Restore late firmware path/rebind; reuse the historical alias and label fixes, then verify the actual device/channel |
| Battery / charging kernel interface | run137/run139: both `qcom,fg` and `qcom,qpnp-smbcharger` bound; `battery`/`bms`/`usb` registered; charger probes `SCHG_LITE ... usb=1`; gauge line matches stock | FIXING | Kernel interface complete. Charging/protection behaviour **unmeasured** — every read channel is blocked by SELinux plus the absence of `su`, and `selinux=0` is a dead end (2/2 recovery) |
| Touchscreen `edt-ft5406` probe | `-110` at 300/500 ms (s50/s52); re-toggle + 500 ms succeeds (s51); bounded retry clean s53–s56 | CLOSED | Fix verified 4 boots; keep in tree |
| `pclk0 update_config` WARN | `clk-rcg2.c:133` ×2 in 12/16 runs (s48, s49 r4–r6, s51–s57 except s55 r2). Cause: RCG CMD_UPDATE cannot latch while the DSI PLL is unprepared | **CLOSED** | `CLK_OPS_PARENT_ENABLE` (`4c7eb3f0d`) verified: 0 occurrences in s58 run1. Keep in tree |
| WLAN `wcn36xx` + pronto PIL | run130: `wcnss.mdt` load fails `-2`; late firmware path setup EACCES (L130:562,2583); vendor `/dev/wcnss_ctrl` absent (H130:1713) | BLOCKED | Restore firmware/rebind first, then assess upstream WLAN versus vendor control-node ABI; no Wi-Fi operation verdict yet |
| QSEE listener registration | SMC `0x32000206` `ret=-5` ×8/boot; 8 siblings succeed same path | CLOSED | TZ refuses those IDs; tolerated; no kernel fix applies |
| `mdp5 INTF1_UNDER_RUN` | Single `errors: 04000000` at first modeset, panel healthy after | CLOSED | Benign bringup transient |
| Coldboot time | 34.3 / 11.1 / 13.7 / 13.7 / 12.6 / 16.4 s vs stock 2.8 s (s50–s56) | WATCH | ~11–18 s band; s50 34 s the outlier. Bisect if a run exceeds ~25 s |
## Proprietary (vendor blobs / downstream drivers)

| Component | Symptom (evidence) | Status | Next action |
|---|---|---|---|
| GPU 3D (`kgsl`, EGL blob `libGLESv2_adreno.so`) | run130: NOMMU selected, CP idle timeout x6, gfx clock-update WARN x8, unsignalled SF/bootanimation fences; see current evidence above | BLOCKED | Resolve GPU start with actual mapping/clock evidence; zero fields from `cb=unavailable` are not SMMU readback |
| Lineage charging-control HAL / SystemServer | run130: declared charging HAL unavailable; SystemServer watchdog stack waits in `ChargingControlController` (H130:137675-137697); replacement PID waits again | FIXING | Battery/charging source port prepared; HAL registration, node permissions and watchdog recovery remain unverified |
| ADSP firmware (`adsp.mdt`+segs) | run130: modem partition mounts, but late firmware path write fails EACCES (L130:1645,2583), before rebind | BLOCKED | Check the helper's actual write credentials/policy and loader path; historical late-load success exists in `1039261ce` / `ec6c8ea78` |
| WCNSS firmware (`wcnss.mdt`+`.bXX`) | run130: early load `-2`, same late-path EACCES; no successful boot of WCNSS observed | BLOCKED | Same late firmware/rebind repair as ADSP; firmware presence alone will not prove WLAN ready |
| Camera stack (`msm_camera`, CPP, sensors, HAL) | Nothing probed; blobs (`cpp_firmware_*`) present but no kernel side | PARKED | CAMSS/Venus 8953 variant + sensor/HAL project (backlog #4) |
| Fingerprint (`goodix_fp` / FPC candidate) | run130: fingerprint HAL SIGSEGV at `0xc0` in `Session::Session` twice (H130:27573-27603,164175-164205); donor drivers/DT exist in `oc-test` | PARKED | Identify fitted sensor and HAL initialization failure; scope donor driver/DT port, preserving GPIO/clock/pinctrl inputs |
| Vendor thermal service | run130: CPU mitigation initialization fails and no monitoring sensor is set up (H130:1625,1652) | SCOPED | Compare donor SoC/cpufreq/thermal interfaces and policy; full thermal protection remains unverified |
| Sensors / vibrator | run130: `sns_time_init` failure and missing vibrator `activate` path (H130:1020,1104) | WATCH | Separate DSP-dependent sensor failures from missing downstream device/sysfs interfaces; verify each independently |
| Modem packet path (IPA/RMNET) | QRTR signalling works; no IPA v2 in-tree, RMNET off | SCOPED | IPA-v2 backport (backlog #6) |
| EGL/GLES userspace | run130: Adreno 506 renderer initializes, but SF and bootanimation wait on unsignalled KGSL fences (H130:1373,3838,6591) | BLOCKED | Keep userspace fence evidence alongside the GPU-start investigation; no successful rendered frame established |

## Working (do not regress)
wled OVP IRQ pairing (0 `enable_irq` WARN s50–s52) · GPU fw load
(`qcom/*.fw` from ramdisk) · DSI/panel (`otm1911` status `0x9c`) ·
eMMC HS400 + SD SDR104 · USB adb handoff · QSEE SMC
(`LAST=SMC_RET -> died outside SMC`) · ramoops/logdump/SD/USB evidence
channels · Bluetooth `BT_QCA` built (unexercised) · `max98927` probes.

Verified again in s58 run1: `ft5x06` probes clean (`input: generic ft5x06
(3b) as /devices/platform/soc@0/78b7000.i2c/i2c-1/1-0038/input/input2`),
SD comes up at SDR104, `dsi_err_worker: status=5` only, and both logdump
slots (`seq=165`, `seq=166`, console `DBGC`) plus the SD mirror agree
byte-for-byte on 39500 bytes. Boot stops in the userspace bringup: the
recorder is denied `{write}` on `/kmsg` and `/mmcblk0p48` by SELinux, then
the log ends at ~37 s — before `sys.boot_completed`, so the 360 s watchdog
reboots the phone back to ROM.

## Reset logging repair — orderly persistence verified in run107

Late PARTLABEL lookup is repaired; generic pstore-blk is not a panic-safe
replacement for the existing ramoops/logdump owner. Raw snapshot recovery
exports two actual run106 console samples, no valid panic/oops dump.
CRC32-committed snapshots, separate periodic printk tails and a 60 MiB
userspace prefix cap now protect the reserved diagnostic ranges.

Host regressions and ARM64 prefix smoke pass, including physical Tissot.
User's Linux build of `be63c5ea5` RAM-booted in run107; the controlled 45 s
deadline invoked the orderly notifier, flushed a genuine ACKRESET1 record,
and returned to stock. Both console snapshots and periodic printk tails
passed header/payload CRCs after return. Earlier spontaneous reset cause and
final panic/watchdog persistence remain unproven.
See `TRACKER-kgsl-port.md`, "Run107", and `logging/README.md` for evidence.

## Run108 — early silent reset tied to MDP deferred re-probe

Removing only run107's deadline reproduced a spontaneous return at 66.54 s:
no ACKRESET1, no panic/oops, four CRC-valid snapshots. The log ends 8
records after the late MDP re-probe kick. Runs 102/105/108 all die within
15-37 records of that kick; all other runs pass it. Suspect an unclocked
MDSS register access hanging the bus (`[INFERENCE]`; access unidentified).
Next: eMMC markers around each re-probe hardware access. Late 107-160 s
resets stay open. Details: `TRACKER-kgsl-port.md`, "Run108".

## Run109 — SD removed; resets track MDSS first use

SD removal cleared the stuck-busy/-110 wedge and the boot passed MDP
re-probe, then died silently at 125.3 s, 0.4 s after `display-color-hal`
start. Eleven late runs die 0.1-2.3 s after display HAL / surfaceflinger
start or `mdss_fb_open`; early runs die in MDP re-probe. MDSS is the prime
suspect (`[INFERENCE]`). Next: init-only display-HAL gate. Details:
`TRACKER-kgsl-port.md`, "Run109".

## Run110 — display gate held; reset is below the HAL layer

Stubbing hwcomposer, display-color-hal and surfaceflinger (three DIAG
overrides errno=0, zero starts) did not prevent the silent reset: run110
died at 62.7 s inside the kernel's deferred MDP re-probe, with no display
userspace ever running. The resetter sits in the kernel MDSS re-probe
path, not the display HALs. Next: eMMC markers around each re-probe
register access. Details: `TRACKER-kgsl-port.md`, "Run110".

## Run112 — re-probe passes by luck; marker writes land nowhere

Marker kernel (`0f72ccf3…`) booted fine and the deferred re-probe passed
(framebuffer 68.6 s); silent reset at 132.4 s with the display gate
still held. Marker code ran (pr_infos present) but all eMMC slots zero
— the `/dev` open path fails in kernel context. Next: multi-path open
with error logging + readback verify. Details: `TRACKER-kgsl-port.md`,
"Run112".

## Run113 — opens fail ENOENT: kworker has no fs_struct

Writer-fix kernel (`d644bb5a…`) proves every absolute `filp_open` from
the deferred probe fails ENOENT: kworkers carry no fs_struct, so path
lookup has no root. Init opens the same nodes fine. Silent reset at
153.9 s, gate held. Next: `file_open_root` on init's borrowed root.
Details: `TRACKER-kgsl-port.md`, "Run113".

## Run114 — death before the kick; re-probe exonerated as necessary

Root-fix kernel (`2fc6e428…`) died at 62.3 s in early-init, ~10 s
before the deferred kick was due: no re-probe, no display, no trace.
Markers never ran (no tags, no errors), so the `file_open_root` fix
is still unproven. With run110 (no display userspace) this leaves no
driver-access theory standing alone — next: watchdog audit alongside
re-running for an early-path marker validation. Details:
`TRACKER-kgsl-port.md`, "Run114".

## Run115 — both watchdogs disarmed, stock regmap path opened

WDT-dump kernel (`c5f37244…`) died silently at 136.5 s. APPS WDT reads
all zero, PMIC WDT S2 master-disabled, POFF shows no PMIC_WD/UVLO/OTST3
— neither watchdog can be the resetter on the fastboot-boot path.
Stock regmap debugfs yields the live PON block, enabling post-reset
cause reads with no bootloader in between. APCS-layout second line
queued to close the offsets hole. Details: `TRACKER-kgsl-port.md`,
"Run115".

## Run116 — VFS abandoned, PON exhausted, KPSS layout proven

Root-fix kernel (`2fc6e428…`) died at ~134.3 s after an `mdss_fb_open`
by `mm-pp-dpps` (coincidence-grade; run110 had none). Borrowed-root
opens still ENOENT, so markers move to reserved-RAM writes. APCS
reads prove the KPSS layout is real (WDT truly off); PON A/B dumps
are identical, closing the register path. Open defect split out: DSI
PLL clock imbalance (`pll0_pixel already disabled` WARN, ~30x lock
failures) — display issue, not reset. Details: `TRACKER-kgsl-port.md`,
"Run116".

