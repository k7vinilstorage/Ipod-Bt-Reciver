Bluetooth audio receiver (A2DP sink) for a **jailbroken iPod touch 5** (iOS 6 – 9.3.5).

### Install

**Easiest — Cydia repo:** add `https://k7vinilstorage.github.io/Ipod-Bt-Reciver/` as a
Cydia source and install *BT Receiver (A2DP Sink)*.

**Or install the .deb directly:**

1. Download the `.deb` below.
2. Copy it to the iPod and install it as root:
   ```
   dpkg -i local.a2dpsink_*_iphoneos-arm.deb
   ```
   Or open it with **Filza** and tap *Install*.
3. Open **BT Receiver**, switch **Receiver** on, and pair your phone with **iPod Receiver**.

If something doesn't work, the log is at `/var/log/a2dpsinkd.log`. If iOS Bluetooth stays
off after a test, reboot (and re-jailbreak if your jailbreak is semi-untethered).

Built automatically by GitHub Actions from the tagged commit. Uses
[BTstack](https://github.com/bluekitchen/btstack): free for non-commercial use only.
