# Jose Virtual Soundcard

A Mac virtual soundcard that sends and receives up to 64 channels of audio with Dante equipment, for use with a DAW or ProPresenter.

It speaks Dante's own protocol, so the Mac shows up in Dante Controller like any other Dante device. That part comes from [Inferno](https://github.com/teodly/inferno), an independent, open-source implementation of the Dante protocol, which this project ports to macOS. It is not made or endorsed by Audinate.

## The plan

1. **Mac shows up in Dante Controller** (this stage): Inferno on macOS, our own Dante clock follower (PTPv1, plus PTPv2), and a test app with meters, test tones, speaker monitoring and recording. → [Testing on your Dante network](docs/testing-with-dante.md)
2. **Virtual soundcard**: a Core Audio driver so the Mac has a 64-in, 64-out audio device for a DAW or ProPresenter.
3. **Settings**: sample rate, latency, channel count and device name (in the app now; carried over to the driver).
4. **Installer and testing**: a signed installer and a full 64-channel test.

## How it works

- **Clock**: Dante devices keep time with PTP. Our follower (`engine/src/ptp.cpp`) locks to PTPv1, Dante's native clock, or to PTPv2. The app's **Clock** setting picks Automatic, PTPv1 or PTPv2. Automatic prefers PTPv1.
- **Dante**: Inferno (`third_party/inferno`, Rust) handles discovery, Dante Controller and audio flows. It reads our clock through a small local socket (`engine/src/clock_server.cpp`). `dante/` is the bridge between Inferno and the C++ engine.
- **Audio**: received and sent audio sit in ring buffers addressed by clock time (`engine/src/dante_device.cpp`), which the app and, later, the driver read and write.

## Layout

| Folder | What's in it |
| --- | --- |
| `engine/` | C++ engine: PTP clock, clock server, Dante device, test tools, and the C interface the app uses (`jvs_c.h`). |
| `dante/` | Rust bridge to Inferno. |
| `third_party/inferno/` | Inferno, with small changes so it builds on macOS. |
| `app/` | The Mac test app (SwiftUI). |
| `tools/jvs-poc/` | Command-line tool and a fake clock for testing. |
| `tests/` | Unit tests and on-one-machine Dante tests. |
| `docs/` | How to test on a Dante network. |

## Download

The newest test build of the app: [JoseVirtualSoundcard.dmg](https://github.com/stefanhoagland/Jose-Virtual-Soundcard/releases/download/latest-test/JoseVirtualSoundcard.dmg).

## Building

GitHub builds everything on every push. To build it yourself you need the Xcode command-line tools, CMake and Rust (`rustup target add aarch64-apple-darwin x86_64-apple-darwin`):

```
cmake -S . -B build
cmake --build build
./build/jvs_tests
./scripts/build-app.sh      # the app and .dmg, in dist/
```

## Testing without Dante gear

`jvs-poc ptp-master` runs a fake network clock so the code can be tried on any network. **Never run it on your Dante network.** `jvs_dante_test` and `jvs_capi_test` use it to run two Dante devices on one machine and route test tones between them.

## License

GPL-3.0-or-later, because Inferno is GPL. See `LICENSE` and `third_party/inferno/`.
