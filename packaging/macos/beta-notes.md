**Beta.** This is a first macOS build of the Super Mario All-Stars PC port. Please report anything that doesn't work (see below).

**No game data is included.** You need your own **Super Mario All-Stars (USA)** ROM (CRC32 `925637C7`), dumped from your own cartridge. This is a free fan project, not affiliated with Nintendo.

## Install
1. Download `SuperMarioAllStars-macOS.zip` below and open it. Move **Super Mario All-Stars** to your Applications folder.
2. Open it. The first time, macOS says it can't check the app for malware, because it isn't signed with a paid Apple developer account. To open it anyway:
   - **macOS 15 Sequoia and later:** click **Done**, then open System Settings → Privacy & Security, scroll down and click **Open Anyway** next to "Super Mario All-Stars", and confirm.
   - **macOS 11 to 14:** right-click (or Control-click) the app, choose **Open**, then **Open** again.

   You only do this once.
3. Click the "ROM not found" card and choose your `Super Mario All-Stars (USA).sfc`. It's copied in, so you only do this once. You can also drag the ROM onto the window.

## Notes
- **Requirements:** macOS 11 Big Sur or later, on an Apple silicon or Intel Mac. The picture is drawn with Metal.
- **Fullscreen:** Option+Return or Ctrl+Cmd+F switches between windowed and fullscreen.
- **Controllers:** Xbox, PlayStation, Switch Pro and other controllers macOS supports work, wired or Bluetooth.
- **Where your files go:** saves, states and settings are kept in `~/Library/Application Support/SuperMarioAllStars`. A `roms` folder there, or beside the app, is searched for the ROM as well.
- **Crackling sound?** Raise Options → Audio → Latency, for example to 100 ms.
- **Slang shaders:** Options → Graphics → Slang Shader runs the seven bundled CRT presets (RetroArch's, through librashader). Your own `.slangp` presets go in a `shaders` folder in `~/Library/Application Support/SuperMarioAllStars`.
- **Not in this version:** High Frame Rate is Windows-only.

## Found a problem?
Please [open an issue](https://github.com/Darhkfox/SuperMarioAllStars/issues) and include:
- Your Mac model and macOS version.
- What happened.
- The log file at `~/Library/Application Support/SuperMarioAllStars/smas_port.log`.
