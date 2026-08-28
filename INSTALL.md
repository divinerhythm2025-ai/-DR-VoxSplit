# Installing DR-VoxSplit

DR-VoxSplit is a VST3 plugin for drag-and-drop vocal/instrumental stem
separation. Builds are currently **unsigned** (no Apple Developer ID /
Windows code-signing certificate yet), so both platforms will show a
security warning on first run - this is expected, not a sign anything is
wrong.

## Windows

1. Download `DR-VoxSplit-Setup-x64.exe` from the [latest release](../../releases).
2. Run it. Windows SmartScreen will likely show **"Windows protected your
   PC"**. Click **More info**, then **Run anyway**.
3. Follow the installer - it installs to the common VST3 folder
   (`C:\Program Files\Common Files\VST3\DR-VoxSplit.vst3`) automatically.
4. Rescan plugins in your DAW if it doesn't appear right away.

## macOS

1. Download `DR-VoxSplit-Installer-macos.pkg` from the [latest release](../../releases).
2. Gatekeeper will likely refuse to open it ("Apple could not verify..."). Go
   to **System Settings → Privacy & Security**, scroll down, and click
   **Open Anyway** next to the DR-VoxSplit message. (Or: right-click the
   `.pkg` in Finder and choose **Open**.)
3. Follow the installer - it installs to
   `/Library/Audio/Plug-Ins/VST3/DR-VoxSplit.vst3` automatically.
4. Rescan plugins in your DAW if it doesn't appear right away.

## High-quality (fine-tuned) model - optional

The default install uses the standard model. A higher-quality fine-tuned
model (`htdemucs_ft.safetensors`, ~320MB) is available as a separate
download on the release page for anyone who wants it:

1. Download `htdemucs_ft.safetensors` from the release.
2. Find your installed plugin's binary folder:
   - Windows: `C:\Program Files\Common Files\VST3\DR-VoxSplit.vst3\Contents\x86_64-win\`
   - macOS: `/Library/Audio/Plug-Ins/VST3/DR-VoxSplit.vst3/Contents/MacOS/`
3. Drop the file in next to the other model file. The plugin will pick it
   up automatically as a "High Quality" option next time you open it.

## Uninstalling

- Windows: use **Add or Remove Programs**, or run the uninstaller left in
  the install folder.
- macOS: delete `/Library/Audio/Plug-Ins/VST3/DR-VoxSplit.vst3`.
