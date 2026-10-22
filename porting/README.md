# tissot / Mi A1 → android-mainline

## Status — 2026-10-22

The phone runs the Evolution X ROM on this kernel, flashed to `boot_b`.
The running build is `793615c17`; `c2b2e78a9` is committed and not yet
built. [CURRENT-STATE.md](CURRENT-STATE.md) holds the evidence for each
row; the sections after this one are the history up to 2026-10-18.

### Working

| Component | State |
|---|---|
| Boot, Android UI, adb, root | Boots to the home screen; adb up; KernelSU-Next adb root |
| CPU | One `schedutil` policy over CPUs 0-7, 480-2784 MHz |
| Display + backlight | Panel driven, home screen renders, WLED through the backlight class |
| GPU | Adreno 506 on KGSL, vendor GLES 3.2 driver |
| Touch, keys | Multitouch, power/volume, capacitive navigation keys, hall sensor, double-tap wake |
| Wi-Fi, Bluetooth | wcn36xx associates; the QTI Bluetooth HAL reaches `ON` |
| Modem | Comes up with the rmtfs shared-memory UIO driver |
| Hardware video decode | H.264 1280x720 at 30 fps decodes on Venus through the ROM's OMX service; no software decoder running |
| Audio calibration | The ACDB loader finds `/sys/devices/soc0/hw_platform` = `QRD` and loads the QRD calibration |
| Freeze evidence | A hard lockup now panics, so a frozen phone leaves a logdump record |

### Not working / open

| Component | State |
|---|---|
| Full-screen video, YouTube mini player | Solid blue: the rotator overwrote the MDP core clock with 50 MHz and the panel underran. Fixed in `c2b2e78a9`, not yet built |
| Screen off (suspend) | adb/USB is lost after resume (`gcc_usb30_master_clk` stuck on, ep0 not queued); Wi-Fi and other components reported dead too. The video driver also refuses suspend (`-EBUSY`) while a session is open. Under investigation |
| Speaker output | Calibration loads, the amplifier powers on and its I2S pins are now muxed (`24ceeffd2`, `fe78c82ef`); audible output not yet confirmed |
| Freeze when leaving YouTube | Caused by an out-of-bounds write into the PCM platform's mutex (`45847644a`); not reproduced since that fix, on few runs |
| Video beyond 720p decode | 1080p decode, encode (camera recording) and secure playback untested; secure playback is out of scope |
| Cameras | Not ported: the ROM needs CAF's msmb camera stack (`techpack/camera-legacy`) |
| Sensors, fingerprint, IR, haptics | See [TRACKER-tissot-hardware.md](TRACKER-tissot-hardware.md) |

## Bring-up commits by component

Functional commits only; diagnostics, reverts and documentation are left
out. Hashes are on `main`.

* **GPU — Adreno 506 on KGSL**
  * Bring-up: `e883a4be9` enable the GPU node, `ba01d8ba1` bind it to kgsl
    instead of drm/msm, `466b1d985` vendor the 4.19 kgsl with compat shims
  * SMMU: `85ad23b58` one owner for the GPU context bank, `689664e8b`
    NOMMU with the bank untranslated, `8a50e48ba` CAF's page-table format
  * Start and hang: `966fd5e12` soft reset A5xx on start, `131277c33` A506
    quirks, `7bf34e26b` CPU latency QoS request
  * Correctness: `5037fd74d` 6.12 memory ownership, `4d2602e38` physical SG
    mapping and ZAP failure, `6bad9b4fa` the QSMMU SError fix
  * Bus votes: `4f7944387` one interconnect path per client, `66fb850af`
    drop the pending-vote retry
  * Clean-up: `3622e538f` `019b7a5ad` `532ea5b70` `9924e4f29` `97ff6777f`
    `ba2e3e1b1` `9d8f8e7aa` remove the bring-up logging; `258f96236`
    `40e5deac0` keep the IB dump on GPU faults
* **Display — MDSS, DSI, panel, backlight**
  * Import: `23f85fef1` display, panel and WLED bring-up tree
  * MDSS: `17fa25c0c` skip late hw_init while runtime-suspended,
    `6355b61d0` probe MDP without the boottime gate, `bab96fd63` single
    MDSS SMMU client and 1:1 splash map
  * DSI and panel: `2609852be` DSI byte/pixel RCG parents, `80e9cd828` DSI
    PLL lifecycle, `84f86005d` panel policy after node resolution,
    `26c607aa8` stock supply topology, `125c70dcf` reset and TE pins
  * Backlight: `c7ca1a69c` `31f923968` WLED OVP IRQ, `d3998f3ec` (re-applied
    by `35373d1a2`) WLED through the backlight class, `c0e9c6487` stock
    current limit
  * Bandwidth and rotator: `4992773f3` bus voter paths, `410c96756`
    display bandwidth through the msm-bus shim, `c2b2e78a9` rotator votes a
    minimum MDP clock (not built)
