#!/usr/bin/env bash
set -euo pipefail

WORK="${LANTERN_QEMU_WORK:-/tmp/lantern-qemu}"
KERNEL_VERSION="${LANTERN_KERNEL_VERSION:-6.8.0-138-generic}"
DISK_SIZE_MB="${LANTERN_QEMU_DISK_MB:-256}"
GUEST_MEMORY="${LANTERN_QEMU_MEMORY:-1024}"
BOOT_TIMEOUT="${LANTERN_QEMU_TIMEOUT:-900}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

log() { printf '[qemu-env] %s\n' "$*"; }

require_tools() {
  local missing=()
  for tool in qemu-system-x86_64 busybox cpio dpkg-deb; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
  done
  if [ ${#missing[@]} -gt 0 ]; then
    log "Installing: ${missing[*]}"
    if [ "$(id -u)" -ne 0 ]; then
      log "Root privileges are required to install ${missing[*]}"
      exit 3
    fi
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q
    apt-get install -y -q qemu-system-x86 busybox-static cpio dpkg
  fi
}

fetch_kernel() {
  mkdir -p "$WORK/packages"
  if [ ! -f "$WORK/kernel/boot/vmlinuz-$KERNEL_VERSION" ]; then
    log "Downloading kernel $KERNEL_VERSION"
    ( cd "$WORK/packages" && apt-get download \
        "linux-image-$KERNEL_VERSION" "linux-modules-$KERNEL_VERSION" )
    mkdir -p "$WORK/kernel"
    for package in "$WORK"/packages/*.deb; do
      dpkg-deb -x "$package" "$WORK/kernel"
    done
  fi
}

copy_shared_objects() {
  local binary="$1"
  local root="$2"
  ldd "$binary" 2>/dev/null | awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^\//) print $i }' |
    sort -u | while read -r library; do
      mkdir -p "$root$(dirname "$library")"
      cp -Ln "$library" "$root$library" 2>/dev/null || true
    done
}

build_initramfs() {
  local root="$WORK/initramfs"
  rm -rf "$root"
  mkdir -p "$root"/{bin,sbin,proc,sys,dev,tmp,run,lib,lib64,modules,lantern}

  cp /usr/bin/busybox "$root/bin/busybox"
  ( cd "$root/bin" && for applet in $(./busybox --list); do
      [ "$applet" = busybox ] || ln -sf busybox "$applet"
    done )
  ln -sf ../bin/busybox "$root/sbin/poweroff"
  ln -sf ../bin/busybox "$root/sbin/insmod"

  local modules_root="$WORK/kernel/lib/modules/$KERNEL_VERSION/kernel"
  for module in virt/lib/irqbypass.ko drivers/iommu/iommufd/iommufd.ko \
                drivers/vfio/vfio.ko drivers/vfio/vfio_iommu_type1.ko \
                drivers/vfio/pci/vfio-pci-core.ko drivers/vfio/pci/vfio-pci.ko; do
    local source="$modules_root/$module"
    if [ -f "$source.zst" ]; then
      zstd -q -d -f "$source.zst" -o "$root/modules/$(basename "$module")"
    elif [ -f "$source" ]; then
      cp "$source" "$root/modules/$(basename "$module")"
    else
      log "Missing kernel module $module"
      exit 4
    fi
  done

  for binary in runner.exe test.exe; do
    cp "$PROJECT_ROOT/_build/default/test/$binary" "$root/lantern/$binary"
    copy_shared_objects "$root/lantern/$binary" "$root"
  done
  cp "$PROJECT_ROOT/_build/default/bin/tool.exe" "$root/lantern/tool"
  copy_shared_objects "$root/lantern/tool" "$root"
  cp /lib64/ld-linux-x86-64.so.2 "$root/lib64/" 2>/dev/null || true

  cat > "$root/init" <<'INIT'
#!/bin/sh
export PATH=/bin:/sbin:/lantern
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts && mount -t devpts devpts /dev/pts

status=1
echo "[guest] Kernel $(uname -r)"

insmod /modules/irqbypass.ko
insmod /modules/iommufd.ko
insmod /modules/vfio.ko
insmod /modules/vfio_iommu_type1.ko allow_unsafe_interrupts=1
insmod /modules/vfio-pci-core.ko
insmod /modules/vfio-pci.ko

bdf=""
for device in /sys/bus/pci/devices/*; do
  class=$(cat "$device/class")
  if [ "$class" = "0x010802" ]; then
    bdf=$(basename "$device")
    break
  fi
done

if [ -z "$bdf" ]; then
  echo "[guest] No NVMe controller found"
else
  echo "[guest] NVMe controller at $bdf"
  if [ -e "/sys/bus/pci/devices/$bdf/driver" ]; then
    echo "$bdf" > "/sys/bus/pci/devices/$bdf/driver/unbind"
  fi
  echo vfio-pci > "/sys/bus/pci/devices/$bdf/driver_override"
  echo "$bdf" > /sys/bus/pci/drivers_probe
  group=$(basename "$(readlink "/sys/bus/pci/devices/$bdf/iommu_group")")
  echo "[guest] IOMMU group $group, driver $(basename "$(readlink "/sys/bus/pci/devices/$bdf/driver")")"
  ls -l /dev/vfio/

  echo "[guest] Running the C layer selftest against real VFIO"
  if /lantern/runner.exe "$bdf" unused vfio; then
    echo "[guest] Running the OCaml control plane against real VFIO"
    /lantern/tool identify --transport vfio "$bdf" -v &&
    dd if=/dev/urandom of=/tmp/pattern.bin bs=512 count=64 2>/dev/null &&
    /lantern/tool write --transport vfio "$bdf" 4096 64 -i /tmp/pattern.bin &&
    /lantern/tool read --transport vfio "$bdf" 4096 64 -o /tmp/readback.bin &&
    cmp /tmp/pattern.bin /tmp/readback.bin &&
    /lantern/tool bench --transport vfio "$bdf" --size 8m --io-size 4096 --queue-depth 16 &&
    status=0
  fi
fi

if [ "$status" -eq 0 ]; then
  echo "LANTERN_RESULT PASS"
else
  echo "LANTERN_RESULT FAIL"
fi
sync
poweroff -f
INIT
  chmod +x "$root/init"

  ( cd "$root" && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -9 ) \
    > "$WORK/initramfs.cpio.gz"
}

create_disk() {
  if [ ! -f "$WORK/nvme.img" ]; then
    log "Creating a ${DISK_SIZE_MB}M raw disk image"
    dd if=/dev/zero of="$WORK/nvme.img" bs=1M count="$DISK_SIZE_MB" status=none
  fi
}

run_guest() {
  local accel="tcg"
  if [ -e /dev/kvm ] && [ -r /dev/kvm ]; then
    accel="kvm"
  fi
  log "Booting the guest with accelerator $accel"
  set +e
  timeout "$BOOT_TIMEOUT" qemu-system-x86_64 \
    -machine q35,accel="$accel",kernel-irqchip=split \
    -cpu max \
    -smp 2 \
    -m "$GUEST_MEMORY" \
    -device intel-iommu,intremap=on,caching-mode=on \
    -kernel "$WORK/kernel/boot/vmlinuz-$KERNEL_VERSION" \
    -initrd "$WORK/initramfs.cpio.gz" \
    -append "console=ttyS0 intel_iommu=on iommu=pt panic=1 quiet" \
    -drive "file=$WORK/nvme.img,if=none,id=nvme0,format=raw" \
    -device pcie-root-port,id=rp0,bus=pcie.0,slot=1 \
    -device nvme,drive=nvme0,serial=deadbeef,bus=rp0 \
    -nographic -no-reboot 2>&1 | tee "$WORK/console.log"
  set -e
}

require_tools
fetch_kernel
create_disk
build_initramfs
run_guest

if grep -q "LANTERN_RESULT PASS" "$WORK/console.log"; then
  log "Guest reported PASS"
  exit 0
fi
log "Guest reported failure, console log at $WORK/console.log"
exit 1
