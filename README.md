# vlc-resume

A VLC interface plugin that automatically saves the current playback position every 5 seconds and resumes from that position when the same file is played again.

## Features

- Works with playlists and single files
- Position saved per MRL — multiple files tracked independently in one JSON file
- Crash-safe: atomic write via `.tmp` + `rename()` / `MoveFileEx`
- Skips resume if saved position is under 10 seconds (treat as "just started")
- Deletes the entry when a track finishes cleanly (`INPUT_EVENT_STATE` → `END_S`)
- Playlist-level resume: reopening the same playlist jumps to the same track and second
- No UI, no configuration needed — runs silently as a background interface module

## State file

Positions are stored in:

| Platform | Path |
|----------|------|
| Linux | `~/.local/share/vlc/resume.json` |
| macOS | `~/Library/Application Support/org.videolan.vlc/resume.json` |
| Windows | `%APPDATA%\vlc\resume.json` |

Format:
```json
{
  "positions": {
    "file:///music/track01.mp3": 183400,
    "file:///movies/film.mkv":   5432100
  },
  "last_session": {
    "tracks": ["file:///music/track01.mp3", "file:///music/track02.mp3"],
    "index":  1,
    "mrl":    "file:///music/track02.mp3",
    "ms":     45200
  }
}
```

All times are in milliseconds.

- **`positions`** — last position per file (MRL → ms), used to resume *within* a
  track. Updated every 5 s; an entry is deleted when its track finishes cleanly.
- **`last_session`** — the playlist that was playing, the track within it
  (`mrl`/`index`) and its position (`ms`). Used to jump back to the right song
  when the same playlist is reopened. The `tracks` list is the flat set of leaf
  MRLs, so a nested input (e.g. an `.m3u8`) is stored as its real tracks, not
  the container.

## Build

### Dependencies

- CMake ≥ 3.16
- A C11 compiler (clang or gcc)
- VLC plugin headers

**macOS (Homebrew VLC):**
```bash
brew install cmake
```

The headers ship inside the VLC `.app` bundle. Point CMake to them:
```bash
cmake -B build \
  -DVLC_INCLUDE_DIRS=/Applications/VLC.app/Contents/MacOS/include
cmake --build build
```

**Linux (Debian/Ubuntu):**
```bash
sudo apt install cmake vlc-plugin-base libvlc-dev
cmake -B build
cmake --build build
```

### Install

```bash
cmake --install build
```

This copies the plugin to the per-user VLC plugin directory
(`~/.local/lib/vlc/plugins` on macOS/Linux).

### Auto-load on every VLC start

The plugin lives outside the signed `VLC.app` bundle, so VLC must be told where
to find it (`VLC_PLUGIN_PATH`) and which extra interface to load (`extraintf`).

**1. Enable the interface** in `vlcrc`
(`~/Library/Preferences/org.videolan.vlc/vlcrc` on macOS):
```ini
extraintf=vlc_resume
```

**2. Export `VLC_PLUGIN_PATH` globally (macOS LaunchAgent).**
Create `~/Library/LaunchAgents/org.videolan.vlc.pluginpath.plist`:
```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>org.videolan.vlc.pluginpath</string>
    <key>ProgramArguments</key>
    <array>
        <string>/bin/launchctl</string>
        <string>setenv</string>
        <string>VLC_PLUGIN_PATH</string>
        <string>/Users/YOUR_USER/.local/lib/vlc/plugins</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
</dict>
</plist>
```
Then load it (runs automatically at every login afterwards):
```bash
launchctl load ~/Library/LaunchAgents/org.videolan.vlc.pluginpath.plist
launchctl getenv VLC_PLUGIN_PATH   # should print the plugin dir
```

**3. Disable VLC's own "continue where you left off" on macOS.**
VLC's native resume (`macosx-continue-playback`) restores the last item *and*
duplicates the playlist on launch, which races against this plugin's resume and
produces wrong/no jumps. Set it to *Never* so the plugin is the single source of
truth — in `vlcrc`:
```ini
macosx-continue-playback=2
```
(Or in the GUI: Preferences → Interface → "Continue playback?" → Never.)

Restart VLC. To verify the plugin loaded:
```
Tools → Messages (Ctrl+M) → filter "resume"
```
Or from the command line:
```bash
VLC_PLUGIN_PATH="$HOME/.local/lib/vlc/plugins" \
  /Applications/VLC.app/Contents/MacOS/VLC -I dummy --list 2>&1 | grep resume
```

## Architecture

```
plugin.c           — Open() / Close(), module descriptor
events.c           — InputCurrentCallback (track change), IntfEventCallback (position/state)
timer.c            — background thread: 200 ms poll loop; deferred seek, playlist
                     resume jump, and a disk flush every 5 s
state.c            — read/write/delete positions + last_session in resume.json (atomic)
vlc_resume_utils.h — recursive leaf snapshot of the playlist + MRL lookup helpers
cJSON.c/h          — embedded cJSON v1.7.17 (MIT)
```

**Resume seek timing.** A position seek is wanted only once `INPUT_EVENT_STATE`
reaches `PLAYING_S` (seeking during `OPENING_S`/`BUFFERING_S` is ignored by VLC).
But that callback runs on the input event thread, where a blocking
`var_SetInteger("time")` would deadlock — so the callback only *flags* the seek
(`b_seek_pending`) and the timer thread performs it. The `b_resumed` flag
prevents a second seek if the player briefly pauses within the same track.

**Playlist resume jump.** Opening an `.m3u8` expands asynchronously, often only
*after* the first track is already playing, with no further track-change
callback. The jump therefore lives in the timer's 200 ms poll, not in a
callback: it retries until the live playlist matches the saved session, jumps to
the saved track by MRL, and re-asserts if VLC's own autoplay clobbers it —
bounded by an 8 s deadline so it never fights a user navigating manually. Saving
`last_session` is suppressed while a jump is pending (and for container-only
snapshots) so the save path can't overwrite the data being read to resume.

## Caveats

- VLC plugins are **ABI-tied** to the VLC version they were compiled against. Recompile after every VLC update.
- `p_input` can become `NULL` at any time; the code holds a reference via `vlc_object_hold()` and null-checks before every access.
- Network streams (HLS, RTSP) use their URL as MRL — resume works but seeking accuracy depends on the server.
