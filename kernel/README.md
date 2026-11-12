<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# Kernel part of the Titan 2 GNSS support

The kernel side of GNSS is a patch series (`patches/`) against the mainline port of
[titan2-linux](https://github.com/Serlk/titan2-linux), branch `titan2`:

- `drivers/soc/mediatek/mtk-gps-dl.c`: the GNSS data link `/dev/gpsdl0` between mnld and the
  GNSS DSP of the CONNSYS subsystem (GPL port of MediaTek's `connectivity/gps/data_link`);
- its binding `Documentation/devicetree/bindings/soc/mediatek/mediatek,mt6878-gps.yaml`;
- the `/gps` node of the Titan 2 device tree.

This directory:

- `patches/`: the series as `git format-patch` files (3 patches):
  - `0001-dt-bindings-soc-mediatek-Add-the-MT6878-GNSS-data-li.patch`
  - `0002-soc-mediatek-Add-the-MT6878-GNSS-data-link-driver.patch`
  - `0003-arm64-dts-mediatek-mt6878-titan2-Describe-the-GNSS-d.patch`
- `titan2-gnss.config`: the kernel config fragment with these options.

## Building

The kernel code of this part lives only here, as a patch series; titan2-linux itself carries
only the base port. The commands assume `titan2-tools` and this repository are cloned side by
side and run in the root of `titan2-tools`, whose `scripts/build-mainline.sh` builds the kernel
(titan2-docs: `docs/BUILD-LINUX.md`).

**The patches** of this directory go on top of the branch `titan2` of
https://github.com/Serlk/titan2-linux:

    git clone -b titan2 https://github.com/Serlk/titan2-linux linux
    cd linux
    git checkout -b titan2-local 50a22e5003dd4a71f27f241069064799bf2a6a3c
    git am ../../titan2-gnss/kernel/patches/*.patch
    cd ..

The series was made with `git format-patch` on `titan2` commit `50a22e5003dd4a71f27f241069064799bf2a6a3c` (the `git checkout` line
above). A newer `titan2` usually takes it as well (`git checkout -b titan2-local titan2`); if
`git am` stops, use the base commit. The series of titan2-camera, titan2-fingerprint and
titan2-gnss touch different hunks and can be applied one after another on the same tree (tested
in the order camera, fingerprint, gnss).

**The config fragment** `titan2-gnss.config` is merged after the base config
(`configs/mainline-titan2.config` of titan2-tools) by `scripts/build-mainline.sh`:

    SRC=linux FRAGMENTS=../titan2-gnss/kernel/titan2-gnss.config scripts/build-mainline.sh

`FRAGMENTS` takes several files separated by spaces (a later value of a symbol wins, as in the
kernel's `scripts/kconfig/merge_config.sh`), e.g. all three optional parts:

    FRAGMENTS="../titan2-camera/kernel/titan2-camera.config ../titan2-fingerprint/kernel/titan2-fingerprint.config ../titan2-gnss/kernel/titan2-gnss.config" \
        SRC=linux scripts/build-mainline.sh

Without the matching patches the symbols of the fragment have no Kconfig entry and the kernel
build drops them silently.
