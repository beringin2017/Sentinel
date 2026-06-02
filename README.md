# Sentinel

A lightweight Windows utility that records webcam footage when your laptop is locked, saves a trimmed incident clip if physical interaction is detected, and discards everything otherwise.

## How it works

1. When the session is locked (Win+L), Sentinel starts recording from the webcam in 60-second segments via FFmpeg.
2. While locked, it monitors for physical keyboard or mouse activity using the Raw Input API.
3. On unlock:
   - If no activity was detected, all footage is silently deleted.
   - If activity was detected, Sentinel trims the footage to a configurable window around the trigger event and merges it into a single `incident.mp4`.

## Requirements

- Windows 10 or 11
- [FFmpeg](https://ffmpeg.org/download.html) on your `PATH`
- A C++17-capable compiler (MSVC via Visual Studio 2019+ recommended)

## Building

Open the solution in Visual Studio and build in Release x64. No external dependencies beyond the Windows SDK and FFmpeg.

## Configuration

All options are constants at the top of `Sentinel.cpp`:

| Constant | Default | Description |
|---|---|---|
| `CAMERA_NAME` | `"Integrated Camera"` | DirectShow device name of your webcam |
| `SEGMENT_DURATION_SECONDS` | `60` | Length of each raw footage segment |
| `PRE_TRIGGER_MINS` | `5` | Minutes of footage to keep before the trigger |
| `POST_TRIGGER_MINS` | `10` | Minutes of footage to keep after the trigger |
| `BASE_DIR` | `"monitor_sessions\\"` | Output directory for sessions and incident clips |

To find your exact camera name, run:
```
ffmpeg -list_devices true -f dshow -i dummy
```

## Output

Each triggered session produces a folder under `monitor_sessions\` containing a single `incident.mp4`. Untriggered sessions leave no files on disk.

## Limitations

- `RIDEV_INPUTSINK` receives input routed to the current user session. Full capture of keystrokes entered on the Winlogon desktop (i.e. password attempts) requires a separate Windows Service running as SYSTEM.
- Requires FFmpeg to be accessible on `PATH` at runtime.

## License

None, go nuts.
