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
llvm_bin=${LLVM_BIN:-$HOME/Android/Sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/linux-x86_64/bin}
export PATH="$llvm_bin:$PATH"
export ARCH=arm64 LLVM=1 LLVM_IAS=1

if [[ $# != 0 ]]; then
    echo "Usage: $0"
    echo "Overrides: ACK_TREE, FRAGMENT, OUT_DIR, LLVM_BIN, JOBS"
    if [[ ${1:-} == --help ]]; then exit 0; fi
    exit 2
fi

[[ -f $ack_tree/Makefile ]] || { echo "No kernel tree at $ack_tree (override with ACK_TREE)" >&2; exit 1; }
[[ -f $fragment ]] || { echo "No config fragment at $fragment" >&2; exit 1; }
for tool in clang ld.lld make python3 dtc cat; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done

cd -- "$ack_tree"

# gki_defconfig is the Android baseline; the tissot fragment is merged on top so
# the SoC drivers the board file names actually get compiled in.
make O="$out_dir" gki_defconfig
"$ack_tree/scripts/kconfig/merge_config.sh" -m -O "$out_dir" "$out_dir/.config" "$fragment"
make O="$out_dir" olddefconfig

# Symbols without which the phone cannot reach a usable Android userspace.
# Every one of these has been observed to matter on this device: storage for
# /system, RPM and its clocks for anything CAF, SCM for the secure-world calls
# remoteproc and the clock driver make, and the configfs FunctionFS pair that
# adbd speaks over USB. Checked against the resolved .config, not the fragment,
# because olddefconfig is what actually decides.
required=(
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

make O="$out_dir" -j"${JOBS:-$(nproc)}" Image.gz
make O="$out_dir" qcom/msm8953-xiaomi-tissot.dtb

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