# Testing step 1 on your Dante network

This checks three things with your real Dante gear:

1. The Mac locks to the Dante clock.
2. The Mac hears audio from a Dante device.
3. A Dante device hears audio from the Mac.

It takes about 20 minutes. You'll type a few commands into Terminal; each one is given in full.

## Before you start

- A Mac wired to the Dante network (the primary network, not Wi‑Fi).
- Dante Controller, on that Mac or another computer.
- One Dante device you can use for the test: a stage box, an AVIO adapter, or a console card. It needs to support AES67 mode. Most current Dante hardware does; Dante Virtual Soundcard and Dante Via don't.
- On the test Mac, quit Dante Virtual Soundcard and Dante Via if they're running, because they use the same network ports.

## 1. Turn on AES67 mode on the test device

1. In Dante Controller, double-click the device to open **Device View**.
2. Open the **AES67 Config** tab and set **AES67 Mode** to **Enabled**.
3. Reboot the device when Dante Controller asks you to.

The device keeps working as a normal Dante device. AES67 mode adds the open protocol on top.

## 2. Get the test program

1. Open the pull request for step 1, click the **Checks** tab, then **build**, and download **jvs-poc-macos** at the bottom of the summary page.
2. Double-click the downloaded zip, then double-click `jvs-poc-macos.tar.gz` inside it. You now have a file called `jvs-poc` in Downloads.
3. Open **Terminal** and paste these lines. The first one tells macOS the file is OK to run:

```
cd ~/Downloads
xattr -d com.apple.quarantine jvs-poc
./jvs-poc interfaces
```

The last command lists the Mac's network ports. Find the one with an address on your Dante network (often `en0`, or `en5` and up for USB/Thunderbolt adapters). Use it wherever the commands below say `en0`.

If macOS asks whether Terminal may find devices on your local network, click **Allow**.

## 3. Check the clock

```
./jvs-poc clock -i en0 --seconds 20
```

Within a few seconds it should say `LOCKED`, followed by the grandmaster ID of your Dante clock leader. The `jitter` figure should stay under about 100 us.

If it keeps saying `waiting for a PTPv2 clock`, AES67 mode isn't active yet (step 1), or you picked the wrong port.

## 4. Mac receives from Dante

1. In Dante Controller's Device View for the test device, open the **Transmit** tab and click the **Create Multicast Flow** button (the icon with the + sign).
2. Tick **AES67 Flow**, pick up to 8 channels, and click **Create**.
3. On the Mac:

```
./jvs-poc discover -i en0
```

Wait up to 35 seconds. The flow should be listed with a number in brackets, like `[0]`. Then play some audio into those channels on the device and run:

```
./jvs-poc receive -i en0 --stream 0 --seconds 30 --play --wav dante-test.wav
```

You should hear channels 1 and 2 on the Mac's speakers. Every second it prints the level of each channel. When it finishes, `dante-test.wav` in Downloads has all the channels. Use `--play-ch 3,4` to listen to other channels.

## 5. Dante receives from the Mac

```
./jvs-poc send -i en0 --name "JVS Test" --seconds 300
```

This sends 8 channels of test tones. Channel 1 is 300 Hz, channel 2 is 400 Hz, and so on.

In Dante Controller's **Routing** view, `JVS Test` should appear as a transmitter within a minute. Route its channels to the test device's receive channels and listen.

To try all 64 channels, add `--flows 8`. That sends 8 flows of 8 channels, named `JVS Test 1` to `JVS Test 8`.

## What to send back

Copy the last few lines Terminal printed for each test and paste them in the project thread. The most useful parts are:

- For `clock`: whether it said `LOCKED`, and the `jitter` number.
- For `receive`: the `lost`, `missing` and `arrival … max` figures, and whether the audio sounded clean.
- For `send`: the `late` and `resyncs` figures, and whether Dante heard clean tones.

## If something goes wrong

- **`cannot bind UDP ports 319/320`**: another program on the Mac is using the clock ports. Quit Dante Virtual Soundcard, Dante Via, or Dante Controller on that Mac and try again (Dante Controller can run on another computer).
- **Clicks or gaps when receiving**: raise the latency, e.g. `--latency-ms 10`. The program suggests a number when packets arrive late.
- **Dante doesn't show `JVS Test`**: check that AES67 mode is enabled and the device was rebooted. Dante only shows AES67 flows whose address is in its AES67 multicast range, so the default `239.69.200.1` should match. If you changed the prefix in AES67 Config, use `--addr` with an address in that range.
