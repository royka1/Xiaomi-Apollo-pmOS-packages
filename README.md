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

## Three things you have to know

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

## Building an image

pmbootstrap manages its own pmaports checkout, so the way to use this port is to
point it at the fork rather than cloning pmaports by hand:

```sh
# 1. Install pmbootstrap the normal way, then set it up once
pmbootstrap init

# 2. Clone the fork (its main branch carries the apollo port) and point
#    pmbootstrap at it
git clone https://gitlab.postmarketos.org/royka1/pmaports.git ~/pmaports-apollo
pmbootstrap config aports ~/pmaports-apollo

# 3. Re-run init so the device list comes from the fork, and pick
#    Vendor: xiaomi   Device: apollo
pmbootstrap init

# 4. Build and flash
pmbootstrap install
pmbootstrap flasher flash_kernel
pmbootstrap flasher flash_rootfs
```

The fork's `main` is upstream pmaports plus this port, so it still maps to the
`edge` channel and everything else behaves normally. To go back to stock:

```sh
pmbootstrap config aports ~/.local/var/pmbootstrap/cache_git/pmaports
```

The five packages here are already inside that fork, so you do not need this
repository to build an image — it exists so the packages can be used on their
own, and so the firmware blobs have a home. To use them against some other
pmaports checkout, symlink them in (pmbootstrap takes a single aports tree):

```sh
cd <your-pmaports>/device/testing
for p in <this-repo>/*/; do
    [ -f "$p/APKBUILD" ] && ln -sfn "$(realpath "$p")" .
done
```

## ModemManager

Two patches are needed for **mobile data** (not for calls, SMS or
registration):

* `0001-qcom-soc-mhi-net.patch` — let the `qcom-soc` plugin use `mhi_net`
  interfaces as data ports
* `0002-bearer-bind-pcie.patch` — bind the WDS bearer to the PCIe endpoint

They are kept here for reference, but you do **not** have to apply them by
hand: the pmaports fork carries a `temp/modemmanager` aport that builds Alpine's
ModemManager with both applied, at a bumped `pkgrel` so it outranks the
repository build. Drop that aport once the patches land upstream.

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
