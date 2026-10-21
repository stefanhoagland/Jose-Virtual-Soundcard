# Testing on your Dante network

This checks four things with your real Dante gear:

1. The Mac locks to the Dante clock.
2. The Mac shows up in Dante Controller as a Dante device.
3. A Dante device hears audio from the Mac.
4. The Mac hears audio from a Dante device.

It takes about 15 minutes, all in the **Jose Virtual Soundcard** app. There is nothing to type in Terminal.

## Before you start

- A Mac wired to the Dante network (the primary network, not Wi‑Fi).
- Dante Controller. **Run it on a different computer if you can**, because it uses some of the same network ports. If this Mac is your only computer, try it on the same Mac and tell us what happens.
- Any Dante device to route to and from: a stage box, an AVIO adapter, a console card. No AES67 setup is needed.
- On the test Mac, quit Dante Virtual Soundcard and Dante Via if they're running, because they use the same network ports.

## 1. Install the app

1. Download [JoseVirtualSoundcard.dmg](https://github.com/stefanhoagland/Jose-Virtual-Soundcard/releases/download/latest-test/JoseVirtualSoundcard.dmg).
2. Open it and drag **Jose Virtual Soundcard** onto **Applications** (replace the older copy if asked).
3. Open the app from Applications. The first time, macOS says it can't check the app for malware, because it isn't signed with a paid Apple developer account yet. Click **Done**, open **System Settings › Privacy & Security**, scroll down, and click **Open Anyway** next to Jose Virtual Soundcard. You do this once per new download.
4. When macOS asks whether the app may find devices on your local network, click **Allow**.

## 2. Start the device

1. Under **Dante device**, leave the name as `Jose-Virtual-Soundcard` (or type your own; spaces become dashes).
2. Pick the **Network port** wired to Dante. It shows its IP address, which should be on your Dante network.
3. Leave the rest as it is for the first test: **48 kHz**, **10 ms** latency, clock **Automatic**, **64** receive and **64** send channels.
4. Click **Start**.

Under **Status**, within a few seconds:

- **Clock** should say **PTPv1 locked**. Jitter should stay under about 100 µs.
- The dot should turn green and say **On the network as "Jose-Virtual-Soundcard"**.

If the clock stays on "Looking for a clock leader", you probably picked the wrong port. If it only finds PTPv2, that's fine too; tell us.

## 3. Find it in Dante Controller

1. Open Dante Controller. Within about 30 seconds, `Jose-Virtual-Soundcard` should appear in the **Routing** grid, with 64 receive and 64 transmit channels.
2. Open **Device Info** and **Clock Status** and take a screenshot of each for us.

## 4. Dante hears the Mac

1. In the app, under **Send to Dante**, turn on **Send test tones on every channel**. Channel 1 plays 300 Hz, channel 2 plays 400 Hz, and so on.
2. In Dante Controller, route `TX 1` (and `TX 2`) of `Jose-Virtual-Soundcard` to an output on your Dante device you can listen to.

You should hear a steady, clean tone.

## 5. The Mac hears Dante

1. In Dante Controller, route a Dante source (a mic or line input with signal) to `RX 1` and `RX 2` of `Jose-Virtual-Soundcard`.
2. In the app, the meters under **Receive from Dante** should move for those channels.
3. Turn on **Play on Mac speakers** to listen (pick which channels go left and right).
4. Click **Record all channels** to save a WAV file to **Music › Jose Virtual Soundcard**. Click **Stop recording** after 30 seconds.

If you hear clicks, click **Stop**, pick a higher **Latency**, and **Start** again. Set the same latency on the Dante device's receive side in Dante Controller.

## What to send back

Click **Copy test report** at the bottom of the app while tones are playing and audio is coming in, then paste it into the project thread. Add the two Dante Controller screenshots, and say whether the audio sounded clean both ways.

## If something goes wrong

- **"Couldn't open the clock ports"**: another program on the Mac is using them. Quit Dante Virtual Soundcard, Dante Via, or Dante Controller on that Mac and click **Start** again.
- **Not in Dante Controller**: check that the app says "On the network", that Dante Controller is on the same network, and that you clicked **Allow** for local network access (System Settings › Privacy & Security › Local Network).
- **Clock says PTPv2 but nothing routes**: set **Clock** to **PTPv1 (Dante)** and start again.

## Command-line tool

The same device can be run from Terminal with `jvs-poc device -i en0 --tones` (download **jvs-poc-macos** from the build page). Run `./jvs-poc help` to see the commands.
