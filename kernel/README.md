<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# Kernel part

The kernel side of GNSS is in [titan2-linux](https://github.com/Serlk/titan2-linux), branch
`gnss` (a series on top of the mainline port in branch `titan2`):

- `drivers/soc/mediatek/mtk-gps-dl.c`: the GNSS data link `/dev/gpsdl0` between mnld and the
  GNSS DSP of the CONNSYS subsystem (GPL port of MediaTek's `connectivity/gps/data_link`);
- its binding `Documentation/devicetree/bindings/soc/mediatek/mediatek,mt6878-gps.yaml`;
- the `/gps` node of the Titan 2 device tree.

`titan2-gnss.config` in this directory is the kernel config fragment for these options (on
top of titan2-tools' `configs/mainline-titan2.config`).
