# Testing on your Dante network

This checks three things with your real Dante gear:

1. The Mac locks to the Dante clock.
2. The Mac hears audio from a Dante device.
3. A Dante device hears audio from the Mac.

It takes about 15 minutes, all in the **Jose Virtual Soundcard** app.

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

## 2. Install the app

1. Download `JoseVirtualSoundcard-0.1.0.dmg` from the link in the project thread.
2. Open it and drag **Jose Virtual Soundcard** onto **Applications**.
3. Open the app from Applications. The first time, macOS says it can't check the app for malware, because it isn't signed with a paid Apple developer account yet. Click **Done**, open **System Settings › Privacy & Security**, scroll down, and click **Open Anyway** next to Jose Virtual Soundcard. You only do this once.
4. When macOS asks whether the app may find devices on your local network, click **Allow**.

## 3. Check the clock

1. Under **Network**, pick the port wired to Dante. It shows its IP address, which should be on your Dante network. Click **Start**.
2. Under **Clock**, the dot should turn green and say **Locked** within a few seconds. Jitter should stay under about 100 µs.

If it stays on "Waiting for a PTP clock", AES67 mode isn't active yet (step 1), or you picked the wrong port.

## 4. Mac receives from Dante

1. In Dante Controller's Device View for the test device, open the **Transmit** tab and click the button to create a multicast flow.
2. Tick **AES67 Flow**, pick up to 8 channels, and click **Create**.
3. In the app, the flow should appear under **Receive from Dante** within about 30 seconds. Choose it.
4. Leave **Play on Mac speakers** on (pick which channels go left and right), and tick **Record** if you want a file. Click **Start listening**.

You should hear the audio and see a meter for each channel. The recording goes to **Music › Jose Virtual Soundcard**. If you hear clicks, stop, pick a higher latency, and start again.

## 5. Dante receives from the Mac

1. Under **Send to Dante**, keep the name `JVS Test`, choose how many channels, and click **Start sending**. Channel 1 is a 300 Hz tone, channel 2 is 400 Hz, and so on.
2. In Dante Controller's **Routing** view, `JVS Test` should appear as a transmitter within a minute. With more than 8 channels it shows up as `JVS Test 1`, `JVS Test 2`, and so on, 8 channels each.
3. Route its channels to the test device and listen.

## What to send back

Click **Copy test report** at the bottom of the app while listening and sending, then paste it into the project thread. Also say whether the audio sounded clean both ways.

## If something goes wrong

- **"Couldn't open the clock ports"**: another program on the Mac is using them. Quit Dante Virtual Soundcard, Dante Via, or Dante Controller on that Mac and click Start again. Dante Controller can run on another computer.
- **Dante doesn't show `JVS Test`**: check that AES67 mode is enabled and the device was rebooted. Dante only lists AES67 flows in its AES67 multicast range. The app sends on `239.69.200.x`, which matches Dante's default.

## Command-line tool

The same tests can be run with the `jvs-poc` command-line tool (download **jvs-poc-macos** from the build page). Run `./jvs-poc help` to see the commands.
