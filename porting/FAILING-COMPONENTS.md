# Open components and daily-driver blockers — tissot

Reviewed **2026-10-27**, after a read-only capture on ACK #106. This is the
current work queue, not a chronological bring-up log. See
[CURRENT-STATE.md](CURRENT-STATE.md) for evidence and [README.md](README.md)
for the overview. Dates/builds in each row define its coverage; the latest call
test did not retest every component, and the newest capture retested none of
the camera, call or suspend paths.

**Voice-call audio now works in the tested handset and speaker paths.**
Two calls passed on ACK 6.12 #93, followed by a successful slot-A 4.19 call
with Magisk root. Both were tested with the ROM HAL setting enabled; persistence
has not been installed or reboot-tested. The older blanket CVP/call-failure
status is superseded by the [call-audio report](REPORT-call-audio-2026-10-25.md).
Daily-driver readiness remains unestablished because other items below are
unresolved or lack current-image qualification.

## Working functions with remaining limits

| Component | Verified progress | Still open |
|---|---|---|
| Calls | User-confirmed two-way handset/speaker audio on #93 twice and 4.19 slot A once; HAL selects VoiceMMode1 | Persist the multi-session property before HAL startup; reboot/accessory/other-SIM coverage |
| Android / display / touch / GPU | Interactive UI, touch/navigation and vendor GLES demonstrated | Sustained stability, fault recovery and suspend/wake |
| CPU / idle / memory | Shared DVFS policy and per-core idle residency observed; direct run403 memory measurements retained | Transition overhead, reclaim/exec-performance gap, endurance and sustained thermal qualification |
| Media playback / recording | Hardware decode in tested playback, earlier speaker playback, screen recording and successful save | EOS/underruns, independent post-call media check, hardware encoder execution |
| Camera | Front/rear preview and decodable video demonstrated | Correct post-fix stills, orientation, telephoto/zoom and flash |
| Wi-Fi / Bluetooth / modem / charger | Basic connectivity, tested calls and charging interfaces demonstrated | Resume/recovery, data-service completeness, charging accuracy and long-run reliability |

The 2026-10-27 capture on #106
([findings](findings/2026-10-27-fresh-state-106.md)) associated to a 5 GHz
WPA_PSK AP, then lost the link (BEACON-LOSS, then `reason=4`) with the screen
off, and was still disconnected about 27 minutes later.
Wi-Fi suspend ABI calls fail on that image
(`setSuspendModeEnabled` code 1, `wpa_driver_nl80211_driver_cmd` `errno:95`).
Cause and effect between the two are not established.

**Suspend cannot be tested while the phone is attached to a USB host.** On
#106 the `dwc3-link` wakeup source is held for the whole boot, so
`pm_get_wakeup_count()` blocks and the framework records `suspend attempts: 0`.
That is the intended behaviour of `078fe0316`, not a defect; a suspend/resume
test requires the phone unplugged (or the host suspending the bus). See the
capture's suspend section before drawing any conclusion from zero attempts.

## Known failures and missing integration

These are retained failures or confirmed gaps. Each row names the newest image
on which it was seen, or says it was not reproduced there; warnings alone are
not proof of a broken device. The
[failure inventory](findings/2026-10-24-live-failure-inventory.md) retains
raw source/line references and provenance, including truncated and stale data.

