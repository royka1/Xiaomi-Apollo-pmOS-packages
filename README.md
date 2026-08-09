# Xiaomi Mi 10T / 10T Pro (apollo) — postmarketOS packages

Everything that is not the kernel, for running postmarketOS on the Xiaomi
Mi 10T / 10T Pro (`xiaomi-apollo`, SM8250 + external SDX55 modem).

The kernel lives separately:
<https://gitlab.postmarketos.org/royka1/linux> branch `apollo-7.1`.

## Packages

| Package | What it is |
| --- | --- |
| `firmware-xiaomi-apollo` | Touchscreen, Adreno 650, ADSP/CDSP/SLPI and SDX55 SBL blobs |
| `alsa-ucm-conf-xiaomi-apollo` | ALSA UCM profile for the `X10T` card (speaker, earpiece, mics, headset) |
| `apollo-modem-tools` | `pm_service_native` and `mdm_helper_native` — QMI/ESOC services the SDX55 expects from the AP |
| `tqftpserv-sdx55` | TFTP/RFS server over QRTR, patched for this modem |
| `apollo-modem-support` | EFS extraction, USIM provisioning, udev rules for ModemManager and fastrpc |

## Two things you have to know

### 1. Put the SIM in physical slot 1

In slot 2 nothing addresses the card by default: the modem never creates a
provisioning session, `qmicli` and ModemManager both target `card-slot-1`, and
the modem skips its boot-time RFS config reads. The SIM then sits at
`Application state: 'detected'` forever and ModemManager reports `sim-missing`.

In slot 1 everything is automatic.

### 2. The modem NV (EFS) is per-device and is NOT in these packages

`efs1/2/3.bin` hold **your IMEI**, your RF calibration and your subscriber
state. They are unique to your phone, so they are not — and must never be —
redistributed.

`apollo-modem-support` installs `apollo-efs-extract`, which copies them out of
your own `mdm1m9kefs1/2/3` partitions on first boot. Because the modem reads NV
very early, **cellular starts working on the second boot** after a fresh
install. Without them the SDX55 boots and then ERRFATALs about 15 s into
mission mode.

## Building

These are aports. pmbootstrap takes a single aports tree, so symlink them into
your pmaports checkout:

```sh
git clone https://github.com/royka1/Xiaomi-Apollo-pmOS-packages
cd <your-pmaports>/device/testing
for p in ../../../Xiaomi-Apollo-pmOS-packages/*/; do
    [ -f "$p/APKBUILD" ] && ln -sfn "$(realpath "$p")" .
done
```

Then `pmbootstrap build firmware-xiaomi-apollo` etc., or just let
`pmbootstrap install` pull them in as dependencies of `device-xiaomi-apollo`.

### 3. Pin the hand-installed packages

`apollo-pin-packages` (from `apollo-modem-support`) writes exact-version
constraints for `linux-postmarketos-qcom-sm8250` and `modemmanager` so
`apk upgrade` cannot replace them with repository builds.

This matters: both share a package name with something in the repos. When the
repo kernel wins, apk deletes `/lib/modules/<ver>/` and boot-deploy fails (the
repo package has no apollo DTB), so the phone boots the old image with an empty
module tree and WiFi, sound and the modem all stop at once.

```sh
apollo-pin-packages           # pin to what is installed now
apollo-pin-packages status    # show pins
apollo-pin-packages unpin     # before installing a newer local build
```

## Still manual: ModemManager

`modemmanager-patches/` holds two patches that are needed for **mobile data**
(they are not required for calls, SMS or registration):

* `0001-qcom-soc-mhi-net.patch` — let the `qcom-soc` plugin use `mhi_net`
  interfaces as data ports
* `0002-bearer-bind-pcie.patch` — bind the WDS bearer to the PCIe endpoint

These are not packaged yet: doing it properly means carrying a full
ModemManager aport, which is a lot of surface to maintain against Alpine's.
Until then, apply them to a ModemManager build by hand. Without them the modem
registers on LTE/5G and SMS works, but no data bearer comes up.

## Status

Working on the `apollo-7.1` kernel: display and touchscreen, sound, WiFi and
Bluetooth, all four remoteprocs (modem/ADSP/CDSP/SLPI), and cellular —
registered on LTE/5G with CS+PS attached.

Not verified: camera, GPS (the LOC service does not answer, and its QMI
timeouts can make ModemManager drop the modem on some boots), fingerprint.

## Licensing

The firmware blobs are proprietary Qualcomm/Xiaomi files redistributed from the
stock firmware for this model. Everything else here is GPL-2.0 or BSD-3-Clause
as marked in the individual APKBUILDs.
