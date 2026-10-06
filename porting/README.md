# tissot / Mi A1 → android-mainline

Porting workspace for the Xiaomi Mi A1 (`tissot`, MSM8953 / Snapdragon 625) onto a
mainline kernel. Everything here was derived from one boot of the device plus a
line-by-line comparison against android-mainline's tree.

## Read this first

`msm8953-mainline/linux` already ships a **`6.12/main` branch with tissot
support** — panel, sound card, touch, sensors, LED/haptics/WLED nodes, UFS/SD,
USB — and a companion repo generates the OTM1911 panel driver for this exact
panel. See `findings/05-prior-art.md`. The phase plan assumes android-mainline as
the target because that is what was asked; if `6.12/main` is good enough,
phases 2-5 shrink dramatically.

## Status

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
├── drafts/          drop-in files for the kernel tree (pmi8950.dtsi, ...)
├── upstream-refs/   android-mainline + prior-art sources the analysis uses
├── artifacts/       live DTB/DTS, bootlogs, device fact tarball, ssh helpers
└── build-test/      standalone dtc harness that builds the tissot DTS
```

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