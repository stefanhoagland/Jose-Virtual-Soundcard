# Testing on your Dante network

Version 0.3 adds the sound driver: **Jose Virtual Soundcard** becomes a sound device on the Mac with 64 inputs and 64 outputs, and whatever you play to it goes out on Dante. This guide checks:

1. The device shows up in the Mac's Sound settings.
2. The Mac locks to the Dante clock and shows up in Dante Controller.
3. Sound played on the Mac reaches a Dante device.
4. Sound from Dante reaches the Mac.

It takes about 15 minutes. There is nothing to type in Terminal.

## Before you start

- A Mac wired to the Dante network (the primary network, not Wi‑Fi).
- Dante Controller, ideally on another computer.
- A Dante device to listen on and send from, such as your AVIO adapter.
- On the test Mac, quit Dante Virtual Soundcard and Dante Via if they're running.
- Quit the old Jose Virtual Soundcard app if it's open.

## 1. Install

1. Download [JoseVirtualSoundcard.pkg](https://github.com/stefanhoagland/Jose-Virtual-Soundcard/releases/download/driver-test/JoseVirtualSoundcard.pkg).
2. Double-click it. macOS says it can't check it for malware, because it isn't signed with a paid Apple developer account yet. Click **Done**, open **System Settings › Privacy & Security**, scroll down, and click **Open Anyway** next to JoseVirtualSoundcard.pkg.
3. Follow the installer and enter your Mac password when asked. It installs the app in Applications and the driver for the Mac's sound system.
4. At the end, the Mac's sound stops for a second while the sound system restarts. That's normal.

## 2. Check the Mac sees the device

Open **System Settings › Sound**. Under **Output**, **Jose Virtual Soundcard** should be listed. Don't pick it yet.

## 3. Connect it to Dante

1. Open **Jose Virtual Soundcard** from Applications. The first time, use **Open Anyway** in Privacy & Security again.
2. Pick the **Network port** wired to Dante, leave the rest as it is (48 kHz, 10 ms, Automatic clock, 64 and 64 channels), and click **Start**.
3. Within a few seconds the clock should say **PTPv1 locked** and the dot should turn green: **On the network as "Jose-Virtual-Soundcard"**.
4. In Dante Controller, the device should be in the routing grid with 64 receive and 64 transmit channels.

You can now close the app. The device keeps running in the background, also after a restart.

## 4. Mac to Dante

1. In Dante Controller, route `TX 1` and `TX 2` of `Jose-Virtual-Soundcard` to outputs on your AVIO that you can listen to.
2. In **System Settings › Sound › Output**, choose **Jose Virtual Soundcard**.
3. Play some music on the Mac. You should hear it from the AVIO. Outputs 1 and 2 of the Mac device are left and right.
4. Listen for a minute. Is it clean, or are there clicks or dropouts?
5. Try it from your DAW or ProPresenter too: choose **Jose Virtual Soundcard** as the output device there.

When you're done, set the Mac's output back to your speakers.

## 5. Dante to Mac

1. In Dante Controller, route a Dante source with signal to `RX 1` and `RX 2` of `Jose-Virtual-Soundcard`.
2. In the app, the meters under **Use it on this Mac** should move.
3. In your DAW, choose **Jose Virtual Soundcard** as the input device and record inputs 1 and 2. Listen back for clicks.

## What to send back

Click **Copy test report** in the app and paste it into the project thread. Say whether sound was clean both ways, and which apps you tried.

## If something goes wrong

- **No Jose Virtual Soundcard in Sound settings**: restart the Mac once and look again.
- **The app says the driver isn't installed**: run the installer again.
- **Not in Dante Controller**: check the app says "On the network", and that the network port is the one wired to Dante.
- **Clicks**: in the app, click **Stop**, choose a higher **Latency**, and click **Start**.

## Removing it

Delete **Jose Virtual Soundcard** from Applications. Then, in Finder, choose **Go › Go to Folder**, type `/Library/Audio/Plug-Ins/HAL`, delete **JoseVirtualSoundcard.driver**, and restart the Mac.