* **Hardware video — Venus on CAF vidc_3x**
  * Upstream Venus prototype: `c5e5fa5e0` msm8953 resource table and DT
  * Import: `fa4add5f1` the driver verbatim, `d7e462da4` `7aed9dbc3` the
    user space interface, `d3a1cbac7` the build, `3ed7bd87a` never next to
    upstream Venus
  * Subsystem framework shim: `735dd5498` `64b513d84` `0288bffae`
    `29efad900`
  * 6.12 API conversion: `f7aa33bd9` `2e19d190d` `fdc649252` `62176b8ec`
    `b8123d808` `044d3f406` `162246637` `fe931b92a` `da3e5224b`
    `0cb8dabd8` `f03a27694` `55bfed223`
  * Switch-over: `a31072aeb` the DT moves from Venus to vidc_3x
  * First-boot fixes: `bd462f2f1` interrupt via `platform_get_irq()`,
    `1e3269ac0` TrustZone's non-pixel CP range, `b64b87a7e` IOVAs inside
    the bank's pool, `793615c17` 32-bit compat keeps the plane's fd
* **Audio — CAF q6 stack, PM8953 CDC, MAX98927 speaker amplifier**
  * Import: `15371d63a` `fe8a18433` `51b62321f` `224949976` `6b6a54bc2`
    `7a015a35c` `750cbde2e` `91fe819e4` `0eb206e32` `8a5015f04`
    `7d6f924c4` `302782edc` `c71e69007` `5ddf6b81d` `ca066e50f`
    `867dfaaac` `3ad1dd27d` `23af05ffa`
  * ALSA core helpers: `e63411f23` `bc085d754` `602a82282` `51c41314a`
    `217ced779` `1288fe08a` `9a7f7ccda`
  * Loadable modules: `313c4c1e0` the split, `51cccf6d6` `5b69c90f2`
    `09e3154bf` loading them from the ramdisk
  * DSP and DT: `f9e30b428` APR on SMD, `fe18981e1` audio ION stream ID,
    `ecafa61f1` APR/ION/avtimer nodes, `512f2705b` analog CDC under the
    PMIC, `9b1a63525` CDC pin names, `bc1e1fdf2` digital CDC clock vote
  * Sound card: `be919f62d` `ef199eb14` `f86715c44` `8b3cb50d9`
    `f79691702` `565871d5a` `6861c32da` `59f8270f1`, and `ecad3d95c` the
    donor's PCM device numbers
  * Playback path: `d67199a80` `0700e3b03` `b250002a2` `fdfca326d`
    `4698570a2` `9a1ee1dd5` `ea9ee91a9` `b42ab748f` `f7e403d4f` `0ad17af7f`
  * Speaker and calibration: `5e37ed796` amplifier on quinary MI2S,
    `24ceeffd2` its pins, `fe78c82ef` the pin control driver, `e1ebf25d2`
    socinfo `hw_platform` for the ACDB loader
  * Stability: `45847644a` VoIP/LSM ids kept out of the channel mixer arrays
* **TEE — QSEECom, ION, SCM**
  * `a33336265` legacy QSEECom and ION, `a17c54c4f` listener errors,
    `2f5c7e101` ION heaps on TZ carveouts, `15218cbd4` `13a9c029e` SMC
    spill, TZ address and dma-buf locking, `56aed9bb8` ION for keymaster,
    `97d12f932` crypto-engine clock votes
* **Wi-Fi and Bluetooth — WCN3680**
  * `546f97cd7` wcn36xx and pronto PIL, `e4a462791` late firmware rebind,
    `dbab265f6` no silent WLAN downgrade, `bba8aa1e9` WLAN HAL compatibility,
    `7c1515865` built-in NV blob, `211b20008` Bluetooth over `smd_pkt`
* **Modem**
  * `86cf5e369` QRTR, `a775067e2` MPSS in the DT, `da553ac97` `d6f0eaf9b`
    qrtr-ns, `1e59a9e72` readable modem firmware, `ca5c32973` rmtfs UIO
* **FastRPC / ADSP**
  * `1b7cd42d2` `99e80a477` ADSP channel and config, `4864e1922`
    `5334a0552` the `adsprpc-smd` alias, `7b26d4a99` `665f41ffa` `6fba49f53`
    node name and session, `bf89009c3` `10a0ef91f` `69c93372f` `a526ed9a1`
    `08a2f4293` the CAF ioctl ABI
* **USB / adb**
  * `f630f7c07` init-driven USB handoff, `a06241c69` vendor USB controller,
    `722ab760e` the diag gadget leaves the UDC to adb, `921e51c86` USB HAL
    FunctionFS watch
* **CPU power and clocks**
  * Drivers: `59c0759f2` `46d59808d` `1a5fed588` `a59ea727b` `8d144a023`
    `1ddc32a1e` `b0005e33e` SAW4, APC rail, CPR4, APCS clock
  * Bindings and DT: `1024916e1` `af20876a3` `b8bcb3a31` `a944aed50`
    `284c97fb3` `80bdaea4d`
  * Bring-up fixes: `8302be7ba` `e4edec31e` `9ce7525b9` `416a20928`
    `6d047e8db` SPM regmap, `e15fd57a5` `53581f568` `ba2423683` `558c7aede`
    APCC handoff and range, `407c84d38` cpufreq-dt, `60d89889e` APCS mailbox
  * Tuning: `345b585b5` CPU/GPU overclock, `d5d8a9782` shared cooling device
* **Interconnect and DDR bandwidth**
  * `4463e397e` NoC fabrics on, `2ab6633b0` fabric state sync, `6d554885e`
    fabrics held at maximum, `6f844d075` CPU DDR bandwidth with frequency,
    `17a65d7c8` `3a16f8cb3` `b2134864e` the power HAL's DDR bandwidth device
