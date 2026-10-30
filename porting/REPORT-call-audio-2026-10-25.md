# Tissot call audio: experiments, kernel corrections and final HAL blocker

Prepared from the 2026-10-23 through 2026-10-25 investigation. This is the
current outcome report; the [chronological findings](findings/2026-10-24-call-audio-cvp-zero-channel-map.md)
and [handoff](HANDOFF-call-audio-2026-10-24.md) retain the earlier evidence.

## Result and scope

**Two-way handset and speaker calls worked in two separate calls on kernel
#93 after enabling the ROM audio HAL's multi-session feature.** The second
call used the same kernel, boot, HAL process and property value, with no
additional restart or configuration change.

The final blocker was a ROM/HAL session-selection configuration problem:
RIL requested VoiceMMode1, VSID `0x11c05000`, while the disabled multi-session
path selected legacy voice, VSID `0x10c01000`. The property
`vendor.audio.feature.multi_voice_session.enable` was absent, and the live
HAL feature flag was zero. Setting it to `true` and restarting
`vendor.audio-hal` changed the live flag to one. HAL logs then selected
`voicemmode1-call`, and a kernel return probe independently recorded
`voc_start_voice_call(session=0x11c05000)` returning zero in both successful
calls. The legacy front end remained idle.

This does **not** mean the original investigation was exclusively a ROM
problem. Earlier kernel-side defects had separately prevented channel-info
acceptance, calibration-memory mapping and PCM startup. Those corrections
remain required in the tested configuration. The HAL property addressed
silence after those setup failures had been removed.

The working property is currently a **temporary runtime override**. No ROM
property file, boot-time module or persistent override was installed. A
reboot clears this nonpersistent setting. Persistence is the remaining
implementation step; no further kernel build was needed for the two
successful runtime tests.

## Failure chain and the evidence that separated its stages

| Stage | Failure before correction | Observed result after correction |
| --- | --- | --- |
| Legacy CVP channel-info packet | `0x1328f` inside set-param `0x1133d` rejected with `ADSP_EBADPARAM`; voice setup aborts | #91 accepts nine channel-info commands across repeated setups after restoring the eight-byte mapping layout |
| Voice calibration shared memory | #91 records 16 MVM map failures with `ADSP_EFAILED`, table IOVA near 4 GiB | #93 records four fresh maps returning handles `0x20`-`0x23`, table and regions inside the donor audio pool |
| Hostless PCM startup | #92 voice start returns zero, but playback pre-start returns `-EPIPE` before trigger | #93 both voice directions pass pre-start and trigger with the scoped no-buffer hook |
| Compiler's in-band payload bounds | FORTIFY field-spanning warning on a 56-byte endpoint payload declared as a zero-length member | Flexible trailing member preserves packet bytes; warning is absent in the #93 startup capture |
| Modem/HAL/kernel session association | HAL starts legacy voice although RIL requests VoiceMMode1; PCM and powered routes exist but calls are silent | Enabled HAL multi-session handling selects the requested session; two-way handset/speaker audio works twice |

The separate stages matter: a successful DSP create, successful PCM trigger
or powered DAPM graph alone was never proof of audible end-to-end audio.

## Experiment ledger

The original captures are under `porting/artifacts/snapshot60/`. The
[artifact index](artifacts/snapshot60/call-audio-final-report-20261025/evidence-index.json)
records all 21 call-audio evidence directories and the report-time hashes
of 524 files. All 407 entries in existing historical checksum manifests
verified. Directories without an original finalized manifest are identified
as such; hashing them now does not manufacture historical completeness.
Selected diagnostic lines, with original filenames and line numbers, are
preserved in [key-evidence.txt](artifacts/snapshot60/call-audio-final-report-20261025/key-evidence.txt).

