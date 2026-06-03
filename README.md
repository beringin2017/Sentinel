# Sentinel

A lightweight Windows utility that records webcam (and optional audio) footage when your laptop is locked, saves a trimmed incident clip if physical interaction is detected, and discards everything otherwise. It also supports schedule-based arming and optional email notifications with video attachments.

## How it works

1. **Schedule Check**: When the session is locked (Win+L), Sentinel checks if the current day and time fall within the configured arming schedule. If outside the schedule, it remains inactive.
2. **Recording**: If armed, Sentinel starts recording from the configured webcam (and optional audio device) in configurable segments (default 60 seconds) via FFmpeg.
3. **Trigger Detection**: While locked, it monitors for physical keyboard or mouse activity using the Raw Input API.
4. **Immediate Alert**: If activity is detected, Sentinel immediately fires a detached background email alert (if email is configured) to notify you that an incident is being recorded, without blocking the input loop.
5. **On Unlock**:
   - If **no activity** was detected (or unlocked within the "quick unlock" window), all footage is silently deleted.
   - If **activity** was detected, Sentinel trims the footage to a configurable window around the trigger event, merges it into a single `incident.mp4`, and sends a final email notification with the video attached (subject to the configured size limit).

## Requirements

- Windows 10 or 11
- [FFmpeg](https://ffmpeg.org/download.html) accessible on your system `PATH`
- A C++17-capable compiler (MSVC via Visual Studio 2019+ recommended)
- Network access (only required if email notifications are enabled)

## Building

Open the solution in Visual Studio and build in Release x64. The project uses a precompiled header (`pch.h`) and links against standard Windows libraries (`ws2_32.lib`, `secur32.lib`, `wtsapi32.lib`) via `#pragma comment`. No external dependencies beyond the Windows SDK and FFmpeg are required.

## Configuration

Configuration is now managed via a `sentinel.ini` file located in the same directory as `Sentinel.exe`. If the file does not exist, Sentinel will generate a default one with explanatory comments. 

The configuration is organized into three sections:

### `[Recording]`
| Key | Default | Description |
| --- | --- | --- |
| `CameraName` | `Integrated Camera` | DirectShow device name of your webcam. |
| `AudioName` | *(empty)* | DirectShow device name of your microphone. Leave blank for video-only recording. |
| `PreTriggerMins` | `5` | Minutes of footage to keep before the trigger event. |
| `PostTriggerMins` | `10` | Minutes of footage to keep after the trigger event. |
| `MaxRecordingMins` | `30` | Rolling window: segments older than this are pruned while locked and untriggered to save disk space. |
| `QuickUnlockSecs` | `3` | Unlock within this many seconds with no trigger -> treated as owner, footage deleted. |

### `[Schedule]`
| Key | Default | Description |
| --- | --- | --- |
| `ArmDays` | `Mon,Tue,Wed,Thu,Fri` | Comma-separated day abbreviations (`Mon`,`Tue`,`Wed`,`Thu`,`Fri`,`Sat`,`Sun`). Leave blank to arm every day. |
| `ArmStart` | `09:00` | 24-hour `HH:MM` start time. Leave blank to arm all day. |
| `ArmEnd` | `18:00` | 24-hour `HH:MM` end time. Leave blank to arm all day. |

### `[Email]`
| Key | Default | Description |
| --- | --- | --- |
| `SmtpHost` | `smtp.gmail.com` | SMTP server hostname. |
| `SmtpPort` | `465` | SMTP server port (typically 465 for implicit TLS). |
| `SmtpUser` | *(empty)* | SMTP authentication username (e.g., your email address). |
| `SmtpPass` | *(empty)* | SMTP authentication password (use an App Password for services like Gmail). |
| `EmailTo` | *(empty)* | Recipient email address for incident alerts. |
| `MaxAttachMB` | `25` | Skip video attachment in emails if the processed file exceeds this size in megabytes. |

> **Tip:** To find your exact camera or audio device names, run:  
> `ffmpeg -list_devices true -f dshow -i dummy`

## Output

Each triggered session produces a folder under `monitor_sessions\` containing a single merged `incident.mp4`. Untriggered sessions leave no files on disk. If email is configured, notifications are sent to the specified recipient.

## Limitations

- `RIDEV_INPUTSINK` receives input routed to the current user session. Full capture of keystrokes entered on the Winlogon desktop (i.e., password attempts) requires a separate Windows Service running as `SYSTEM`.
- Requires FFmpeg to be accessible on `PATH` at runtime.
- Email notifications rely on Windows Schannel for TLS. Some highly restrictive corporate networks or non-standard SMTP configurations may require adjustments.
- Video attachments are omitted from emails if the final `incident.mp4` exceeds the `MaxAttachMB` limit, though a text-only alert is still sent.

## License

None, go nuts.

## [v2.0.0] - Major Update

### Added
- **Email Notifications**: Integrated SMTP email support with TLS/Schannel encryption to send incident alerts and processed footage.
- **Schedule-Based Arming**: Added configuration options (`ArmDays`, `ArmStart`, `ArmEnd`) to restrict monitoring to specific days and time windows.
- **Audio Recording**: Added support for capturing audio alongside video via the new `AudioName` configuration parameter.
- **Attachment Size Limit**: Introduced `MaxAttachMB` configuration to skip video attachments in emails if the processed file exceeds the specified size limit.
- **Immediate Activity Alert**: Added logic to fire a detached background email alert immediately when physical input is detected on a locked machine, before the session ends.
- **Structured Configuration**: Updated `sentinel.ini` generation and parsing to support INI-style sections (`[Recording]`, `[Schedule]`, `[Email]`) for better organization.
- **Base64 Encoding**: Added a helper function for Base64 encoding to support SMTP authentication and file attachments.

### Changed
- **FFmpeg Command Construction**: Refactored `StartFFmpegRecording` to dynamically build input and mapping arguments based on whether an audio device is configured.
- **Session End Processing**: Updated `EndSession` to automatically invoke `SendEmail` with the merged `incident.mp4` file attached upon successful processing.
- **Schedule Logic**: Refactored `IsArmed()` to evaluate day and time restrictions independently. An empty configuration field now correctly means "no restriction on that axis" rather than "fully disarmed".

### Fixed
- **Socket Leak**: Balanced `WSAStartup` and `WSACleanup` calls within the TLS connection lifecycle (`TlsConnect` and `TlsClose`) to prevent Windows Socket API reference count leaks across multiple `SendEmail` invocations.
- **Thread Handle Leak**: Fixed a handle leak in `RawInputWndProc` by immediately calling `CloseHandle(h)` after spawning the detached email alert thread.
- **Data Race**: Introduced `g_sessionDirMutex` to protect `g_currentSessionDir`, preventing concurrent read/write data races between `SessionWorkerThread` and `SegmentWatcherThread`.
- **Schedule Logic**: Corrected the `IsArmed()` evaluation to ensure empty day/time strings do not inadvertently disarm the system.
- **Config Parsing Crash**: Added `!val.empty()` guards to all `std::stoi` calls within `LoadConfig` to prevent `std::invalid_argument` exceptions and crashes when parsing empty or malformed numeric configuration values.