* **Charging**
  * `d2c01d412` QPNP fuel gauge and charger, `353b9bb70` PMI8950 VADC
    channels, `27b6590a1` battery supply label
* **Input**
  * Touch: `8c533557b` `3533dddf6` `900a730bf` identify retries and reset
    timing, `7d0cc9a60` double-tap wake
  * Keys: `5841f8c23` `73f3f9746` `539c21b10` `a8269c845` `206e79244`
    capacitive keys, `2fce0e4d2` `0594a25e6` recents key
* **Root — KernelSU-Next**
  * `ee8b1f258` vendor it, `c4c433375` build it in-tree, `0f39c6ff7` the
    grant-root soft lockup
* **SELinux labels for the 6.12 sysfs layout**
  * `f5416e35a` `7bea83fe8` overlay contexts and relabel grants,
    `15c6948f8` kgsl attributes, `72b101287` `0b789200b` wakeup statistics
* **Evidence and build tooling**
  * pstore/logdump: `ea2850426` `2a900533c` `ee4f0bff6` `ac32929e5`
    `4cf3cabc4` `0ad385b31` `e87ca57ef`, `1e3cdeabe` panic on hard lockup
  * Init and build: `5d406d440` `379c0589d` `677e57dab` `c33b36364`
    `8f238506a` `5bf753710` `b55eb9f2e` `271fbc67b` `a8fc36128`

## The port in full: 231 commits, 2026-10-06 → 2026-10-18

This is the whole journey, not just the GPU endgame. It spans **two branches**
and you need both to read it:

| branch | commits | dates | what it is |
|---|---|---|---|
| `mdss-bisect` | **231** | 2026-10-06 → 10-16 | the real work: QSEECom, ION, display, WLAN, policy, then the GPU |
| `master` | 28 | 2026-10-17 → 10-18 | a fresh re-import at "run198" plus the SError fix and adb unlock |

They share **no merge base** — different root commits (`ae580fc8a` vs
`4d4fcc16b`), because the original repo was deleted and `master` was rebuilt
from the finished tree. So `master`'s 28 commits contain almost none of the
history that produced the driver, and reading only `master` badly understates
the effort. Author breakdown on `mdss-bisect`: 226 commits Anil Kumal, 5
sneha.katwal.

Commit character on `mdss-bisect`, which tells you how the work was done:

```
diag:     30     porting:  34     docs:    56
fix(:      6     feat(:     1     port(:    2
```

Roughly **120 of 231 commits are diagnostics and evidence**, not code. That is
the actual methodology: instrument, boot, read, conclude, revert. Several
commits are explicit reverts of earlier diagnostics that turned out to be wrong
(`58bcef215`, `a7fe6fb5a`, `876248591`), and one
(`377949eac docs: the device fragment was never merged; 17 built-ins missing
from every shipped kernel`) documents a multi-day config mistake.

### Day by day

**2026-10-06 — foundation (10 commits).** Snapshot of ACK android16-6.12,
relocate ramoops, keep bring-up tooling, then `23f85fef1` imports the first
real port chunk: QSEECom/ION 6.12-API migration, the OTM1911 DRM panel, WLED and
fbcon DT, tissot display nodes. `compat_ion` is added because upstream's
`compat_alloc_user_space` is gone, so the stack-carve helper had to be kept.

**2026-10-07 — QSEECom from the ground up (33 commits).** This is the work you
asked about, and it is the single biggest item in the port.
`a33336265 port(tissot): legacy QSEECom + ION bring-up for ACK 6.12` writes
`drivers/misc/qseecom.c` — **10,317 lines** — because upstream has no such
driver. Note that upstream *does* ship a QSEECom driver, but it is a different
thing: `drivers/firmware/qcom/qcom_qseecom.c` is 120 lines, a 2023 Maximilian
Luz rewrite, and is **disabled** in our config (`CONFIG_QCOM_QSEECOM=n`) while
our `CONFIG_QSEECOM=y` selects the legacy one. The commit implements the legacy
client ABI (20-byte MDTP compat ioctl, fixed-width CE forwarding), unlocked
DMA-BUF use, platform-owned `dma_parms`, failed-open cleanup, and non-ICE key
handling split from unsupported ICE.

The rest of the day is the debugging that followed, and it is instructive:
ION heaps bound to TZ-shared carveouts (`2f5c7e101`), carveout SMC spill with
tzbuf backing and dma-buf locking (`15218cbd4`), QSEE SMC call/return tracing
(`aaa1f484d`). Multiple policy commits (`bc871cd44`, `f5416e35a`, `7bea83fe8`)
add SSD overlay contexts, observer debug grants and init relabel grants — the
SELinux layer was load-bearing from day two, not a later addition.

**2026-10-08 — proving the TEE path (10 commits).** A per-daemon liveness probe
(`537a3ce83`) finds the recorder is blind to Android's `/proc`; a pid-ceiling
probe (`613bdfc0a`) confirms there is no pid namespace to enter, so that probe
is dead. Then `d0f4d613e` traces all QSEE svc-0 SMCs and asserts the selinux
bootparam in the build — revealing **the device fragment was never merged and
17 built-ins were missing from every previously shipped kernel**
(`377949eac`). That is a process bug found by instrumenting the build rather
than the device.

