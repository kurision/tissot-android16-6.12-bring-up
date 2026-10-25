# Open components and daily-driver blockers — tissot

Reviewed **2026-10-25**. This is the current work queue, not a chronological
bring-up log. See [CURRENT-STATE.md](CURRENT-STATE.md) for evidence and
[README.md](README.md) for the overview. Dates/builds in each row define its
coverage; the latest call test did not retest every component.

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

## Known failures and missing integration

These are retained failures or confirmed gaps. Their exact persistence on a
newer image must be checked; warnings alone are not proof of a broken device.
The [failure inventory](findings/2026-10-24-live-failure-inventory.md) retains
raw source/line references and provenance, including truncated and stale data.

| Priority | Component | Established failure or gap | Next check |
|---|---|---|---|
| 1 | Unresponsive phone after charger removal | User needed a long Power press. The older retained console ends at 4323.729 s without a captured panic/orderly shutdown; reset slot is stale | Reproduce unplug/screen-off/wake with live identity and logs. Wi-Fi, heat, battery loss and suspend are not established causes |
| 1 | USB attachment / resume / charging fallback | Retained QUSB2 PLL/resume errors and an unattached UDC; rebind restored enumeration in the later investigation | Verify current charger-extcon wiring and donor suspend quirks on an identified image, including repeated cable and sleep cycles. Existing ADB connectivity is not this test |
| 1 | Camera flash / torch | Run401 HAL reports no flash on either camera; no torch LED node; donor PMIC flash resources/camera link are omitted | Review/port the real donor integration, then test both torch and camera flash. No repair is established |
| 1 | Camera still-photo correctness | Last retained #44 rear/front photos hit JPEG RESET32 EFAULT, hybrid fallback and malformed restart streams | Qualify a verified post-fix image with new rear/front photos and warning-free decode. The fix is in source; later call tests did not test stills |
| 2 | Fingerprint HAL | Run401 crash buffer contains a service SIGSEGV at address 0xc0 | Capture a repeat and trace the installed driver/HAL path; no successful fingerprint test or repair is recorded |
| 2 | FastRPC / adsprpcd | Retained repeated listener-open ENOSPC and 25 ms retry loops | Trace the actual operation/allocation and cleanup. ENOSPC is not proof of full storage; earlier ioctl fixes do not qualify listener recovery |
| 2 | Peripheral manager / proxy | Expected SSR discovery interfaces absent; repeated manager/proxy exits | Compare actual donor discovery and lifecycle before adding compatibility behavior; do not fake nodes or use service-stop guards as a fix |
| 2 | Modem recovery / data-service reliability | Retained IPA assertion and remoteproc recovery; the reported crash counter is not ten independently retained events | Retain a complete repeat and correlate firmware/host transactions. Working calls do not qualify all cellular data/IMS services |
| 2 | Audio EOS / low-latency underruns | Earlier deep-buffer playback advances, while EOS close timeouts and underruns remain; related error messages also occur during working calls | Reproduce the specific media/completion failure. Do not reopen the resolved call-silence diagnosis from those messages alone |
| 2 | Wi-Fi resume / wake traffic | Earlier resume/TX warnings and later suspend traffic wake evidence | Qualify the committed multicast/broadcast suspend filter, preserving unicast wake and awake traffic behavior; measure drain separately |
| 2 | Charging-current / input-limit behavior | Retained erratic current readings and an earlier 100 mA input-limit state | Check calibrated current/sign handling and cable enumeration on a verified image before using readings as an energy benchmark |

## Temporary repairs and qualification work

| Component | Actual status | Required completion |
|---|---|---|
| ROM call configuration | `vendor.audio.feature.multi_voice_session.enable=true` plus audio-HAL restart produces working calls. Runtime override remains enabled on the last inspected 4.19 boot | ROM property integration before HAL initialization, then reboot and call verification. No new kernel rebuild is needed for the runtime workaround |
| dpmd / HIDL mapping | Binary-size repair survived two prior temporary service starts (120.647 s / 60.695 s), with hashes/domain checked | Persistent integration and functional data-service coverage. The run401 bind is not assumed active after intervening reboots; suppressing a service is not a repair |
| Diagnostic transport | CAF diag driver was subsequently added in `0286193ba3`; the earlier absent-node inventory predates that change | Verify the actual current node, daemon/HIDL path and functional transport; source presence does not close the service issue |
| Camera JPEG compatibility | `4646f53406c3` fixes six kernel-pointer/usercopy paths; original-code control fails and patched host checks pass | Retain a post-fix hardware reset and correct saved photos. The old claim that every later source image lacks the fix is obsolete |
| Camera orientation / telephoto / zoom | QRD mount-angle and donor EEPROM-voltage corrections are in source; earlier ACK exposed two cameras versus three on donor | Revalidate orientation, third-camera enumeration, zoom and repeated preview/capture handoff on a known image |
| Hardware video encoding | Software AVC selection matches the same ROM on donor 4.19; device QUERYCAP is not a completed encode | ROM/framework codec-registration review and a real hardware-encode test. No new kernel encoder regression follows from software selection alone |
| Storage permissions | One mode-2700 ScreenRecords directory was changed to 2775; a 26,553,766-byte / 673-frame recording then saved | Determine metadata origin and whether other directories are affected. No general migration fix or ROM/kernel attribution is established |
| Battery / thermal / performance | Per-core collapse is observed. Run403 reduced sugov CPU to about 2.5% and parked ramoops at 0.0%, but the exec gap and charging-current concerns remain | Controlled screen-off drain, matched performance tests, sustained CPU/GPU mitigation and current Thermal HAL readiness. Current source disables periodic snapshots; older enabled-snapshot descriptions are historical |
| New build/configuration tuning | AutoFDO profile-use and RCU/configuration changes are in source | Build/image provenance and comparative hardware evidence; no success claim from the audio tests |
| GNSS / haptics / IR / other sensors / notification LED | No fresh functional qualification; historical inventory includes porting gaps | Inspect current implementation and test each function. Keep unknown status distinct from a demonstrated failure |
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

## Evidence and verification boundaries

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
