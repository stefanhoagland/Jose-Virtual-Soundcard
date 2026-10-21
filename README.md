# Jose Virtual Soundcard

A Mac virtual soundcard that sends and receives up to 64 channels of audio with Dante equipment, for use with a DAW or ProPresenter.

It talks to Dante gear over **AES67**, the open audio-over-IP standard that Dante devices support in AES67 mode. No Audinate license or SDK is needed.

## The plan

1. **Proof of concept** (this stage): a command-line tool, `jvs-poc`, that locks to the Dante network clock and sends and receives AES67 streams. → [Testing on your Dante network](docs/testing-with-dante.md)
2. **Virtual soundcard**: a Core Audio plug-in that shows up as a 64-in, 64-out audio device, built on the engine from step 1.
3. **Settings window**: drop-downs for sample rate, latency, and channel count, plus the list of streams on the network.
4. **Installer and testing**: a signed installer and a full 64-channel test.

## What to know about Dante and AES67

- In AES67 mode, Dante devices run at **48 kHz** and carry up to **8 channels per flow**, so 64 channels is 8 flows in each direction.
- Streams are multicast in `239.69.x.x` by default and are announced with SAP, so they show up in Dante Controller's routing view.
- The network clock is PTPv2 (IEEE 1588-2008). A Dante device in AES67 mode provides it.

## Layout

| Folder | What's in it |
| --- | --- |
| `engine/` | The network audio engine: PTP clock follower, SAP/SDP discovery, RTP send and receive. Shared by the tool now and the driver later. |
| `tools/jvs-poc/` | The proof-of-concept command-line tool. |
| `tests/` | Unit tests and an end-to-end loopback test. |
| `docs/` | How to test on a Dante network. |

## Building

GitHub builds a universal Mac binary (Apple Silicon and Intel) on every push. Download it from the **build** workflow's artifacts.

To build it yourself you need the Xcode command-line tools and CMake:

```
cmake -S . -B build
cmake --build build
./build/jvs_tests
./build/jvs-poc help
```

## Testing without Dante gear

`jvs-poc ptp-master` runs a fake network clock so the tool can be tried on any network. **Never run it on your Dante network.** `tests/loopback.sh` uses it to send 16 channels and check that they come back correctly.
