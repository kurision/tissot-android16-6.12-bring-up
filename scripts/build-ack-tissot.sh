#!/usr/bin/env bash
# Build the ACK android16-6.12 kernel for Xiaomi Mi A1 (tissot) with the device config.
#
# Produces, in $ACK_TREE/out/arch/arm64/boot/:
#   Image.gz          plain compressed kernel
#   msm8953-xiaomi-tissot.dtb
#   Image.gz-dtb      gzip(Image) + raw dtb appended, which is the layout the stock
#                     ROM boot.img uses (CAF 4.19 does the same with
#                     CONFIG_BUILD_ARM64_APPENDED_DTB_IMAGE; 6.x has no such target,
#                     so we cat it here).
#
# After resolving the config it asserts that every driver the boot path needs is
# actually built in. A fragment that silently loses a symbol to olddefconfig is
# the failure mode that wastes a phone boot cycle, so it is caught here instead.
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ack_tree=${ACK_TREE:-$(cd -- "$repo_dir/.." && pwd)/ack-android16-6.12}
fragment=${FRAGMENT:-$repo_dir/scripts/tissot-ack.config}
out_dir=${OUT_DIR:-$ack_tree/out}
llvm_bin=${LLVM_BIN:-/usr/bin}
export PATH="$llvm_bin:$PATH"
export ARCH=arm64 LLVM=1 LLVM_IAS=1
export CC="ccache clang"

