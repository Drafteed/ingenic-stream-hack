# Ingenic T31X Based Camera Local Stream Hack

`LD_PRELOAD` hook for Tuya-like cameras based on Ingenic T31X.
It intercepts `IMP_Encoder_GetStream()` from `libimp.so` and mirrors encoded video to a local TCP socket, while keeping the original fw process running normally.

This allows RTSP/WebRTC/HLS/MP4 streaming on your local network via [go2rtc](https://github.com/AlexxIT/go2rtc) without Tuya cloud dependency, while preserving its original functionality.

Based on: https://github.com/FiveLeavesLeft/WyzeCameraLiveStream

## How it works

- Original app still calls Ingenic encoder APIs as usual.
- Hooked `IMP_Encoder_GetStream()` forwards the call to the real `libimp.so`.
- On success, encoded packs are copied to one TCP client (`0.0.0.0:12345` by default).
- If needed, the hook requests an IDR frame (`IMP_Encoder_RequestIDR`) when a client connects or when backpressure is detected.
- Stream starts only after a keyframe to avoid decoder startup issues.
- Pack payload is normalized for decoder compatibility:
  - Annex-B packets are sent as-is.
  - AVCC-length packets are converted to Annex-B (`00 00 00 01` prefixes).

## Run on camera

**ROOT access to the camera is required.**

Download `libimp.so` from [releases page](https://github.com/Drafteed/ingenic-stream-hack/releases), copy to the camera (e.g., to the SD card) and launch the main fw app with `LD_PRELOAD` set accordingly.

Example:

```bash
LD_PRELOAD=/mnt/extsd/lib/libimp.so /tmp/jsapp/baby_monitor &
```

With logging enabled:

```bash
STREAM_HACK_LOG=/mnt/extsd/stream_hack.log LD_PRELOAD=/mnt/extsd/lib/libimp.so /tmp/jsapp/baby_monitor &
```

## Connect from go2rtc

An external [go2rtc](https://github.com/AlexxIT/go2rtc) instance is required to display the stream via RTSP, WebRTC, MP4, HLS, etc.

`go2rtc` config example:

```yaml
streams:
  my_camera_local: tcp://192.168.1.25:12345/
```

Replace `192.168.1.25` with your camera IP.

## Environment variables

- `STREAM_HACK_LOG`
  - `0` or unset: logging disabled
  - `1`: log to default path (`/mnt/extsd/stream_hack.log`)
  - any other non-empty value: treated as full log file path

- `STREAM_HACK_PORT`
  - TCP listen port (default: `12345`)

- `STREAM_HACK_CHANNEL`
  - Fixed encoder channel to stream (default: `0`)
  - `-1` enables auto channel detection from first matching stream callback

- `STREAM_HACK_MAX_DROP_STREAK`
  - Max consecutive `would-block` frame drops before disconnecting slow client (default: `90`)

- `STREAM_HACK_LIBIMP`
  - Path to real Ingenic library (default: `/usr/lib/libimp.so`)

## Behavior notes

- Single-client model: only one TCP consumer at a time;
- New client connection replaces the previous one;
- Non-blocking socket send is used to avoid stalling fw app;
- On backpressure, frames may be dropped intentionally to keep the main process responsive;
- Connection is dropped only on persistent slowness or hard socket errors.

## Current limitations

- Video only (no audio forwarding);
- One TCP client at a time;
- Raw elementary stream transport (no RTSP/HTTP framing in hook itself).

## Troubleshooting

### No stream output

Check:

1. Hook loaded (`LD_PRELOAD` path correct);
2. Port open (`STREAM_HACK_PORT`);
3. Correct encoder channel (`STREAM_HACK_CHANNEL`, usually `0`);
4. Log file for connect/disconnect and socket errors.

### Main process restarts

Common causes:

- Incompatible toolchain;
- Blocking operations inside hook path;
- Too heavy logging;
- ABI mismatch with `IMPEncoderStream` layout.

Current code avoids blocking sends and uses T31X-compatible `imp/imp_encoder.h` layout.

### `unsupported header: ...` on go2rtc startup

Usually means stream did not begin with a decodable keyframe or packet header format mismatch.
Current implementation already mitigates this by:

- Waiting for a keyframe before sending;
- Forcing IDR on connect;
- Converting AVCC packets to Annex-B when needed.

## Build

Toolchain used: https://github.com/Dafang-Hacks/mips-gcc472-glibc216-64bit

```bash
make clean && make build
```

Result: `dist/libimp.so` in project root.

## Maintainer notes (important)

These points should be kept in mind for future changes:

1. **Header/ABI must match camera firmware.**
   `IMPEncoderStream` layout differs across SDK/header versions. Wrong struct layout causes random failures/crashes.

2. **Do not block in `IMP_Encoder_GetStream` hook path.**
   Any blocking I/O can trigger watchdog.

3. **Keyframe-first startup is required.**
   Sending P-frames first often breaks decoder startup.

4. **Annex-B vs AVCC can vary.**
   Some payloads arrive without Annex-B start codes; normalize to Annex-B for better player compatibility.

5. **Drop frames under pressure, do not stall producer.**
   It is better to drop and recover via IDR than to block encoder callback flow.

6. **Keep logging optional and lightweight.**
   High-frequency logs can destabilize low-power camera environments.

7. **Prefer minimal complexity in hook code.**
   This code runs in a critical process. Simpler is safer.

## Special thanks

Inspired by [FiveLeavesLeft](https://github.com/FiveLeavesLeft/) and [WyzeCameraLiveStream](https://github.com/FiveLeavesLeft/WyzeCameraLiveStream) project.

Thanks to [AlexxIT](https://github.com/AlexxIT/go2rtc) for go2rtc project, [Dafang-Hacks](https://github.com/Dafang-Hacks) / [EliasKotlyar](https://github.com/EliasKotlyar) for toolchain and [gtxaspec](https://github.com/gtxaspec) for clues.

Thanks to [Codex](https://openai.com/codex/) for helping with refactoring.

## License

[The MIT License (MIT)](LICENSE)