| Priority | Component | Established failure or gap | Next check |
|---|---|---|---|
| 1 | Unresponsive phone after charger removal | User needed a long Power press. The older retained console ends at 4323.729 s without a captured panic/orderly shutdown; reset slot is stale | Reproduce unplug/screen-off/wake with live identity and logs. Wi-Fi, heat, battery loss and suspend are not established causes |
| 1 | USB attachment / resume / charging fallback | Retained QUSB2 PLL/resume errors and an unattached UDC; rebind restored enumeration in the later investigation. On #106 attached to a PC the UDC reads `configured` and no PLL/resume error appears, but the retained dmesg covers only 188 s | Verify current charger-extcon wiring and donor suspend quirks on an identified image, including repeated cable and sleep cycles. Existing ADB connectivity is not this test, and the `dwc3-link` wake source suppresses suspend while attached |
| 1 | Camera flash / torch | Run401 HAL reports no flash on either camera; no torch LED node; donor PMIC flash resources/camera link are omitted. #106 still exposes only `lcd-backlight`, `mmc0::`, `mmc1::`, `white:indicator` | Review/port the real donor integration, then test both torch and camera flash. No repair is established |
| 1 | Camera still-photo correctness | Last retained #44 rear/front photos hit JPEG RESET32 EFAULT, hybrid fallback and malformed restart streams | Qualify a verified post-fix image with new rear/front photos and warning-free decode. The fix is in source; later call tests did not test stills |
| 2 | Fingerprint HAL | Run401 crash buffer contains a service SIGSEGV at address 0xc0. On #106 the HAL started at 57 s and was still running at 4010 s uptime; the only retained tombstone is the stale 2026-10-09 one. `dumpsys fingerprint` still reports one HAL death since last reboot and no enrolled prints | Enrol a print and exercise unlock to distinguish "HAL up" from "fingerprint working". Trace the HAL death counter's origin; a running service is not a functional pass |
| 2 | FastRPC / adsprpcd | Retained repeated listener-open ENOSPC and 25 ms retry loops. **Not reproduced on #106**: zero `ENOSPC`/`adsprpc`/`fastrpc` hits in the full logcat, `/dev/adsprpc-smd` present | Trace the actual operation/allocation and cleanup on an image that does reproduce it. ENOSPC is not proof of full storage; earlier ioctl fixes do not qualify listener recovery |
| 2 | Peripheral manager / proxy | Expected SSR discovery interfaces absent; repeated manager/proxy exits. #106 repeats it: `/sys/bus/msm_subsys/devices` missing, 102 `SSR root dir` failures, 102 proxy and 51 manager failures between 12:55:42 and 12:59:52, `exited 4 times in 4 minutes` | Compare actual donor discovery and lifecycle before adding compatibility behavior; do not fake nodes or use service-stop guards as a fix |
| 2 | UCE / `data.factory` HIDL interface-start flood | Not in the older inventory as an operational item. On #106 two unresolvable `ctl.interface_start` requests (`com.qualcomm.qti.uceservice@2.3::IUceService`, `vendor.qti.data.factory@2.0::IFactory/default`) repeat about twice a second; both libraries exist but no service is declared | Declare/repair the services or drop the init triggers. It is not a kernel bug, but it writes the kernel ring buffer away in minutes and costs every capture its boot log |
| 2 | Modem recovery / data-service reliability | Retained IPA assertion and remoteproc recovery; the reported crash counter is not ten independently retained events. On #106 `remoteproc0..2` are all `running` with no recovery in the retained window; one `.dataservices` ANR fired 12:15:12 on the BOOT_COMPLETED broadcast | Retain a complete repeat and correlate firmware/host transactions. Working calls do not qualify all cellular data/IMS services |
| 2 | Audio EOS / low-latency underruns | Earlier deep-buffer playback advances, while EOS close timeouts and underruns remain; related error messages also occur during working calls. #106 recorded 501 `out_write: underrun` warnings in 7.2 s on the speaker path | Reproduce the specific media/completion failure. Do not reopen the resolved call-silence diagnosis from those messages alone |
| 1 | Wi-Fi `reason=4` disconnects (TX-completion ABI) | Verified on #106 by kprobe: all 214 sampled `WCN36XX_HAL_OTA_TX_COMPL_IND` (type 130) payloads are 16 bytes, but `wcn36xx_smd_tx_compl_ind()` requires exactly `sizeof(struct wcn36xx_hal_tx_compl_ind_msg)` = 12 and returns `-EIO` (`smd.c:2733`), so `wcn36xx_dxe_tx_ack_ind()` never runs and the 100 ms `tx_ack_timer` clears `IEEE80211_TX_STAT_ACK` (`dxe.c:393`) although the driver advertises `REPORTS_TX_ACK_STATUS` (`main.c:1503`). Consequence in dmesg: `send auth … (try 1/3)` → `(try 2/3)` after ~103 ms → AP `deauthenticated … Reason: 2=PREV_AUTH_NOT_VALID`. Same struct exists in upstream mainline, so this is firmware `1.5.1.2` vs driver ABI, not a local edit. Drops were captured with the screen **on** (14:08:52, 14:23:19) and off (14:32:08) | Source fix applied, not built or booted: `wcn36xx_smd_tx_compl_ind()` now accepts exactly the 12- and 16-byte layouts and reads `status` at the common offset 8; a host harness covers both plus the reject path. The trailing word is a TX BD token per the donor prima `tTxComplParams {status, dialogToken}`, and is deliberately unused because only one management skb is tracked. Requalify on a boot cycle. `setSuspendModeEnabled` `errno:95` is post-disconnect cleanup, not causal |
| 1 | Wi-Fi screen-off reconnect (no scheduled scan) | Verified by kretprobe: `nl80211_start_sched_scan` returns `-95` 6/6, 2 s apart; framework logs `PNO scan failed with reason: -1` / `Failed to start PNO scan`, and `numPnoScanAttempts=0`. Cause: `ieee80211_sched_scan_start()` returns `-EOPNOTSUPP` when `local->ops->sched_scan_start` is NULL (`net/mac80211/scan.c:1317`) and `wcn36xx` implements no `sched_scan_*` op. Normal scan continues while off (`numScans` 40 → 69 in ~9 min), so the radio and scan path are alive. Screen-on auto-join is erratic (2 s once, 2 min 22 s once, none within 30 s/20 s). The HAL already carries offloaded PNO (`SET_PREF_NETWORK_REQ` 147, `PREF_NETW_FOUND_IND` 153, `set_pref_netw_list_req` with scan timers; the stock 4.19 prima driver used it) | Map `sched_scan_start/stop` onto `SET_PREF_NETWORK_REQ` and report `PREF_NETW_FOUND_IND` through `cfg80211_sched_scan_results()`. First prove the firmware accepts the request (response status), then that a found-indication arrives, before claiming reconnect works. A host-side repeating-scan timer is a worse fallback |
| 2 | Wi-Fi resume / wake traffic | Earlier resume/TX warnings and later suspend traffic wake evidence. The #106 suspend-ABI failures (`setSuspendModeEnabled` code 1 twice, private driver command `errno:95` four times, 14 `acquire_supported_features -3`) are cleanup paths and not the disconnect cause | Qualify the committed multicast/broadcast suspend filter, and decide whether the private-command ABI is expected to be unimplemented. Preserve unicast wake and awake traffic behavior; measure drain separately |
| 2 | Charging-current / input-limit behavior | Retained erratic current readings and an earlier 100 mA input-limit state | Check calibrated current/sign handling and cable enumeration on a verified image before using readings as an energy benchmark |