if [[ $# != 0 ]]; then
    echo "Usage: $0"
    echo "Overrides: ACK_TREE, FRAGMENT, OUT_DIR, LLVM_BIN, JOBS"
    echo "Rust overrides: RUSTC, HOSTRUSTC, BINDGEN, RUST_LIB_SRC, LIBCLANG_PATH"
    if [[ ${1:-} == --help ]]; then exit 0; fi
    exit 2
fi

[[ -f $ack_tree/Makefile ]] || { echo "No kernel tree at $ack_tree (override with ACK_TREE)" >&2; exit 1; }
[[ -f $fragment ]] || { echo "No config fragment at $fragment" >&2; exit 1; }
for tool in clang ld.lld make python3 dtc cat; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done

cd -- "$ack_tree"

# Keep tool overrides identical during availability, config and compilation.
# Kbuild assigns RUSTC/BINDGEN itself, so environment overrides must be passed
# as make arguments. HOSTRUSTC also builds Rust host utilities.
make_args=(
    O="$out_dir"
    RUSTC="${RUSTC:-rustc}"
    HOSTRUSTC="${HOSTRUSTC:-${RUSTC:-rustc}}"
    BINDGEN="${BINDGEN:-bindgen}"
    # scripts/Makefile.autofdo assigns RUSTFLAGS_AUTOFDO_CLANG with :=, so the
    # environment cannot override it and rustc 1.99 rejects the old
    # -Zdebug-info-for-profiling spelling. Pass it as a make argument.
    ${RUSTFLAGS_AUTOFDO_CLANG+RUSTFLAGS_AUTOFDO_CLANG="$RUSTFLAGS_AUTOFDO_CLANG"}
)

# A compiler alone is not enough: missing bindgen, libclang or rust-src silently
# prunes RUST and ASHMEM from gki_defconfig. Diagnose it before changing .config.
if ! make "${make_args[@]}" rustavailable; then
    echo "FATAL: Rust ashmem build prerequisites are unavailable (see above)." >&2
    echo "Fedora: sudo dnf install rust rust-src bindgen-cli clang-libs" >&2
    echo "rustup: install rust-src for the selected RUSTC toolchain." >&2
    echo "RUST_LIB_SRC must name its matching source library directory." >&2
    echo "LIBCLANG_PATH may be needed for bindgen to find libclang." >&2
    exit 1
fi

# gki_defconfig is the Android baseline; the tissot fragment is merged on top so
# the SoC drivers the board file names actually get compiled in.
make "${make_args[@]}" gki_defconfig
"$ack_tree/scripts/kconfig/merge_config.sh" -m -O "$out_dir" "$out_dir/.config" "$fragment"
make "${make_args[@]}" olddefconfig

# Symbols without which the phone cannot reach a usable Android userspace.
# Every one of these has been observed to matter on this device: storage for
# /system, RPM and its clocks for anything CAF, SCM for the secure-world calls
# remoteproc and the clock driver make, and the configfs FunctionFS pair that
# adbd speaks over USB. Checked against the resolved .config, not the fragment,
# because olddefconfig is what actually decides.
required=(
    CONFIG_STAGING           # contains ACK's native Rust ashmem driver
    CONFIG_SHMEM             # ashmem mappings are backed by shmem
    CONFIG_RUST_IS_AVAILABLE # rustc + bindgen/libclang + matching core source
    CONFIG_RUST              # ASHMEM depends on Rust support in this ACK tree
    CONFIG_ASHMEM            # SystemServer and vendor userspace need /dev/ashmem
    CONFIG_COMPAT            # preserves the legacy 32-bit ashmem ioctl ABI
    CONFIG_COMMON_CLK_QCOM    # gates every symbol in drivers/clk/qcom/Kconfig
    CONFIG_MSM_GCC_8953       # this SoC's Global Clock Controller
    CONFIG_PINCTRL_MSM8953    # every regulator and PMIC GPIO depends on it
    CONFIG_PINCTRL_MSM        # gates every symbol in drivers/pinctrl/qcom/Kconfig.msm
    CONFIG_PSTORE_BLK         # the only log channel that works before init
    CONFIG_MMC_SDHCI_MSM      # eMMC: without it /system is never found
    CONFIG_MMC_BLOCK
    CONFIG_QCOM_SCM           # secure-world calls; clocks and remoteproc need it
    CONFIG_QCOM_SMD_RPM       # RPM over SMD
    CONFIG_QCOM_CLK_SMD_RPM   # RPM clocks
    CONFIG_QCOM_SMEM
    CONFIG_MFD_SPMI_PMIC      # PM8945: all regulators on this board hang off it
    CONFIG_SPMI_MSM_PMIC_ARB
    CONFIG_SERIAL_MSM         # console, and the only long-term debug channel
    CONFIG_USB_CONFIGFS       # adbd's FunctionFS is a configfs gadget
    CONFIG_USB_CONFIGFS_F_FS
    CONFIG_USB_F_FS
    CONFIG_DRM_MSM            # mdss/mdp5/dsi0: without it the screen can never leave splash
    CONFIG_DRM_PANEL_OTM1911  # tissot's preferred-primary panel (downstream dsi-pref-prim-pan)
    CONFIG_SECURITY_SELINUX          # enforcing mode is proven not to block Android init
    CONFIG_SECURITY_SELINUX_BOOTPARAM # selinux=0 on the cmdline; without it the token is inert
    CONFIG_RFKILL             # gki baseline is =m, and CFG80211's
                              # "depends on RFKILL || !RFKILL" then evaluates
                              # to m, capping CFG80211/MAC80211/WCN36XX
    CONFIG_WLAN_VENDOR_ATH    # n hides the whole "if WLAN_VENDOR_ATH" block
                              # in ath/Kconfig, so WCN36XX is not even visible
    CONFIG_CFG80211           # snapshot52 shipped =m and dropped WCN36XX
    CONFIG_MAC80211           # silently: wireless must be asserted like the rest
    CONFIG_WCN36XX            # WCN3680 WLAN (wcn36xx); deps MAC80211/HAS_DMA
    CONFIG_QCOM_WCNSS_PIL     # pronto remoteproc; blobs from modem harvest
    CONFIG_QCOM_SYSMON        # SSR crash detection; silent FW deaths visible
    CONFIG_FINGERPRINT_FPC1020    # fpc HAL: sysfs legacy-path device + fpctzapp TA
    CONFIG_FINGERPRINT_GOODIX_GF3208 # GF HAL: /dev/goodix_fp + netlink + TA
)

missing=()
for sym in "${required[@]}"; do
    grep -qx "${sym}=y" "$out_dir/.config" || missing+=("$sym")
done
if (( ${#missing[@]} )); then
    echo "FATAL: resolved .config is missing required built-in symbols:" >&2
    printf '  %s\n' "${missing[@]}" >&2
    echo "Check them against $fragment for this tree before booting." >&2
    exit 1
fi

make "${make_args[@]}" -j"${JOBS:-$(nproc)}" Image.gz
make "${make_args[@]}" qcom/msm8953-xiaomi-tissot.dtb

boot=$out_dir/arch/arm64/boot
cat "$boot/Image.gz" "$boot/dts/qcom/msm8953-xiaomi-tissot.dtb" > "$boot/Image.gz-dtb"

echo
echo "built:"
for f in "$boot/Image.gz" "$boot/dts/qcom/msm8953-xiaomi-tissot.dtb" "$boot/Image.gz-dtb"; do
    printf '  %-58s %s bytes\n' "${f#$out_dir/}" "$(stat -c %s "$f")"
done
gunzip -c "$boot/Image.gz" | strings -a | grep -m1 'Linux version' || true
echo
echo "driver coverage of the built dtb:"
"$repo_dir/scripts/ack-driver-audit.py" --dtb "$boot/dts/qcom/msm8953-xiaomi-tissot.dtb" \
    --tree "$ack_tree" --config "$out_dir/.config" | tail -3