| Experiment | Test or observation | Outcome and interpretation | Evidence |
| --- | --- | --- | --- |
| Earlier legacy front-end restoration | The initial ROM `voice-call` route refers to missing CSVoice/Voice_Tx controls; PCM 2 was associated with the wrong FE | Restore donor legacy DAIs, controls, routes and board selection. This removed a separate porting gap; it was not proof that calls were audible | Existing commit `c3ec18206f`, full message and recorded run `97afb7703` |
| Original sample-width hypothesis | #88 and #89/run405 send numerical width 16; #90/run406 restores raw ALSA enum 2 | DSP rejects both variants across boot tests. Width conversion is not the solution; it stays reverted | `call-audio-20261024T120357Z/`, `call-audio-20261024T132636Z/` |
| Zero channel map | Calibration supplies one RX/two TX channels with all-zero maps; run405 substitutes a valid front-center map | Rejection persists. The zero map is real but does not explain this failure | `call-audio-20261024T130414Z/` |
| Donor field-value reproduction | Run406 sends bits 2 and map 0 with CVP version 2 | Still rejected. This establishes the field values, not byte identity of the complete packet | `call-audio-20261024T132636Z/` |
| Cross-tree voice-path comparison | Compare Lineage tissot, Lucifer, local 4.19 and AOSP paths | Similar function text missed a macro-expanded wire-size difference. The original packet-content exoneration was withdrawn | Chronological findings, eight-versus-32 section |
| ACDB identity | Compare eight QRD ACDB files and the workspace file with published tissot vendor data | All nine files are byte-identical. No wrong-device ACDB replacement was justified | Findings, reference comparison and SHA table |
| Custom topology decoding | Decode property `0x131a7` in the global ACDB data | `0x1000fffc`/`0x1000fffe` are explicit custom voice graphs with 10/13 modules, not generic placeholders | Findings, topology offsets and module lists |
| Early #90 APR probes | Capture create packets and response prefixes | Confirm V3 create packets, but first filters exclude the desired channel-info packets; 28-byte RX records truncate a 32-byte extended header | Findings, live APR tracing section |
| Changed #90 failure stage | Later attempts reject CVP create before channel-info is sent | Cannot retest the original rejection in that state. Resource exhaustion/leaks were hypotheses, not demonstrated causes | Same APR section and contemporaneous dmesg excerpts |
| Legacy channel-info ABI | Compare actual packed structures with eight and 32 mapping entries | Device data is 16 versus 40 bytes; complete packet 64 versus 88 bytes. Give only this legacy parameter an eight-entry map | Final `cvp-abi.json`; run407 APR records |
| #91/run407 | Build and RAM-boot the approved channel-info correction; test repeated calls | Nine channel-info commands accepted. Calls still fail later; the fix does not by itself produce audio | `call-audio-run407/apr-decoded.json` |
| #91 MVM transaction tracing | Capture map arguments, 24-byte CPU table, full 45-byte request and DSP result | All 16 maps fail. Alignment and table structure are valid; the lost donor IOVA restriction becomes the supported next target | `call-audio-run407-mvm/mvm-decoded.json` |
| Restore donor DMA window | Constrain audio IOVAs to `[0x10000000,0x20000000)` and reject complete out-of-range mappings | #92 no longer shows the earlier map-error sequence; #93 later proves fresh mappings directly | Pool candidate checks; #92/#93 captures |
| #92 PCM return probes | Trace voice start, PCM pre-start and trigger boundaries | Voice returns zero, playback pre-start returns `-32`, and playback never reaches trigger. Locate the empty host-buffer check | `call-audio-pcm-start-20261024T153455Z/trace.txt` and `decoded.json` |
| Hostless voice exemption | Restore the donor exemption using the ACK Android no-buffer hook, scoped to each voice substream | #93 passes both PCM directions. Ordinary empty media playback remains rejected in host tests | No-buffer candidate checks; #93 trace |
| Connected-path review | Audit endpoint payload declaration and all seven voice link settings | Apply flexible-array declaration, POST/POST triggers and immediate power-down behavior. Do not claim trigger ordering caused the earlier pre-start EPIPE | Related-fixes candidate and ABI/link checks |
| #93 first-call startup capture | Trace four fresh MVM mappings, voice return and both PCM directions | Four valid handles, voice return zero and both pre-start/trigger paths succeed, but the user still hears no call audio | `call-audio-run93-20261024T160912Z/decoded.json` |
| Initial route collector | Read active DPCM and five DAPM components | Partial handset data saved; collector then fails on unavailable verbose PCM proc files | `call-audio-active-routes-20261024T161809Z/` |
| Wireless route retry | Add per-widget filename reads | Times out because hundreds of per-widget `cat` processes make collection too slow. Availability-only preflight was insufficient | `call-audio-active-routes-wifi-20261024T162724Z/` |
| Bulk collector and preflight | Read one bulk dump per component and test full snapshot duration | Bulk benchmarks pass. The final 2026-10-24 recorder retains logs but has no active samples or finalized manifest; do not treat it as successful call coverage | `call-audio-route-bulk-preflight-20261024T163036Z/`, `call-audio-active-routes-bulk-20261024T163154Z/` |
| Complete silent-route capture | Save six stable snapshots on #93, including both output routes, pinmux and clock summary | Software routes and powered endpoints exist; both modes remain silent. Detect RIL/kernel session discrepancy | `call-audio-active-routes-20261025T041323Z/analysis.json` |
| Actual loaded HAL analysis | Copy/hash the mapped 32-bit HAL, inspect Thumb code, resolve property and feature-flag addresses | The missing multi-session property defaults false. The disabled path falls back to legacy voice. An earlier live read is interrupted by loss of ADB | `call-audio-hal-session-20261025T043239Z/analysis.json` |
| Fresh live baseline | Reconnect on a new #93 boot; read the property and five HAL feature bytes | Property empty; multi-session flag zero; same HAL hash. All three voice front ends idle | `call-audio-multisession-20261025T160238Z/baseline/state.json` |
| Temporary HAL correction | Set only the multi-session property and restart `vendor.audio-hal` | New HAL PID; multi-session flag becomes one, other four inspected feature flags remain zero; kernel and boot unchanged | Same directory, `enabled/state.json`, `apply.stdout` |
| Successful call 1 | Capture actual VSID, DPCM, DAPM and logs; test handset and speaker | Kernel starts `0x11c05000`, return zero; user confirms two-way audio in both modes | Same directory, `success-analysis.json`, `probe-final.txt:13` |
| Successful call 2 | Repeat without a further restart or configuration change | Same VSID/return, both routes captured, user reports success again | `call-audio-multisession-repeat-20261025T161527Z/repeat-success-analysis.json` |