## Temporary repairs and qualification work

| Component | Actual status | Required completion |
|---|---|---|
| ROM call configuration | `vendor.audio.feature.multi_voice_session.enable=true` plus audio-HAL restart produces working calls. Runtime override remains enabled on the last inspected 4.19 boot | ROM property integration before HAL initialization, then reboot and call verification. No new kernel rebuild is needed for the runtime workaround |
| dpmd / HIDL mapping | Binary-size repair survived two prior temporary service starts (120.647 s / 60.695 s), with hashes/domain checked. On #106 no abort occurred: PID 925 was up for the whole capture and `init.svc.dpmd=running` | Persistent integration and functional data-service coverage. The run401 bind is not assumed active after intervening reboots; suppressing a service is not a repair |
| Diagnostic transport | CAF diag driver was subsequently added in `0286193ba3`; the earlier absent-node inventory predates that change. `/dev/diag` and two `ffs-diag` nodes exist on #106 | Verify the daemon/HIDL path and functional transport; a present node does not close the service issue |
| Camera JPEG compatibility | `e7bb44d74` (formerly `4646f53406c3`) fixes six kernel-pointer/usercopy paths; original-code control fails and patched host checks pass | Retain a post-fix hardware reset and correct saved photos. The old claim that every later source image lacks the fix is obsolete |
| Camera orientation / telephoto / zoom | QRD mount-angle and donor EEPROM-voltage corrections are in source; earlier ACK exposed two cameras versus three on donor | Revalidate orientation, third-camera enumeration, zoom and repeated preview/capture handoff on a known image |
| Hardware video encoding | Software AVC selection matches the same ROM on donor 4.19; device QUERYCAP is not a completed encode | ROM/framework codec-registration review and a real hardware-encode test. No new kernel encoder regression follows from software selection alone |
| Storage permissions | One mode-2700 ScreenRecords directory was changed to 2775; a 26,553,766-byte / 673-frame recording then saved | Determine metadata origin and whether other directories are affected. No general migration fix or ROM/kernel attribution is established |
| Battery / thermal / performance | Per-core collapse is observed. Run403 reduced sugov CPU to about 2.5% and parked ramoops at 0.0%, but the exec gap and charging-current concerns remain. On #106 `dumpsys thermalservice` still reports `HAL Ready: false` and no vendor thermal-HAL binary exists in `/vendor/{bin,lib64}/hw`; only `thermal-engine` runs | Controlled screen-off drain (needs the phone unplugged), matched performance tests, sustained CPU/GPU mitigation and a vendor thermal HAL. Current source disables periodic snapshots; older enabled-snapshot descriptions are historical |
| New build/configuration tuning | AutoFDO profile-use and RCU/configuration changes are in source | Build/image provenance and comparative hardware evidence; no success claim from the audio tests |
| GNSS / haptics / IR / other sensors / notification LED | No fresh functional qualification; historical inventory includes porting gaps. #106 has `gnss_service` running and QMI sensor transport errors (`-16`, 4×), but no `/dev/gnss*` node | Inspect current implementation and test each function, starting with the location HAL's actual device path. Keep unknown status distinct from a demonstrated failure |
| Panic / freeze observability | Ramoops console retention works; older controlled-panic results cover their own images. Hung-task/buddy-watchdog limits remain | Separately approved current-image panic/recovery test; do not infer that every freeze leaves a panic record |
| Long-run GPU / security services | GLES rendering and selected QSEE transactions work | Sustained rendering/fence recovery and full credential/keystore qualification, without treating old warnings as universal failure |