**2026-10-09 — the widest day: USB/adb, display, touch, WLAN, FastRPC (41
commits).** `86cf5e369` hands USB to adb. `f630f7c07` gets **adb working on 6.12
via an init-driven USB handoff** — the first time a mainline kernel here talked
to a host — and immediately finds graphics blocks boot. The adreno 506 GPU node
is enabled (`e883a4be9`), GPU firmware staging and exfiltration added
(`677e57dab`), and WCN3680 WLAN enabled with wcn36xx plus pronto PIL
(`d62974bac`) with a build assertion to stop silent downgrades (`677226833`).
Touch gets a real fix: `18cdcb220`/`394f6b75e` make edt-ft5x06 retry identify
across reset toggles, and `ff2d31d65` allows 500 ms post-reset boot time — the
touch row closes here. FastRPC/ADSP work begins, including harvesting modem
firmware and rebinding ADSP/WCNSS late.

**2026-10-10 — stage 1 passes, stage 2 begins (11 commits).**
`f9d9b30f1 diag: add guard-input stager, the missing packaging step` and
`fa37accb3 diag: take guard files verbatim, not derived; adb works on 6.12`
close the adb gap. Headless stubs clear the crash loop (`9bbfe6f5c`), and the
boot blocker is identified as surfaceflinger (`a1e4992dc`). Then
`f3248eda0 gpu: bind tissot GPU node to kgsl, drop drm/msm claim` — the board
now claims the GPU for KGSL and removes the `qcom,kgsl-3d0` entry from
`adreno_device.c`'s dt_match **and** from `msm_drv.c`'s `msm_gpu_match`
component list, without which msm_drm waits forever for a component that never
binds. Stage 1 verdict: **PASS, display safe GPU-less**
(`08aec7abf`). `7ba68117a` then vendors the donor's 90-file `drivers/gpu/msm`
plus the **1,884-line uapi header kept verbatim as the blob ABI contract**,
with driver-private compat layers replacing vendor-only APIs.

**2026-10-13 — the MDSs silent-death bisect (33 commits).** The longest single
investigation. Six run42 changes are reverted for a clean control
(`db5ddd4a4`), then MDP silent-death is isolated to `idle_pc_restore` hw_init
across runs 50–62 (`6ad77e420`), followed by full-probe plus a 45 s defer
(`c510185d0`). Display probes for runs 66–76 (`0b426ffb2`) add a vreg guard,
fbcon off, deferred kick, fb traces and selinux probes, landing on a composer
policy block. `ff6c9b18a` adds the policy-append hook and mdss caps delta.
Run85 reaches **BootAnimation plus GPU start -110** (`e542e2744`). GPU power
transitions are traced (`68f757b9b`) and then reverted (`58bcef215`) when the
volume proves unhelpful.

**2026-10-14 — watchdog exoneration and the DSI PLL (41 commits).** A
systematic watchdog audit concludes **stock pets the watchdogs and the port
builds none** (`f835efb01`, `474163070`), which closes off a whole class of
theory. Per-access eMMC markers and reserved-RAM markers are developed because
the VFS markers do not survive (`58e6c03cf`), and `f834a9b1e mdss: skip late
hw_init when MDSS is runtime-suspended` is the real display fix. Run116 finds a
**DSI PLL defect** (`0dfba107b`). ashmem is enabled and the DSI PLL lifecycle
repaired (`17a964539`), fbdev probes MDP without the boottime gate
(`5a970d696`), and KGSL registers a CPU latency QoS request (`537a374fa`).

**2026-10-15 — context-bank ownership and battery (9 commits).** KGSL cannot
reach its SMMU path until `250485599 gpu: give the gpu context bank a single
owner`, refined by `fef07bec1 gpu: keep kgsl NOMMU and leave the gpu context
bank untranslated`. The QPNP fuel gauge and charger land (`48a3edebe`), the
PMI8950 VADC reference channels are defined so the charger binds
(`a32fe6a2a`), and the battery supply is labelled so the charging-control HAL
registers.

**2026-10-16 — FastRPC, and the GPU hang chain (43 commits).** Packaging is
hardened against duplicate ramdisk records (`9cc4eaab3`). FastRPC converges:
a DMA pool is added and reverted (`5080f3fab`/`a7fe6fb5a`) because the real
problem is a **node-name mismatch** (`d6ec3cfc5`, `a07c1b12c`), then the
missing session is found (`5a2768b8c`), then run149 shows adsprpc failing at an
unimplemented ioctl 12 rather than a driver bug (`6d11ff430`) — with the scope
corrected again because **CAF ioctl numbering differs from mainline**
(`0df15065a`). Two policy label attempts are made and reverted
(`bd02a4791`/`876248591`), the second documenting that **genfscon type validity
is not checkable offline** (`d9c1ba0fa`). The GPU hang is then narrowed step by
step: hangs at ME_INIT but boot completes anyway (`3c5c8c250`), the ring base is
correct and the hang predates ME_INIT (`623bc708b`), RBBM state is sampled at
three points in `a5xx_start()` (`c2dd623be`), clean through `a5xx_start()` and
hanging at microcode load (`01cbe43a0`), bracketed with state probes
(`ac7728da2`, `5ee66cef0`), and finally `4aafe26dc msm: kgsl: soft reset A5xx on
start when GX power is not regulator managed`. A506 KGSL quirks are restored in
DT (`c72069425`), and CP fetch-pointer IB dumps land (`12fd6a087`, `8f6a13ced`).