## Kernel-side changes committed

The following three new commits sit on earlier audio bring-up work.
Existing `c3ec18206f` restored the donor's legacy voice front ends, mixer
controls/routes and board selection, after a captured prepare failure and
missing ROM-requested controls. Existing `fe18981e15` corrected the audio
SID source. They remain in history; neither is reverted because the final
successful HAL setting uses VoiceMMode1. These changes address separate
porting contracts, and legacy support remains available for ROMs using it.

### 1. Correct the legacy CVP wire layout and in-band payload declaration

Commit: `370287534f` — `ASoC: msm: q6voice: restore legacy CVD parameter layouts`.

Files: `sound/soc/msm/caf-include/dsp/q6voice.h` and
`sound/soc/msm/q6voice.c`.

Only `vss_param_vocproc_dev_channel_info_t.channel_mapping` becomes eight
bytes. RX, TX and EC-reference copies use the size of that destination.
The shared 32-entry calibration ABI, endpoint media formats and unrelated
multichannel structures are not globally rewritten. Sample width and the
calibration's zero maps are not changed by this patch.

The resulting device data/parameter/command/packet sizes are
`16/28/44/64`, versus the old `40/52/68/88`. Eight compile-time assertions
using declarations extracted from the current production headers verify
both layouts. [Lineage's tissot DSP header](https://github.com/LineageOS/android_kernel_xiaomi_msm8953/blob/lineage-20/techpack/audio/include/dsp/q6voice.h#L27)
provides the eight-entry comparison. A published Qualcomm firmware source
also documents eight-versus-32 compatibility handling, but that source is
not proof of the exact firmware installed on this phone.

The separate `param_data[0]` to `param_data[]` correction makes the
allocated in-band payload visible to compiler bounds checking. Header size
and payload offset remain 36 bytes. Host tests compare old/new packets for
payload sizes 0, 4, 16, 56, 64 and 4096; the bytes are identical and the
observed 56-byte member bound is represented correctly. This follows the
[kernel guidance on flexible trailing arrays](https://docs.kernel.org/process/deprecated.html#zero-length-and-one-element-arrays).
The #93 startup capture no longer contains the earlier endpoint FORTIFY
warning. This declaration fix was not independently shown to cause silence.

The commit also retains the CVP-version and calibration topology/channel-map
diagnostics used during this investigation. They are diagnostics, not a
fallback to a different topology or DSP API version.

### 2. Restore the donor audio IOVA window

Commit: `2878bedb32` — `ASoC: msm: audio-ion: restore the donor audio IOVA window`.

Files: `sound/soc/msm/msm_audio_ion.c` and
`arch/arm64/boot/dts/qcom/msm8953-xiaomi-tissot-audio.dtsi`.

The board supplies `qcom,iommu-dma-addr-pool = <0x10000000 0x10000000>`.
The allocator validates an optional two-cell, nonzero, page-aligned range
whose end fits in 32 bits, and sets the DMA mask/coherent mask ceiling.
Both shared dma-buf and resource mappings must fit completely within that
range before SID bits are added. Rejection unmaps/detaches/frees the
appropriate resources. Boards without the optional property keep their
existing behavior, and the non-SMMU path retains its bypass.

This is a firmware-compatible IOVA window, not a declaration that physical
RAM must reside in that interval. The [DMA API distinguishes DMA/bus
addresses from CPU physical addresses](https://docs.kernel.org/6.12/core-api/dma-api-howto.html).
The mask supplies an upper limit; explicit whole-range validation also
checks the lower limit and buffer end.

On #91 the table IOVA is `0xfffff000`; on #93 it is `0x1ffff000`.
The SID-tagged table address is correspondingly `0x11ffff000` on #93.
Fresh regions at `0x1fbe0000`, `0x1fbdf000`, `0x1fc00000` and
`0x1fbf0000` all fit the donor pool, and all four map functions return zero
with nonzero DSP handles. The first map request's only in-packet change
from the compared #91 request is byte 23, the high IOVA byte; the
out-of-band table/region addresses also move into the pool.

The existing explicit SID fix in `fe18981e15` remains: audio stream
`0x2401 & 0xf` produces SID 1. The `iommus` cell `0xf` identifies the
context bank on this port and must not replace the stream ID. The pool
fix does not undo SID tagging.

### 3. Restore hostless PCM startup and donor voice-link lifecycle settings

Commit: `67c4758554` — `ASoC: msm: preserve hostless voice PCM startup and link settings`.

Files: `sound/soc/msm/msm-pcm-voice-v2.c` and `sound/soc/msm/msm8952.c`.

Voice samples are exchanged within the DSP rather than filled into a host
PCM playback ring. The CAF donor's no-host-buffer exemption was absent
from the target ALSA path. On #92, voice setup returns success and the
empty host-ring test rejects playback before any trigger operation.

The driver registers `android_vh_snd_pcm_check_no_buffer` for each opened
voice substream and sets the exemption only when the callback's data
pointer exactly matches that substream. It does not dereference unrelated
streams or exempt ordinary media. Registration failure returns before
session state changes. Close unregisters that exact callback/data pair
and synchronizes before teardown, following [tracepoint removal/lifetime
requirements](https://docs.kernel.org/6.12/trace/tracepoints.html).

All seven `msm-pcm-voice` links restore the donor's POST/POST trigger order
and `ignore_pmdown_time=1`: Circuit-Switch Voice, Voice2, VoLTE, VoWLAN,
VoiceMMode1, VoiceMMode2 and QCHAT. These are donor lifecycle restorations;
they are not presented as the branch responsible for the measured EPIPE.
The nonvoice links are unchanged by this delta.

Host tests cover unrelated empty media still returning EPIPE, queued media
succeeding, bad PCM state returning EBADFD, registration failure, all seven
session names, concurrent playback/capture registration, and 109 close/
synchronize cycles including 100 reopens. The #93 hardware trace then
shows both voice substreams passing pre-start and trigger.

## ROM/HAL-side finding and change actually performed

The mapped HAL is the 32-bit ARM library
`/system/vendor/lib/hw/audio.primary.msm8953.so`, copied via its equivalent
`/vendor/lib/hw/` path. Its on-device and local SHA-256 match:

`381abe92aefb18dc16dcbacf52dbed6925bdccf9d743385c1012b68f20f0d755`.

The saved disassembly is decoded as Thumb for the relevant functions.
PC-relative literal resolution identifies the multi-session property string
at ELF virtual address `0x1c120` and the feature flag at `0x79d3b`.
Five consumers resolve to that same flag. The default ARM-mode decoding
was kept as a diagnostic artifact and is not used to interpret Thumb
function bodies or their literal pools.

The actual binary reads the property with default false. With the flag
disabled, its voice-session lookup/start helpers return `-ENOSYS`, and
`voice_start_call()` falls back to legacy usecase 35. `voice_init()` gives
that session VSID `0x10c01000`. A nonlegacy RIL VSID is not translated into
a matching mmode session by that disabled path. Enabled handling
initializes and selects separate mmode sessions. The distinction is
consistent with [AOSP's voice-session/usecase mapping](https://android.googlesource.com/platform/hardware/qcom/audio/+/refs/tags/android-9.0.0_r35/hal/voice_extn/voice_extn.c)
and its [legacy fallback on ENOSYS](https://android.googlesource.com/platform/hardware/qcom/audio/+/refs/tags/android-13.0.0_r67/hal/voice.c);
the loaded binary, not those reference versions, establishes this ROM's
actual gate and default.

The vendor XML already contains both route families. Platform XML assigns
VoiceMMode1 to PCM 34 and VoiceMMode2 to PCM 35. Mixer XML distinguishes
legacy `voice-call`/CSVoice routes from `voicemmode1-call`/VoiceMMode1
routes. Both platform variants match each other byte-for-byte, as do both
mixer variants. No mixer path or PCM-number edit was required for the
successful property test.

The only functional ROM-side runtime changes were:

```sh
adb -s 192.168.1.85:39879 shell setprop vendor.audio.feature.multi_voice_session.enable true
adb -s 192.168.1.85:39879 shell setprop ctl.restart vendor.audio-hal
```

The baseline had HAL PID 743 and flag zero. The restarted HAL had PID
21537 and flag one. `dynamic_ecns`, `incall_music`, `compr_voip` and
`power_mode` remained zero in the inspected five-byte block. The kernel
and boot identity did not change during the experiment:

- Kernel: `6.12.96-4k-gc9d35ed0d67f-dirty #93`.
- Build string: `Sat Oct 24 21:48:58 +0545 2026`.
- Successful-test boot ID: `a98ac416-a276-4ad7-b1e3-efa74f3ca13f`.

No persistent ROM file was edited. The archived `apply_override.py` has
boot/property/idle-state guards and restores the original value on setup
failure. `rollback.py` restores the empty value and restarts the same
service; it was prepared but not run after audio succeeded. These scripts
are historical, guarded experiment records with a recorded endpoint and
boot ID, not universal commands for an unidentified device.

The persistent configuration should supply:

```properties
vendor.audio.feature.multi_voice_session.enable=true
```

before the audio HAL initializes. The appropriate ROM vendor-property
source or a separately approved early-boot override must be chosen when
implementing persistence. Changing kernel VSID constants, remapping every
legacy voice session or rebuilding the kernel is not the demonstrated
solution to this final configuration problem.

## Successful tests and exact proof

| Observation | Call 1 | Call 2 |
| --- | --- | --- |
| Evidence directory | `call-audio-multisession-20261025T160238Z/` | `call-audio-multisession-repeat-20261025T161527Z/` |
| User result | Explicitly confirmed two-way audio in handset and speaker | Reported it worked again after being asked to repeat both modes |
| HAL session selection | `INACTIVE -> ACTIVE vsid:11c05000`, logcat line 56924 | Same, logcat line 46429 |
| HAL usecase | `voicemmode1-call`, line 56925 | Same, line 46430 |
| Kernel probe | `session=0x11c05000 ret=0`, `probe-final.txt:13` | Same at line 13 |
| HAL start / stop result | Zero at logcat 57187 / 65705 | Zero at 46764 / 52335 |
| Validated snapshots | Five, one handset and four speaker | Two, one handset and one speaker |
| Snapshot duration | 0.591-0.934 seconds | 0.641-0.717 seconds |
| Kernel/boot/HAL change between calls | Initial runtime property and HAL restart only | None |
| Trace loss / return-probe misses | Zero / zero | Zero / zero |
| Original manifest entries verified | 146 | 51 |
| Probe cleanup | Own event/instance removed; original wifi instance retained | Same |

VoiceMMode1 playback/capture and their backends are started in every active
snapshot. Playback moves from PRI_MI2S_RX to QUIN_MI2S_RX when speaker is
selected; capture remains TERT_MI2S_TX. The voice FE is mono S16_LE at
8 kHz, RX backend mono 48 kHz and TX backend stereo 48 kHz. Circuit-Switch
Voice and VoiceMMode2 remain inactive.

The pre-override silent #93 captures were on earlier boots. A property-off
call followed by property-on call on this exact successful boot was not
performed. The evidence combines the reproduced earlier failure, the
actual binary behavior, direct live 0-to-1 flag measurement, matching
RIL/HAL/kernel session IDs, and two successful calls. It supports the
working ROM/HAL correction without claiming an exhaustive single-variable
same-boot A/B or that every other audio feature has been validated.

## Rejected explanations and remaining observations

- Width 16 versus raw enum 2 and map zero versus front-center were tested;
  changing those values did not solve the channel-info rejection. The
  retired width regression test is obsolete and is not included in these
  commits.
- The voice topology IDs are custom ACDB graphs, not placeholders. Do not
  replace them merely because they are absent from a built-in constant
  list. All nine compared tissot ACDB/workspace files match the published
  reference artifact.
- Identical C source did not establish an identical packet: the mapping
  macro changed structure sizes. The initial full-packet exoneration was
  withdrawn once the packed ABI was checked.
- Blanket legacy DSP API fallbacks were tried earlier and reverted by
  `5550566795`; playback rejected OPEN_WRITE with EUNSUPPORTED. Do not
  restore that fallback from an opcode alone.
- The observed routing-register EIO has matching absent register callbacks
  in donor/port paths. Faking register success was not proposed or applied.
- Volume STEP EALREADY, AFE "No cal sent" messages and near-teardown ASM
  OPEN_WRITE EUNSUPPORTED also occur during the successful call capture.
  They are not sufficient to explain the previously silent call. This is
  not a claim that those errors are universally harmless or fixed.
- EOS timeouts predate these changes; `f431b1110055` records advancing
  speaker playback with EOS timeouts on two earlier boots. A new media
  regression was not established from those messages.
- Inherited ignored voice/calibration return values and an MVM map timeout
  path that can return zero were reviewed and left outside this change.
  The fresh #93 maps have direct responses/handles, so a masked timeout
  is not used to explain their success.

## Verification, provenance and limits

The [fresh check record](artifacts/snapshot60/call-audio-final-report-20261025/checks.json)
records source hashes and commands. All six files match the previously
checked candidate manifest. The saved host harness functions were also
compared with the current production definitions before execution.

Fresh checks completed without a kernel build:

- Strict checkpatch: zero errors and warnings; two style CHECKs prefer `u8`
  over retained CAF `uint8_t` wire declarations.
- Added-patch-line API scan: no findings. The separate full-file scan
  reports pre-existing vendor pointer-format uses and heuristics for newer
  kernel versions; those are preserved, not hidden or fixed incidentally.
- Whitespace checks and both existing audio bind-contract unit tests pass.
- Audio-pool ASan/UBSan host tests pass, including 10,000 range-oracle
  comparisons, whole-buffer boundaries, optional/malformed properties,
  DMA-mask failure and mapping cleanup.
- Voice no-buffer ASan/UBSan host tests pass, including media isolation,
  registration failure, seven sessions, concurrent streams and close/reopen
  synchronization behavior in the host model.
- Flexible-payload ABI test passes for six payload sizes, with unchanged
  header/offset and packet bytes. Eight channel-info ABI assertions pass.
- Collectors pass syntax checks and full real-phone idle preflights.
  Synthetic tests reject missing sections, absent required widgets/pins,
  bad boot IDs, missing PCM fields and duplicate markers, flag boundary
  transitions as unstable, and exercise normal/failed collection cleanup.
- Both successful captures have matching DPCM boundary states, complete
  required sections, zero stream stderr, zero trace loss/misses, verified
  shutdown and probe cleanup, and verified historical manifests.

No new kernel compilation, Sparse run or DT schema build was requested
for the final commits. Host mocks do not execute real kernel RCU, DMA or
OF APIs. #91 was built/RAM-booted during the investigation; #92/#93 were
built/booted by the user. Runtime build/boot/DT observations are captured,
but an independent hash attestation of the user's exact #93 boot image
against every source file was not captured.

The handoff preserves the build-environment note: `build-kernel.sh` needs
the recorded `RUST_TOOLCHAIN` and `BINDGEN` overrides on this host. A later
requested build should use the normal incremental `out` tree and a real
known-good diagnostic boot base, following `REFERENCE-boot-testing.md`;
reconstructing the ramdisk from capture files is not equivalent. No build
or boot was performed to prepare these final commits.

No persistence/reboot validation, second-SIM call, VoLTE/VoWiFi call,
Bluetooth/headset call or independent post-change media-playback test is
claimed. DAPM reports software power state, sequential snapshots are not
atomic, per-widget debugfs output can be page-size limited, and the clock
summary does not expose these DSP-managed LPASS clocks. Actual two-way
audibility is established by the user reports, not inferred from widgets.

## Evidence retention and current handoff

The three kernel commits above contain the six-file code delta. Git also
records this report, the updated historical/handoff documents,
compact traces/decodes, validation records, source harnesses and relevant
collector scripts. Large raw logcat/dmesg archives, the vendor HAL binary,
boot images and generated host executables remain in the local artifact
folders; their indexed hashes and diagnostic excerpts preserve provenance
without adding those large files to Git.

The report/evidence commit is separate from unrelated build/configuration
work. The audio checks cover the six audio source files and do not validate
later optimization changes. Routine zero-byte stdout/stderr files remain
local rather than tracked; JSON check records preserve their outcomes.
Captured nonempty output retains its original final blank lines; standard
source/script/document whitespace checks pass.

Earlier temporary binaries and comparison downloads did not survive in
`/tmp`; the older call captures in artifacts did survive. A previous
blanket statement that older data was lost would be wrong. All useful
records/scripts must now live in `porting/artifacts/`, not `/tmp`.
Collector failures and the caught wrapper quoting error are retained as
process evidence; no user call was requested until the final recorder had
passed its preflight.

The phone is left with the working temporary property enabled. The two
recorders are stopped, and their probes/instances are removed. Do not
revert the kernel corrections because the final blocker was a HAL
property, and do not claim the ROM is permanently fixed until the property
is persisted and a reboot test has passed.
