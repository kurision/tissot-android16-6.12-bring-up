# kgsl a5xx bring-up: port vs. CAF reference, function-level parity diff

> **Historical record - 2026-10-18.** This is what
> the evidence showed at the time and has not been rewritten. The
> current state is [CURRENT-STATE.md](CURRENT-STATE.md).

Last update: 2026-10-18, after run224. Results 1 and 2 are a source-vs-source
diff (no build, no boot cycle). Result 3 was **rewritten** from run224 evidence:
its original text read the end of the ACM capture as the end of the boot.
All "at parity" / "diverges" statements below are about *source*, and none of
them is a device-failure verdict.

Reference pinned at `../kgsl` `2b2d762c0b843c560d2ae2203f6b3cfe1474fb3f`
(same pin as `TRACKER-kgsl-port.md`).

## Why this exists

After run223 (SECVID TSB parity) still reset, the decision was to stop guessing
and diff the port's a5xx bring-up against the maintained CAF driver
register-by-register, the way `_set_secvid()` was found. This is the result of
that pass.

## Method

1. Extracted every function body from both `adreno_a5xx.c` files and scored
   pairwise similarity (normalised text, comments stripped). 46 functions in
   common, 20 port-only (mostly this port's diagnostics), 10 reference-only.
2. Diffed every function scoring below ~0.85 by hand, plus the register tables
   (`adreno-gpulist.h`) and the entire `a5xx_start` sequence.
3. Re-read the run222 capture to see what the device actually does, so the diff
   could be pointed at the path that runs before the reset.

## Result 1 — the a5xx hardware bring-up is at parity

Confirmed identical (modulo struct type name `adreno_reglist` vs
`kgsl_regmap_list`):

* `a530_vbif_regs` — a single write, `A5XX_VBIF_ROUND_ROBIN_QOS_ARB = 3`.
* `a50x_hwcg_regs` — 40 registers, byte-for-byte the same list.
* `_setprotectreg`, `_preemption_init`, `_execute_reg_sequence`,
  `_read_fw2_block_header`, `_do_fixup`, `_poll_gdsc_status`,
  `a5xx_gpmu_init`, `a5xx_restore_isense_regs`, `a5xx_cp_hw_err_callback`,
  `gpmu_set_level`, `lm_limit` — similarity 1.00.
* `a5xx_send_me_init`: the port's `_set_ordinals()` expands to exactly the
  reference's inlined `CP_INIT_MASK` ordinal sequence
  (`adreno_a5xx.c:1696-1713` and `:1767-1790` vs the port's `_set_ordinals`).
* `a5xx_start` register sequence: same writes in the same order. The
  differences are structural, not values — the reference moved
  `kgsl_mmu_start()`, `adreno_get_bus_counters()`, `adreno_perfcounter_restore()`
  and the 64-bit address-mode block *into* `a5xx_start`; the port does those in
  `_adreno_start` before calling `gpudev->start()`. Net effect is the same.
* 64-bit addressing: reference writes the 12 `*_ADDR_MODE_CNTL` registers
  unconditionally; the port gates on `MMU_FEATURE(KGSL_MMU_64BIT)`. run222
  logs `DIAG KGSL_MMU_64BIT enabled`, so the port takes the same branch.

The a506 core definition differs in cosmetic/layout fields only
(`snapshot_size`, `uche_gmem_alignment`, `perfcounter`, `busy_mask`), plus
`ADRENO_64BIT` which the port adds to the feature set and the reference now
assumes. Device-facing values agree: `gmem_size = SZ_128K + SZ_8K`,
`bus_width = 16`, `highest_bank_bit = 14`, `a530_pm4.fw` / `a530_pfp.fw`.

**Conclusion: the GPU register programming is not where the port diverges.**

## Result 2 — the divergences that remain, ranked

### D1. ZAP / secure-mode policy is inverted (high signal)

The reference will not touch the ZAP or secure mode on an MMU that is not
secured; the port does both.

* Reference `a5xx_microcode_load()` (`kgsl/adreno_a5xx.c:1663`):
  `if (!device->mmu.secured) return 0;` — ZAP is never even loaded.
  Its `a5xx_rb_start()` (`:1870-1887`) then takes the `!zap_loaded` branch and
  writes `A5XX_RBBM_SECVID_TRUST_CNTL = 0` from the CPU.
* Port `a5xx_microcode_load()` (`ack-kgsl/.../adreno_a5xx.c:1873-1882`) loads
  the ZAP **regardless of `mmu.secured`**, with a comment stating this is
  deliberate ("ZAP is needed even without a secure MMU context bank").
  Its `a5xx_rb_start()` (`:2291`) therefore calls `adreno_set_unsecured_mode()`
  (`adreno.c:2092`), which - because `zap_loaded` is now true - takes
  `adreno_switch_to_unsecure_mode()` (`adreno.c:2070` → `CP_SET_SECURE_MODE`)
  and **never writes `SECVID_TRUST_CNTL` from the CPU**.
* With this port's DT and config (`mmu.secured == false`: the kgsl-iommu node
  carries only `gfx3d_user`, no `gfx3d_secure`; `CONFIG_QCOM_SECURE_BUFFER` is
  not set in `out/.config`), the reference driver would also evaluate
  `mmu.secured == false` (`kgsl/adreno.c:2186`) and would therefore take the
  *CPU-write* branch. The port takes the *secure-CP* branch.

Caveat, stated plainly: this is a real behavioural divergence with a plausible
mechanism (an authenticated ZAP plus `CP_SET_SECURE_MODE` on a CP with no
secure context bank), but it is **not** established as the reset cause. The
port already reset on both sides of this fork — run205 with the ZAP loaded and
run≤202 on the CPU-write fallback — so this policy alone does not explain the
reset, and adopting the reference's policy is not guaranteed to fix it.

Port-side divergences in the same area, for completeness: the port programs
`SECVID_TSB_CONTROL`/`TRUSTED_BASE`/`TRUSTED_SIZE` unconditionally
(`adreno.c:2042-2060`, the run223 change), where the reference does it only
under `if (device->mmu.secured)` (its `a5xx_start`, `:1495`). drm/msm
`a5xx_hw_init()` is the precedent for the port's version.

### D2. Power-on path is a different generation of the code (high signal)

`kgsl_pwrctrl.c` scored far worse than `adreno_a5xx.c` and the differences are
architectural, not cosmetic:

| step | port | reference |
|---|---|---|
| rail on/off | `_enable_regulators()` / `ftbl->regulator_disable_poll()` (`kgsl_pwrctrl_pwrrail`) | `enable_gdscs()` / `kgsl_pwrctrl_disable_gx_gdsc()` / `kgsl_pwrctrl_disable_cx_gdsc()` + `kgsl_regulator_set_voltage(parent, corner)` |
| clock rate | `kgsl_clk_set_rate(device, level)` | `device->ftbl->gpu_clock_set(device, level)` |
| bus vote | `kgsl_pwrctrl_buslevel_update()` → the port's bus shim | `kgsl_bus_update(device, KGSL_BUS_VOTE_{ON,OFF})` |
| level on enable | `wakeup_maxpwrlevel` / `popp` / `default` | `default_pwrlevel` only |
| pwrlevel order | bus before clock always | bus before clock on increase, after on decrease |
| TLB | - | `kgsl_mmu_send_tlb_hint()` on rail on/off |

The bus layer is the starkest: the reference's `kgsl_bus.c:237` does
`of_icc_get(dev, NULL)` and **fails probe** if there is no `interconnects`
property and no GMU to scale bandwidth. The port has no `kgsl_bus.c` at all
(`msm-bus-compat.c` instead) and its GPU node has no `interconnects`, so every
bus vote is accepted-and-warned rather than applied. This is a known, still
open defect — it is called out in the port's own DT comment
(`msm8953-xiaomi-tissot.dts:575-583`).

### D3. Smaller items, recorded for completeness

* `adreno_a5xx_gpudev` ops table (0.08 similarity): the reference exposes
  `clear_pending_transactions`, `is_hw_collapsible`, `hw_isidle`,
  `power_stats`, `read_alwayson`, `zap_shader_resume` as ops. The port keeps
  `adreno_clear_pending_transactions()` in `adreno.c:1856` and calls it from
  `adreno_stop()` (`:2539`) and `adreno_reset()` (`:2618`); the reference calls
  it from `adreno_stop()` and `adreno_soft_reset()`. Functionally the same
  idea; the port also invokes the same VBIF halt/ack sequence. Not chased
  further.
* `a5xx_err_callback` / `a5xx_irq_storm_worker` / `a5xx_critical_packet_*`
  differ mainly by this port's diagnostics and by interrupt-mask bookkeeping.
* `a5xx_init` (0.43) and `a5xx_microcode_read` (0.53) are refactors around
  firmware/feature probing, no register-sequence change found.

## Result 3 — the bring-up completes; the board dies later, elsewhere

**Rewritten after run224 (2026-10-18).** The text that stood here was written
from `run222-usb.log` alone and read the end of that file as the end of the
boot. It is not: the ACM/diag stream is cut by the diag init's deliberate USB
handoff (`ACK diag: USB handoff … configfs=2`, 41.82 s) *before* `a5xx_start`,
so that stream cannot contain the bring-up at all. The bring-up lives in the
persistent logdump channels — stage records at `logdump+0x3f01000`, console
snapshot at `logdump+0x3c00000` — which were not read at the time.

What those channels show for run224 (markers added in that build); run222 and
run223 carry the same 24 stage records:

* `_adreno_start` runs to completion, step by step: `entry state=128` →
  `rail-probe left_on=1` → `bus-cfg-high done gpu_cfg=0` → `AWARE ok state=32
  gfx_rate=650000000` → `mmu-start ok` → `gmu-path done (secured=0)` →
  `secvid done` → `addr-mode done 64bit=64` → `gpu-start returned`
  (41.6752–41.6761 s).
* `a5xx_start` runs (`bringup[start-entry]`, `[ahb-reporting-on]`,
  `[preemption-done]`, `[rb-programmed]`); the a506 zap is loaded and
  authenticated (`DIAG zap: a506_zap.mdt 8192 bytes at 0x81800000
  auth_and_reset=0`); `microcode-loaded`; `me-unhalted cp_status_1=8C42440A`;
  `ME_INIT submit … zap_loaded=1`; `secvid: zap_loaded=1 ret=0
  trust_cntl=00000000`. All by 41.6888 s.
* All 24 stage records are present (0–10 and 16–28), including the
  OPEN/PROCESS_OPEN/IDLE/ACTIVE/NAP group. No SLUMBER (29/30) — but the stage
  writer is a deferred ordered workqueue lagging 40–150 ms, so a record created
  in the last ~150 ms before a hard reset is lost; absence of 29/30 is **not**
  evidence that `_slumber()` never ran.

So Result 3's premise is refuted: `a5xx_start` runs, the registers program, and
the CP accepts `ME_INIT`. The death is *after* a complete, successful bring-up.

Timing, run224: GPU service 41.22 s, `_adreno_start` 41.675 s, zap auth 41.688 s,
userspace still healthy at 41.848 s (surfaceflinger/zygote running; `ppd`,
`boringssl_self_test_apex32` starting), then the board hard-resets at ~42.0 s —
the diag gadget (18d1:9008) is on the bus 11:31:09→11:31:32 and the USB device
simply disappears. The next stock boot's `qcom,qpnp-power-on` reports
`Triggered from Hard Reset and 'cold' boot`: an abrupt PS_HOLD-level reset with
no warm-reset magic, not an orderly reboot. The port's APSS watchdog is
disabled (`DIAG apps_wdt en=00000000`, probed state). `DIAG pon_reason
reason1=0x21 poff1=0x02 …` is unchanged from every earlier run, i.e. the PON
reason bits do not discriminate this death.

## Where the log is blind

Every channel misses the last moments:

* ACM/diag — cut by the USB handoff at 41.82 s, before the GPU service.
* Console snapshot (`fs/pstore/ram.c`, `RAMOOPS_SNAP_INTERVAL_MS = 2000`) —
  last sample 41.856 s in run224, ~150 ms short of the reset.
* Stage records — deferred workqueue, ~40–150 ms behind, tail lost on a hard
  reset.
* `adreno_zap_stage()` prints nothing: it only queues a block write, so the
  stage timeline is not in the console ring either.

The reset time is ~42.0 s in all three runs while the GPU start varies
(41.566 / 41.680 / 41.747 s), which argues for an event correlated with *time*
rather than a fixed delay after the GPU start — but the reset-time estimate is
only good to ~±0.5 s (gadget-appearance time is the anchor), so this is not
settled.

## Suggested next probe (one boot cycle, high yield)

Instrumentation only, no behavioural change:

1. `RAMOOPS_SNAP_INTERVAL_MS` 2000 → 200 so the last console sample lands
   within 200 ms of the reset.
2. `pr_info()` inside `adreno_zap_stage()` (before the "first visit" bit is
   set) so every stage transition lands in the console ring immediately, not
   only in the deferred block write.
3. Markers on the paths that can kill the board after a clean start: the first
   ringbuffer submission (`adreno_ringbuffer_issueibcmds`), the GPU fault path
   (`a5xx_fault_detect` / `adreno_gpu_fault`), the state machine
   (`kgsl_timer`, `kgsl_idle_check`, `kgsl_pwrctrl_change_state`) and the
   clock/rail/axi on/off helpers.

If a submission is then visible in the last 200 ms, the next step is the
SMMU/fault side. If nothing prints at all, the reset is below the kernel
(power transient / PS_HOLD drop) and the next experiment is behavioural — e.g.
clamp the GPU to the lowest power level for one boot.

Do not combine this with a behavioural change — one variable per boot cycle.

## Explicitly unverified

* That D1 (ZAP policy) causes the reset. The port reset on both sides of that
  fork, and run224 shows the zap path returns 0 all the way through
  `auth_and_reset`.
* That D2 (power-on path) causes the reset. It is the *evidence-indicated*
  area, not a demonstrated cause; run224 shows every `_adreno_start` step
  returning success, which narrows D2 to what happens *after* the start.
* That the reset is time-correlated (~42 s) rather than a fixed delay after
  the GPU start. The GPU start varies across runs while the reset stays ~42 s;
  the reset-time estimate itself is only ±0.5 s.
* That adopting any reference behaviour would fix the reset. The reference
  itself has not been built or booted on this device.