**2026-10-17 → 10-18 — `master`: the SError and adb (28 commits).** Re-imported
at run198. `0fc24c807` makes the IOMMU use CAF's page-table format for the GPU
bank. The QMSSU SError fix and the false-reset retraction (`915bcfa1f`) and the
UDC/adb unlock (`24bcdc3ed`) — detailed in the sections below. A parallel work
stream adds `msm8953-apm.c` and `qcom-msm8953-mem-acc.c` (`60d877157`).

### What "written from the ground up" actually covers

Not just QSEECom. Written or substantially rewritten for this port:

| component | size | note |
|---|---|---|
| `drivers/misc/qseecom.c` | 10,317 lines | legacy TEE client; upstream has no equivalent |
| `drivers/staging/android/ion` + `compat_ion` | — | upstream `compat_alloc_user_space` removed |
| `drivers/gpu/msm/` (KGSL) | 90 files, ~76k lines | vendor 4.19 → 6.12 |
| `include/uapi/linux/msm_kgsl.h` | 1,884 lines | kept verbatim; blob ABI contract |
| `scripts/initramfs-diag/init.c` | — | custom static init; see trickery below |
| `drivers/soc/qcom/msm8953-apm.c`, `drivers/regulator/qcom-msm8953-mem-acc.c` | 233 / 155 | in flight on `master` |

The KGSL port also needed driver-private compat layers for vendor-only APIs:
`msm-bus-compat.c` (legacy `msm_bus_scale_*` parsed from `qcom,msm-bus,*` DT
properties, falling back to accepted no-op votes when the interconnect path is
absent, since bus voting is perf-only) and `secure_buffer_compat.c`
(`hyp_assign_table()`).

### The trickery, and why each piece exists

- **Custom diagnostic init** (`scripts/initramfs-diag/init.c`, static ARM64).
  Installs read-only fstab and vendor contexts, appends actions to
  `init.qcom.usb.rc` and `logd.rc` via bind mounts, holds a canonical bootdevice
  alias, streams logcat to persistent storage, runs a `sync` loop so a hard
  reset leaves evidence, and manages the USB handoff. This is the single
  biggest reason any of the above was debuggable at all.
- **SELinux policy work** — SSD overlay contexts, observer debug grants, init
  relabel grants, composer caps deltas, backlight and battery supply labels,
  and a policy-append hook in the packaging harness. Several of these are
  explicit "temp-only TEE bypass" experiments to prove a downstream path.
- **The UDC conflict** (`24bcdc3ed`). The diag ACM gadget binds the board's only
  UDC, so adbd can never bind. Gated behind `ANDROID_USB_CONSOLE`, off by
  default. This is what turned "the board resets" into "our own capture ended".
- **pstore/ramoops logdump channel** — committed slots, bounded retry for late
  eMMC attach, SEA fault context capture, and reset-path markers that survive
  into the next boot.
- **Firmware harvesting** — GPU/ADSP/WCNSS/modem firmware staged, exfiltrated
  and rebound late, because upstream expects it staged and this ROM does not.
- **Kernel command line** tuned for survivability: `panic=60`,
  `ramoops.reset_capture=1`, `pstore.backend=ramoops`, `mdss_mdp_cold_boot=1`.

---

## Where this port got to: GPU booted, adb up, real rendering

