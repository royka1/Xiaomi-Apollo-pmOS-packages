# ModemManager patches for the Apollo SDX55 fusion modem

Two patches are needed to get mobile **data** working on postmarketOS with the
SDX55 attached over PCIe/MHI. Voice, SMS and registration work without them;
only the data bearer is affected.

Both apply to ModemManager commit `d776ea38d29ca472a12323c1d45002ee19a66f57`
(= Alpine `modemmanager-1.25.95_git20260709-r0`).

## Why

The SDX55 is an *external* modem whose **control plane is QRTR** and whose
**data plane is a `mhi_net` netdev**. Nothing upstream expects that pairing:

* `src/kerneldevice/mm-kernel-device-qrtr.c:148` hardcodes `ID_MM_QCOM_SOC=1`
  on every QRTR-discovered device, so a QRTR modem *always* gets the `qcom-soc`
  plugin. There is no udev property that can opt out.
* That plugin overrides `peek_port_qmi_for_data` and only accepts the data
  drivers of SoC-internal modems, `ipa` and `bam-dmux`. A `mhi_net` data port
  fails with *"Unsupported QMI kernel driver for 'net/mhi_hwip0': mhi_net"* —
  even though the generic class in `mm-broadband-modem-qmi.c` handles `mhi_net`
  perfectly well. → **0001**
* With that fixed, MM reaches `WDS Start Network` and the modem answers QMI
  error 70 `InvalidOperation`. This firmware requires the WDS client to be
  bound to a data endpoint first, but MM only binds when multiplexing is in
  use. → **0002**

`0001` is a strict extension (a new `else if`). `0002` widens a condition and
is scoped to `QMI_DATA_ENDPOINT_TYPE_PCIE`, so USB and SoC-internal modems keep
their current behaviour.

## Verifying the bind theory without MM

`wds_test.c` (in the parent directory) binds and starts a call from a single
QMI client. This cannot be done with `qmicli`: over QRTR the client *is* the
socket, so a CID does not survive the process and the second invocation gets
`Unknown client N for service wds`.

```
$ sudo ./wds_test --bind=none                       # InvalidOperation
$ sudo ./wds_test --bind=mux --ep-type=pcie --ep-iface=4 --mux-id=0
  CONNECTED (handle 0x737a5690)
    address: 10.26.143.7
    mtu:     1500
```

Traffic flows on **`mhi_hwip0`** (IP_HW0). `mhi_swip0` gets nothing: endpoint
interface number 4 maps to the hardware channel.

## Building on the phone

`apk`'s solver is wedged on an unrelated `alsa-utils-openrc` / systemd
conflict, so `apk add` of the build deps fails. Work around it by grafting the
`-dev` packages into a private sysroot instead of installing them (see
`~/devfetch.sh` on the device): fetch the apk, untar it under `~/sysroot`,
rewrite `prefix=/usr` in each `.pc`, and drop `Requires.private` lines whose
`.pc` files are absent (we link dynamically, the real `.so` already carries the
right `DT_NEEDED`).

```sh
export PKG_CONFIG_PATH=/home/roy/sysroot/usr/lib/pkgconfig
meson setup output --prefix=/usr --libdir=/usr/lib --sysconfdir=/etc \
  --localstatedir=/var --buildtype=plain -Db_lto=true \
  -Dudevdir=/usr/lib/udev -Dsystemdsystemunitdir=/usr/lib/systemd/system \
  -Ddbus_policy_dir=/usr/share/dbus-1/system.d -Dgtk_doc=false \
  -Dpolkit=permissive -Dsystemd_journal=false -Dsystemd_suspend_resume=true \
  -Dvapi=true
ninja -C output src/ModemManager src/plugins/libmm-plugin-qcom-soc.so
```

`-Dudevdir` is passed explicitly only because `udev.pc` is missing; it affects
install paths only. Everything else matches Alpine's APKBUILD, so the plugin
stays ABI-compatible with the packaged daemon.

Install:

```sh
sudo install -m755 output/src/ModemManager /usr/sbin/ModemManager
sudo install -m755 output/src/plugins/libmm-plugin-qcom-soc.so \
     /usr/lib/ModemManager/libmm-plugin-qcom-soc.so
```

Untouched originals are kept next to them as `*.orig`.

> **`apk upgrade` silently reverts both files.** They are not tracked by any
> package. After upgrading `modemmanager`, rebuild and reinstall, or package
> the patches with `abuild`.

## Also required (not a patch)

`/etc/udev/rules.d/77-mm-ignore-sdx55-efs.rules` keeps MM away from the EFS
port:

```
SUBSYSTEM=="wwan", KERNEL=="wwan0efs0", ENV{ID_MM_PORT_IGNORE}="1"
```

MM AT-probes every wwan port it does not recognise. `wwan0efs0` exists only
because of the out-of-tree `WWAN_PORT_EFS` kernel patch; the modem's
`rmts_srv` remote-storage server receives the AT strings as garbage and the
firmware watchdog bites. The proper fix is to drop that kernel patch — it was
written for the remote-EFS theory, which turned out to be wrong.
