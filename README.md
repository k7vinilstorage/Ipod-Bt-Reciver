# BT Receiver: A2DP sink for a jailbroken iPod touch 5

[![Build](https://github.com/k7vinilstorage/Ipod-Bt-Reciver/actions/workflows/build.yml/badge.svg)](https://github.com/k7vinilstorage/Ipod-Bt-Reciver/actions/workflows/build.yml)

**Install on the iPod (Cydia repo):** in Cydia, *Sources → Edit → Add* and enter
`https://k7vinilstorage.github.io/Ipod-Bt-Reciver/`, then install **BT Receiver (A2DP Sink)**.
Or download the latest `.deb` from [Releases](https://github.com/k7vinilstorage/Ipod-Bt-Reciver/releases/latest).

> First-time repo setup (once, by the repo owner): CI pushes the Cydia repo to the
> `gh-pages` branch. Enable it under **Settings → Pages → Source: Deploy from a branch →
> `gh-pages` / `root`**. After a minute the `github.io` URL above serves the repo.

Turns an iPod touch 5 (iOS 6 – 9.3.5, jailbroken) into a Bluetooth audio receiver.
Your phone pairs with it like a speaker, and the music plays through the iPod's
headphone jack, speaker, or dock line-out. The phone's volume slider and track info
(title and artist) come through over AVRCP.

> **Status:** the `.deb` builds (armv7, iOS 6.0+). Every function it uses exists on
> iOS 9.3. It has **not been run on real hardware yet**. See
> [If it doesn't work](#if-it-doesnt-work).

## Why this needs its own Bluetooth stack

iOS's Bluetooth stack (BTServer) can only *send* audio (A2DP source). It has no receiver
(sink) role, and no tweak can add one. So this project replaces the stack while the
receiver is on:

1. **`a2dpsinkd`** (root daemon, `/usr/libexec`) stops Apple's Bluetooth daemons. It
   powers up the Broadcom BCM4334 chip with Apple's own `BlueTool`, then talks to the
   chip directly over the UART (through the same netgraph socket BTServer uses).
2. On top of that it runs [BTstack](https://github.com/bluekitchen/btstack): A2DP sink,
   AVRCP controller/target, SBC decoder.
3. Decoded audio goes out through AudioQueue.
4. When you switch the receiver off, Apple's Bluetooth daemons get started again.

The chip bring-up and UART access come from BTstack's old "BTstack for iOS" Cydia port,
which supported iOS 9 (in 2015, v1.1). That port never had an A2DP sink. Modern BTstack
does, so this project combines the two.

The **BT Receiver** app runs as `mobile`. It only creates or removes the flag file
`/var/mobile/Library/A2DPSink/enabled`. launchd keeps the daemon running while that file
exists, so the app never needs root.

```
app (mobile) ──flag file──▶ launchd ──▶ a2dpsinkd (root)
     ▲                                   │  BlueTool: boot chip
     └──── status file ◀─────────────────┤  BTstack: HCI ▸ L2CAP ▸ AVDTP/A2DP ▸ SBC
                                         └─▶ AudioQueue ▶ headphones / speaker
```

## Build

Every push is built by GitHub Actions (`.github/workflows/build.yml`), using the same
pinned Theos, toolchain and SDK. The `.deb` is available as a workflow artifact.

To publish a release: bump `Version:` in `control`, push, then in the **Actions** tab
pick **Build → Run workflow**, tick **Publish release** and run it. That creates the tag
`v<Version>` and the release with the `.deb`. Pushing a matching `vX.Y.Z` tag also works.

To build locally:

You need [Theos](https://theos.dev) with a toolchain that can still build **armv7**.
The easiest is Linux or WSL, using Theos' own toolchain. On macOS, Xcode 14 and later
cannot build armv7, so you'd need Xcode 13 or older.

```bash
bash -c "$(curl -fsSL https://raw.githubusercontent.com/theos/theos/master/bin/install-theos)"
```

Make sure the **iOS 10.3 SDK** is in `$THEOS/sdks`. It's the last SDK that supports
armv7. The binaries still target iOS 6.0+, so they run on iOS 9. (The 9.3 SDK in that
repository is missing `liblaunch`.)

```bash
curl -L https://github.com/theos/sdks/archive/master.tar.gz | tar -xz -C "$THEOS/sdks" --strip-components=1 sdks-master/iPhoneOS10.3.sdk
```

You also need `rsync` (`sudo apt install rsync`).

Then, inside this folder:

```bash
./get_btstack.sh
```

```bash
make package FINALPACKAGE=1
```

This produces `packages/local.a2dpsink_0.4.4_iphoneos-arm.deb`.

## Install

With OpenSSH installed on the iPod:

```bash
make install THEOS_DEVICE_IP=<ipod-ip-address>
```

Or copy the `.deb` over and run `dpkg -i` on it as root, then `uicache`.

## Use

1. Open **BT Receiver** and switch **Receiver** on. The status changes from
   *Starting…* to *Waiting for a phone* after a few seconds.
2. On your phone, pair with **iPod Receiver**. No PIN is needed; old phones get `0000`.
3. Play music.

While the receiver is on, the iPod's own Bluetooth (Settings) is off, and the iPod is
kept awake. The screen can turn off, but the device won't sleep, so keep it plugged in.

### Options in the app

- **Only While Charging:** the receiver only turns on while the iPod has external power
  (charger, computer or dock). The rest of the time, the iPod's own Bluetooth works
  normally and the iPod can sleep. When power is connected, the receiver turns on within
  a second or two. Five seconds after the cable is removed, it hands Bluetooth back to iOS.
- **Reconnect Automatically** (on by default): when the receiver starts and after the
  connection was lost (out of range, Bluetooth toggled on the phone), the iPod connects
  back to the last phone. It retries every 10 s for 2 minutes, then every 60 s. It doesn't
  reconnect if you disconnected from the phone on purpose; connect from the phone and it
  starts working again. The app shows *Reconnecting to …* while it tries.
- **Auto Play on Connect** (off by default): as soon as the phone connects, the iPod
  sends it a *Play* command over AVRCP, so the music starts by itself. It only does this
  once per connection, and not if the phone is already playing.
- **Lock Screen & Car Controls** (on by default): while connected, the receiver acts like a
  music app (Spotify, YouTube Music, …) on the iPod. It is the system "Now Playing" app
  and publishes the phone's track — title, artist, album, duration and elapsed time — so
  the lock screen shows a progress bar and the head unit can show the track and time. The
  head unit's buttons (play/pause, next/previous, including the ones it sends over the
  USB cable) control the *phone* over Bluetooth instead of the iPod's own Music app.

  iPod-mode head units only send next/previous when they think the current playlist has
  another track; on a single track (what the receiver reported before 0.4.4) they send
  *stop* instead. The receiver now reports a long playlist with the current song in the
  middle, so next/previous come through. A *stop* is never passed on to the phone.

  If the head unit still starts the iPod's **Music app** (some do after a track change or
  when you switch to the USB source), the receiver notices, pauses Music and takes the
  audio back within about a quarter of a second, and then skips (if you had just pressed
  next) or resumes the phone (if it was paused). While the phone is paused the receiver
  keeps a silent stream running for up to 10 minutes, the way a paused music app stays the
  current player. If your head unit keeps restarting Music anyway, removing the songs
  from the iPod's Music library stops it for good.

  This part is isolated from the receiver: if it can't start on your iPod (or crashes), it
  switches itself off, the app shows a warning, and Bluetooth audio keeps working.
- **Name** and **Forget Paired Devices**.

## Audio quality tips

- **Turn Wi-Fi off on the iPod in the car.** The iPod touch 5 uses one Broadcom chip and
  antenna for both Wi-Fi and Bluetooth; Wi-Fi scanning steals air time from the audio link.
- The receiver keeps about **0.3 s of audio buffered** (more than a typical speaker), so a
  short Bluetooth hiccup or iOS's power saving with the screen locked doesn't cause a
  dropout. Pause/play therefore takes effect about 0.3 s later.
- Every 30 s the log has a health line, e.g.
  `audio: ok, last 5 s: 0 dropouts, 0 SBC frames dropped, 0 skips; buffered 330 ms`.
  Lines marked `PROBLEMS` mean real dropouts; send those if audio still breaks up.

## If it doesn't work

The daemon logs to **`/var/log/a2dpsinkd.log`**. To debug by hand over SSH (as root),
switch the receiver off in the app, then run it in the foreground with an HCI packet log:

```bash
/usr/libexec/a2dpsinkd -f -d
```

Press Ctrl+C to stop it. This also hands Bluetooth back to iOS. The packet log is
`/tmp/a2dpsink.pklg`, which Wireshark can open.

Where things are most likely to break, in order:

| Symptom in the log | Likely cause |
|---|---|
| `socket(PF_NETGRAPH) failed` / `ioctl(... com.apple.uart.bluetooth) failed` | Something still holds the UART. Check `ps ax \| grep -i -e blue -e BTServer` |
| `Bluetooth controller did not respond` | BlueTool bring-up or UART speed. Check the `BlueTool >` lines and `transport-speed` |
| Connects but no sound | AudioQueue from a daemon. Look for `AudioQueue…failed` lines |
| Stutters | Raise `OPTIMAL_FRAMES_MIN/MAX` in `daemon/sink.c` |

If iOS Bluetooth stays dead afterwards (for example after a crash), reboot. Or load the
Apple jobs again by hand:

```bash
launchctl load /System/Library/LaunchDaemons/com.apple.BTServer.plist
```

## Files

| Path | What |
|---|---|
| `daemon/main.c` | Startup, launchd and flag-file handling, signals, shutdown |
| `daemon/sink.c` | A2DP sink, AVRCP, SBC decode, clock-drift resampling (from BTstack's `a2dp_sink_demo.c`) |
| `daemon/btstack_control_ipod.c` | Stops/starts Apple's Bluetooth, configures the chip with BlueTool |
| `daemon/hci_transport_h4_ipod.c` | H4 over the netgraph UART socket |
| `daemon/media_controls.m` | Now Playing / lock screen and car-button forwarding (MediaPlayer loaded at runtime) |
| `daemon/btstack_audio_ios.c` | AudioQueue output (lock-free hand-off between threads) |
| `daemon/ios9_compat.h` | `clock_gettime` replacement (it only exists on iOS 10+) |
| `app/` | The BT Receiver settings app |
| `layout/` | LaunchDaemon plist, Debian install scripts |
| `stubs/` | iOS 9.3 library stubs for armv7-only functions missing from the 10.3 SDK (`__divsi3`, `objc_msgSend_stret`, `_Unwind_SjLj_*`, …) |

## License

BTstack and the code derived from it (the iOS chip control and transport, and the sink
logic) are under the [BTstack license](https://github.com/bluekitchen/btstack/blob/master/LICENSE):
free for **non-commercial / personal** use. Keep this project personal, or get a
commercial license from BlueKitchen.
