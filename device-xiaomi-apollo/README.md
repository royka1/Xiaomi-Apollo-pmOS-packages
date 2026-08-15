# Xiaomi Mi 10T / 10T Pro (apollo)

Modem, mobile data, SMS, voice calls, speaker and earpiece audio, and the
sensor DSP all work. Everything needed for that is a dependency of
`device-xiaomi-apollo`, so a plain `pmbootstrap install` is enough -- there is
nothing to copy onto the phone by hand afterwards.

## Building from a clean machine

The device needs this pmaports fork, not upstream pmaports: the kernel, the
firmware packages and the patched ModemManager only exist here.

**Run these one at a time.** `pmbootstrap init` is an interactive wizard: paste a
block of commands into a terminal where it is already running and it takes each
following line as an answer to a prompt, quite happily creating a work directory
named `git clone https:/...` and cloning upstream pmaports into it. The symptom
is that `apollo` is then not among the available devices.

First clone the fork, and let it finish:

    git clone https://gitlab.postmarketos.org/royka1/pmaports.git ~/pmaports-apollo

**Then give it a remote pointing at official pmaports, and fetch that remote:**

    git -C ~/pmaports-apollo remote add upstream \
        https://gitlab.postmarketos.org/postmarketOS/pmaports.git
    git -C ~/pmaports-apollo fetch upstream

Without the remote, pmbootstrap fails immediately with

    pmaports: could not find remote name for any URL
    '['https://gitlab.postmarketos.org/postmarketOS/pmaports.git', ...]'

It identifies which remote is upstream by matching remote URLs against the
official address, and a clone of a fork has no remote that matches.

Fetch the whole remote, not one branch. pmbootstrap reads the channel
definitions with `git show <remote>/<branch>:channels.cfg` -- from *upstream*,
never from this fork -- and which branch that is depends on the pmbootstrap
version: 3.9 reads `master`, 3.11 reads `main`. Fetching everything satisfies
both. Miss it and the error moves on to

    Failed to read channels.cfg from 'upstream/main' branch of your local
    pmaports clone

The remote is only consulted for channels and update checks; the tree still
builds from the fork's `main`, which is the branch `channels.cfg` maps the
`edge` channel to.

`pmbootstrap pull` will refuse this checkout -- "is tracking unexpected remote
branch 'origin/main' instead of 'upstream/main'". That is correct and harmless:
it is declining to fast-forward the fork onto upstream. Update with

    git -C ~/pmaports-apollo pull origin main

Then start the wizard on its own and answer its questions:

    pmbootstrap init

| prompt | answer |
| --- | --- |
| Work path | accept the default |
| **pmaports path** | `~/pmaports-apollo` -- the clone above, *not* the default |
| Channel | `edge` |
| Vendor | `xiaomi` |
| Codename | `apollo` |
| UI | `phosh` |
| systemd | accept the default |

Finally, as its own command:

    pmbootstrap install

The pmaports answer is the one that matters, and answering it here is enough --
`pmbootstrap config aports ~/pmaports-apollo` does the same thing if the wizard
has already been through. Left pointing at pmbootstrap's own checkout the
install fails outright: that tree's `postmarketos-base` recommends
`doas-sudo-shim`, which conflicts with `sudo-rs`.

**Do not answer `never` to systemd.** `apollo-modem-support`, `apollo-modem-tools`
and `tqftpserv-sdx55` ship their services as systemd units only, so on OpenRC
the EFS extraction, the UIM provisioning and the RFS server never run and the
modem is dead with nothing in the logs to say why. systemd is the default for
phosh, and the device package has no way to enforce it -- pmbootstrap takes that
setting from the UI package alone.

Then, with the phone in fastboot (volume-down):

    pmbootstrap flasher flash_kernel
    pmbootstrap flasher flash_rootfs --partition userdata

The rootfs goes to `userdata` rather than a partition of its own, because it is
a single disk image carrying its own partition table -- `p1` is `/boot`, `p2` is
`/`, written with 4096-byte logical sectors. By hand that is

    fastboot flash boot     ~/.local/var/pmbootstrap/chroot_rootfs_xiaomi-apollo/boot/boot.img
    fastboot flash userdata ~/.local/var/pmbootstrap/chroot_native/home/pmos/rootfs/xiaomi-apollo.img

and the image is an Android sparse image, so `losetup` and `fdisk` see nothing
in it until it is unsparsed. The nested layout is also why Android recovery
cannot back this install up -- it sees `userdata` as a blob and offers to
format it.

## The kernel

`linux-postmarketos-qcom-sm8250` builds from a tag of
<https://gitlab.postmarketos.org/royka1/linux>, currently `apollo-7.1.0-r3`
off the `apollo-7.1` branch. A tag rather than the branch so the archive keeps
a stable checksum.

**After pushing kernel changes, move the tag on and update the aport**, or a
clean build silently produces a kernel without them:

    git tag apollo-7.1.0-r4 apollo-7.1 && git push origin apollo-7.1.0-r4
    # then in this aport: bump _tag and pkgrel, and replace the sha512 of
    # linux-postmarketos-qcom-sm8250-apollo-7.1.0-r4.tar.gz

Note that a locally built kernel from `pmbootstrap build --src` is versioned
`7.1.0_p<timestamp>`, which apk ranks *above* `7.1.0-rN`. Such a package left in
`~/.local/var/pmbootstrap/packages` shadows the aport build on that machine
forever. Clear them out before building an image meant for anyone else.

## First boot

- **Put the SIM in physical slot 1.** In slot 2 nothing addresses the card: no
  provisioning session, PIN verify returns Internal, and the modem skips its
  RFS configuration reads.
- `apollo-efs-extract` copies the modem's per-device NV (`efs1/2/3.bin`, which
  carry the IMEI and RF calibration) out of the phone's own partitions on first
  boot. It is deliberately not in any package. Without it the modem crashes
  about fifteen seconds into mission mode.
- `apollo-pin-packages` pins the kernel, ModemManager and the device packages,
  so a later `apk upgrade` cannot replace them with upstream builds.

## Packages

| package | why |
| --- | --- |
| `firmware-xiaomi-apollo` | ADSP/CDSP/SLPI/GPU/touch blobs and the pd-mapper JSON |
| `firmware-xiaomi-apollo-modem` | `/lib/firmware/sdx55m` -- where Sahara looks for the X55 |
| `firmware-xiaomi-apollo-dsp` | IPA microcontroller and SLPI calibration |
| `firmware-xiaomi-apollo-audio` | voice calibration tables; without them a call is silent |
| `firmware-xiaomi-apollo-sensors` | the registry HexagonFS serves to the sensor DSP |
| `apollo-modem-support` | EFS extraction, UIM provisioning, package pinning, udev |
| `apollo-modem-tools` | pm-service, mdm-helper, EFS sync, DIAG and ADSP F3 readers |
| `tqftpserv-sdx55` | RFS server on QRTR instance 3; instance 1 makes the modem drop the SIM |
| `alsa-ucm-conf-xiaomi-apollo` | HiFi and VoiceCall verbs, including the earpiece route |
| `hexagonrpcd-apollo` | sensor DSP service definitions |
| `modemmanager` (in `temp/`) | carries the mhi_net and PCIe-bearer patches this modem needs |