Summary of the journey from the initial tree to a booting kernel with a working
GPU. Written 2026-10-18. Operational docs are listed under
[Current ACK 6.12 port](#current-ack-612-port-2026-10-17) below.

### Start state

`4d4fcc16b android-6.12 port: tissot bring-up tree at run198`. The ACK 6.12
tree already had upstream mainline support for this hardware — panel, sound
card, touch, sensors, LED/haptics, UFS/SD, USB — but **no GPU driver at all**.
`/dev/kgsl-3d0` did not exist, so the vendor `libGLESv2_adreno.so`, sdm HWC and
gralloc all failed and SurfaceFlinger could not compose. The "GPU 3D" row of
`FAILING-COMPONENTS.md` was backlog #1.

### What had to be ported

**KGSL, forward-ported from CAF OSS 4.19 to 6.12.** This was the bulk of the
work: 93 files, ~76,000 lines under `drivers/gpu/msm/`, taken from the
`../kgsl` CAF OSS tree at `2b2d762c` and converted to 6.12 APIs. Research and
conversion criteria are in
[REFERENCE-driver-forward-porting.md](REFERENCE-driver-forward-porting.md).
Because a compile is not a port, each conversion was checked against upstream
callers, the upstream conversion commit series, and the AOSP sources, with
DMA-BUF reservation, page references vs DMA pins, and compatibility shims
treated as explicit review targets.

**SMMU page-table format for the GPU bank** (`0fc24c807`): mainline's
`qcom_iommu` writes a different page-table format than the CAF SMMU driver the
GPU's firmware expects.

**Display** (`9462ebf80`, `0d5f9bbe4`, `b978a1d3e`): panel policy selected after
node resolution, panel supply topology matched to the stock DT, and reset/TE
pin states restored.

**CPU/regulator prerequisites** (`60d877157`, in progress in a parallel work
stream): `msm8953-apm.c` and `qcom-msm8953-mem-acc.c`.

### Donor-contract violations found and fixed

These were the substance of the port — places where a faithful API conversion
still violated what the CAF driver actually guarantees. Fixed in
`915bcfa1f`, and covered by 13 host regressions under
`porting/logging/test_kgsl_*.py` (all pass under GCC and Clang with UBSan;
`checkpatch --strict` clean):

- msm-bus field order (`<src dst ab ib>`) and bytes/s → kB/s conversion
- parser null-deref and double-free; interconnect lookup error handling;
  signed-overflow dimension check
- `pm_runtime_resume_and_get()` in `kgsl_open()`; IOMMU clock leak on
  global-map failure
- `_aware()` failure state and cleanup; `counter_delta()` baseline update
- boottime snapshot timestamps; removed false `NR_SLAB_UNRECLAIMABLE_B`
  accounting; post-start error propagation in `a5xx_rb_start()`
- fail-closed MMU probing with `of_node_get/put` balance; `FOLL_LONGTERM`
  pinning of user pointers; physical SG mapping/sync that fails loudly on ZAP
  (`389156a34`)

### The SMMU SError, and the false reset it caused

The long-running blocker was an asynchronous `SError` (ESR `0xbf000002`)
inside `kgsl_iommu_start()` on every GPU open, panicking the kernel. run230
removed the port-added `_apply_impl_defs()` and the reported PC moved to an
ordinary kernel load — proving the fault is asynchronous and its reported
instruction was never the culprit. The remaining MMIO was KGSL re-programming
the donor's `GPU_SMMU_HUM_CFG` (`0x6800`) and `GPU_SMMU_DORA_ORDERING`
(`0x64a0`) on every start, under KGSL's own clock vote with no power or
quiesce sequencing. CAF does that from the SMMU driver's power-sequenced reset
path; mainline `qcom_iommu` writes neither register. Issuing those writes
once per boot removed the fault (`915bcfa1f`).

That fix also retired a long-running false lead. Runs 226–233 reported a
"~89 s reset" with a "~47 s silence" before it, and an extensive PS_HOLD /
TrustZone-watchdog theory was built on that. The logdump console snapshots
disprove it: the port kernel was alive at **1695.9 s** (run231), **331.4 s**
(run232) and **323.5 s** (run233), each ending mid-`init` with no panic,
SError or reset marker. What ended every capture at ~43 s was our own ACM
console stream stopping, not the board dying. Every conclusion derived from
where a capture stopped is void.

Two further trickery items, from the `master` era: the **`ramoops`/pstore
logdump channel** (`9d8caf69a`, `03a27bebb`, plus the committed-slot and
bounded-retry work on `mdss-bisect`) so a panic or reset still leaves a
CRC-verifiable console afterwards; and **evidence discipline** — each run a
single-variable change with cmdline, ramdisk, init and config held
byte-identical (asserted with `cmp` before every build), treating the resolved
`.config` as ground truth rather than the defconfig.

### Verified end state (run234)

Measured on the device, not inferred:

- `uname -r` `6.12.93-4k`; `/dev/kgsl-3d0` present; adb up ~35 s after boot.
- SurfaceFlinger: `GLES: Qualcomm, Adreno (TM) 506, OpenGL ES 3.2 V@0502.0` —
  the 2020 vendor blob driving the ported driver, full GLES 3.2 extension set.
- `screencap` returns a real 1080x1920 frame (lockscreen, wallpaper, status
  bar, vendor USB dialog).

That is `/dev/kgsl-3d0` with the stock ioctl ABI, the vendor EGL, gralloc and
sdm HWC together — the "GPU 3D" row closed.

**Still open:** the board has been observed returning to stock on some later
boots (run235/run236); that reset is not yet explained and is the live
question. Separately, several hardware buttons do not work (nav keys are never
reported; the touch digitizer's measured range exceeds the declared one), and
Bluetooth/WiFi/camera lack their vendor drivers entirely. A parallel work
stream is currently landing CPU/regulator prerequisites
(`msm8953-apm.c`, `qcom-msm8953-mem-acc.c`), which is a plausible thread to
pull on for the reset.

## Current ACK 6.12 port (2026-10-17)

- [BUILD_TISSOT.md](../BUILD_TISSOT.md): build, package, RAM boot and capture instructions.
- [CURRENT-STATE.md](CURRENT-STATE.md): what the port does today, and what is still open.
- [HANDOFF.md](HANDOFF.md): current run187 verdict, artifact provenance and next investigation.
- [FAILING-COMPONENTS.md](FAILING-COMPONENTS.md): current failures and historical evidence.
- [TRACKER-kgsl-port.md](TRACKER-kgsl-port.md) and
  [TRACKER-tissot-hardware.md](TRACKER-tissot-hardware.md): GPU work and hardware coverage.
- [REFERENCE-boot-testing.md](REFERENCE-boot-testing.md): operational evidence reference;
  read before a boot cycle.
- [REFERENCE-driver-forward-porting.md](REFERENCE-driver-forward-porting.md):
  4.19 → 6.12 API conversion research, semantic review and focused static checks.
- [policy/ADAPTING-A-ROM-TO-A-NEWER-KERNEL.md](policy/ADAPTING-A-ROM-TO-A-NEWER-KERNEL.md):
  how a ROM developer adapts a ROM's SELinux policy to a newer kernel.

Run234 completes the GPU port: `/dev/kgsl-3d0` is up with the stock ioctl ABI,
the vendor EGL reports `Adreno (TM) 506, OpenGL ES 3.2`, adb works on the port
kernel and `screencap` returns a real frame. The earlier "visible rendering
unresolved" state from run187 (dim backlight, UCHE OOB faults) is resolved; the
GPU 3D row of `FAILING-COMPONENTS.md` is closed. Note also that the long
"~89 s reset" investigation was chasing an artifact of our own USB console
ending — see [HANDOFF.md](HANDOFF.md) before reusing any of that reasoning.
The reconnaissance and phase plan below are the original mainline baseline;
they do not describe the current ACK build or device status.

## Historical reconnaissance baseline

Porting workspace for the Xiaomi Mi A1 (`tissot`, MSM8953 / Snapdragon 625) onto a
mainline kernel. Everything here was derived from one boot of the device plus a
line-by-line comparison against android-mainline's tree.

## Original mainline starting point

`msm8953-mainline/linux` already ships a **`6.12/main` branch with tissot
support** — panel, sound card, touch, sensors, LED/haptics/WLED nodes, UFS/SD,
USB — and a companion repo generates the OTM1911 panel driver for this exact
panel. See `findings/05-prior-art.md`. The phase plan assumes android-mainline as
the target because that is what was asked; if `6.12/main` is good enough,
phases 2-5 shrink dramatically.

## Original phase status

| Phase | Scope | State |
|---|---|---|
| 0 | Device recon, live DT + bootlog capture | **done** |
| 1 | Make the upstream tissot DTS build | **drafted + verified locally** |
| 2 | cpufreq (GCC CPUSS clocks + OPP table) | planned, data ready |
| 3 | Display: OTM1911 DSI panel + backlight | prior art exists |
| 4 | WLAN (wcnss / wcn36xx + firmware) | blocked on firmware extraction |
| 5 | Thermal (tsens-8953), audio, sensors | planned |

## Layout

```
porting/
├── findings/        evidence: what the device is, what works today, prior art
├── logging/         log capture setup: pull-logs.sh + pstore decoder (see logging/README.md)
├── logs/            one dated directory per boot attempt (git-ignorable)
├── plan/            phase-by-phase port plan with verification steps
├── tools/           the build and the boot-test tooling: build-kernel.sh,
│                    kgsl-stall-watch.sh, boot-cycle.sh, stall-hunt.sh
├── drafts/          drop-in files for the kernel tree (pmi8950.dtsi, ...)
├── upstream-refs/   android-mainline + prior-art sources the analysis uses
├── artifacts/       live DTB/DTS, bootlogs, device fact tarball, ssh helpers
└── build-test/      standalone dtc harness that builds the tissot DTS
```

The boot images and the stock boot backup are not in the repository: they are
in `../ack-kgsl-artifacts`, next to it, with a README there that lists each
one and its checksum. `porting/artifacts/snapshot60/` holds the per-run
evidence, which is generated and git-ignored.

## Building the port kernel

The full procedure is `BUILD_TISSOT.md` at the repository root, and it is
scripted:

```sh
cd /path/to/ack-kgsl
bash porting/tools/build-kernel.sh runNNN     # an unused run number
```

The image is flashed to `boot_b`; the phone then comes back on the port
kernel by itself after a power cycle, and a hung kernel is recovered by
forcing the phone off and flashing the previous image from fastboot.

Five things about the build are not what a kernel tree normally does, and
each one has bitten this port:

**The configuration is a file in the repository, not the defconfig.**
`arch/arm64/configs/defconfig` is dead code here. The build copies
`porting/tools/kernel.config` into `out/.config` and runs `make
rustavailable` before `olddefconfig`. A resync with the wrong toolchain
turns Rust and ashmem off while the C kernel still builds, and the
framework then loses `/dev/ashmem`. The symbols the port needs on top of
it — KernelSU-Next, the audio port, slimbus and zram — are forced
explicitly, because the configuration is not the builder's to edit.

**The kernel is built from the repository itself.** There is no staged
tree: `make -C . O=out` compiles what is committed. There used to be one,
and it was refreshed from the repository before every build and then
verified against it, file by file, because the staged tree was what the
compiler read. A stale staged tree silently produced a kernel that did not
contain the repository's fixes — exactly how the capacitive navigation
keys came to be missing from a flashed image while the volume keys worked:
the keys were committed to the repository and the staged tree was a day
older than the commits. The same staleness kept the CAF fastrpc ioctl ABI
out of the kernel, which is why the `adsprpcd` daemon crash-looped with
`failed to send del client cmd`. Building the repository removes the
whole class of failure.

**The /init is not Android's.** The boot image carries a diagnostic ramdisk
whose init is `scripts/initramfs-diag/init.c`, rebuilt from source on every
build so an image never carries a stale init binary. It hands PID 1 to the
ROM's init and does five things around that handover:

* **The bootdevice guard.** It binds a read-only protected fstab
  (`/dev/block/ack-protected-fstab.qcom`), the vendor file contexts, the USB
  controller property, the early-logcat rc and the display-gate rcs, and it
  releases the canonical bootdevice alias and the legacy wait path — all
  *before* the ROM's init opens its SELinux policy. That window is about
  1.35 seconds wide. The guard failing means the ROM's init waits forever
  for a boot device that nobody releases, and the phone comes up on a dim
  lit screen with no adb and no USB at all. Do not delay the guard by
  anything.
* **The log capture.** It streams `/dev/kmsg` into the `logdump` partition
  and into a USB serial gadget, and it keeps a bounded USB console. The
  `logdump` partition is the reason a boot can be read back after the fact.
* **The ramoops sampler.** A bounded trace of the DIAG signal and process
  events into the ramoops area, so the last reset survives a hard hang.
* **The firmware harvest.** It copies the modem's firmware out of the
  running system (`a530_pm4.fw` and friends) so it can be staged into a
  later image.
* **The adsprpc compatibility.** Downstream `adsprpcd` opens
  `/dev/adsprpc-smd`; upstream FastRPC registers `/dev/fastrpc-adsp`. The
  init creates the downstream name as an alias of the upstream one.

**The SELinux policy is the ROM's, and the port works with it rather than
on it.** The effective policy is the ROM's precompiled policy, loaded by
the ROM's init from its own file. The port's base image carries a patched
`userdebug_plat_sepolicy.cil` and a `force_debuggable` trigger, and the
port can append a CIL fragment to it — but the *effective* policy is still
the ROM's, so a runtime policy rewrite is the only thing that would change
it at run time, and that is what KernelSU-Next's `SET_SEPOLICY` supercall
does. **On this kernel that supercall deadlocks: the first attempt froze
the phone, and the manager's "allow an app root" flow does the same thing.**
The manager's *adb root* feature is fine — it puts the adb shell in the
`u:r:ksu:s0` domain as uid 0 without touching the policy, and that is the
supported way to get root here. Use it; do not use the grant-root path.

The other way the ROM's policy and the 6.12 kernel disagree is by label.
The ROM's policy was written for the 4.19 CAF kernel, and the 6.12 kernel
puts the per-device wakeup statistics where the policy does not expect
them, so they land on the generic `sysfs` type and the suspend service is
denied reading them: 460 denials a boot, a failed checkpoint commit, a
reboot request, and a reboot that never finishes. The fix is a label, not
a policy change — the policy already has the type the ROM expects
(`sysfs_wakeup`) — so the port's init relabels those nodes and the kgsl's
power attributes. The relabel has to happen in the initramdisk's own mount
namespace, before the guard chroots into the ROM's root, where `/sys` is
not the kernel's.

**Root comes from KernelSU-Next, built into the kernel.**
`drivers/kernelsu/` is the v3.4.2 tag vendored in, built in rather than
loaded as a module because the port has no module autoload and the driver
has to be there before the manager talks to it. The version is pinned on
the make command line (`KSU_GIT_VERSION=3355 KSU_GIT_VERSION_VALID=1
KSU_GIT_TAG=v3.4.2`) because the driver derives its version from its own
commit count, which it can only do when it lives in a repository of its
own; vendored here it would fall back to version 1, which no manager
accepts. 33355 is the version in the v3.4.2 manager APK's name, and the
certificate the driver requires is the one that release was signed with.

**The images live outside the repository.** A boot image is tens of
megabytes and the stock boot backup is the only copy of the phone's
original boot partition, so both are in `../ack-kgsl-artifacts` with a
README listing each one. The repository keeps the documentation and the
tooling; the per-run evidence is generated, git-ignored and disposable.

## Reproduce the two verified claims

```sh
cd build-test
sh fetch-includes.sh                       # dt-bindings headers (once)

sh build-dts.sh dts-upstream msm8953-xiaomi-tissot.dts /tmp/o.dts
#   -> fatal error: 'pmi8950.dtsi' file not found     (upstream does not build)

cp ../drafts/pmi8950.dtsi dts-patched/
sh build-dts.sh dts-patched msm8953-xiaomi-tissot.dts /tmp/o.dts
#   -> 76,593 bytes, only style warnings              (fix works)
```

Collect fresh device facts (run on the device, needs root for the FDT):

```sh
artifacts/collect/collect-device-facts.sh /tmp/device-facts.tar
```

## Evidence base

* **Live device tree** — `artifacts/tissot.dtb` (67,341 B) pulled from
  `/sys/firmware/fdt`, decoded to `artifacts/tissot-live.dts` (4,014 lines).
  This is the DT the running kernel actually received, bootloader edits included.
* **Bootlog** — `artifacts/dmesg-live.txt` (692 lines, to t≈10,600 s), plus the
  full journals and the running kernel's `kconfig` inside
  `artifacts/device-facts.tar`.
* **Upstream sources** — `upstream-refs/msm8953.dtsi`, `pm8953.dtsi`,
  `msm8953-xiaomi-tissot.dts` from android-mainline `upstream-master`
  (currently 7.2.0; there is no `android-msm-6.12` branch any more), plus the
  prior-art 6.12 tissot DTS.

The reference kernel for these captures is postmarketOS `6.17.7-msm8953`, not
android-mainline and not 6.12. It is used only as an oracle for "what does this
hardware do without Qualcomm downstream code".

## Related tree in this directory

`../kernel_xiaomi_msm8953` is the CAF/GKI **4.19.325** tree
(`halcyonlv/kernel_xiaomi_msm8953`, branch `16.2`) with an in-progress msm8953
port (`arch/arm64/boot/dts/vendor/qcom/mi8953/tissot/tissot.dtsi`,
`arch/arm64/configs/vendor/tissot.config`). Useful as a downstream reference and
source of stock CAF DTs; its driver stack cannot be carried into a mainline port.