## Landed fixes that should not be reopened from old reports

| Change | Verified scope / limit |
|---|---|
| Audio `0f033a84a7`, `c63c7a8b3d`, `82513bf45b` | CVP wire layout/payload declaration, donor IOVA range and hostless PCM/link lifecycle. Later #93 probes pass setup; final audibility also requires the ROM HAL setting |
| Legacy ADSP fallback revert `555056679` | The rejected blanket-format experiment stays reverted. The newer successful voice tests supersede the old pending-CVP diagnosis |
| ION `5cff6896e`, VIDC debugfs `defe506a4` | Focused regressions reproduce old failures and pass patched host checks; later recording did not repeat the captured oopses |
| Camera time32 / CPP / JPEG resources | Vendor ABI, power, probe lifetime and resource fixes reached preview and a bound JPEG device; complete camera qualification remains separate |
| Per-core idle and diagnostic cleanup | Runtime residency observed; current source parks periodic ramoops. These are not a quantified battery-life fix |
| sdhci partition-label translation `aa6225801` | On #106 `/dev/block/platform/soc@0/7824900.sdhci/by-name/misc` is labelled `u:object_r:misc_block_device:s0` and the device reaches `sys.boot_completed=1`. This is consistent with the run414 review's correction but is not an A/B boot-control or OTA test |

## Evidence and verification boundaries

The 2026-10-27 capture is one read-only boot on ACK #106
(`def5649d5`, boot `c98735e3-…`), phone on a PC, screen mostly off. It lives in
`artifacts/snapshot60/logs-20261027-fresh-106/` with `SHA256SUMS` and is
summarised in [the findings note](findings/2026-10-27-fresh-state-106.md). It
retests no camera still, torch, call, GNSS or thermal-mitigation path, and its
kernel log covers only 188 s because of the interface-start flood. It scheduled
no suspend: the `dwc3-link` wake source holds the link while the phone is
attached to a host. Absence of a crash in that window is not a functional pass.

The call report commits compact checks/decodes/scripts and indexes larger local
captures. #93 has two successful calls with kernel session-return probes and
complete route snapshots. The later slot-A 4.19 call has user-confirmed two-way
handset/speaker audio plus retained HAL/RIL buffers; its route collector failed
preflight because the expected debugfs component path was absent and produced
no complete active-route capture. No property-off comparison call was made
on that 4.19 boot.

Older camera host checks (42 tests plus the routing test), dpmd host checks and
media regression results retain their recorded scope; this documentation update
does not rerun them or establish patched-device behavior. No kernel build, boot,
flash, new call, controlled panic or phone change was performed for this update.

Large artifacts remain local under `porting/artifacts/`; ignore rules do not
make those files recoverable from Git. Preserve unique captures and real boot
bases. Dated findings remain evidence for their named run; use this queue and
[CURRENT-STATE.md](CURRENT-STATE.md) for current priorities.
