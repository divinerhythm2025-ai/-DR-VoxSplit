; Inno Setup script for DR-VoxSplit (VST3, Windows x64)
;
; Build with: iscc installer\windows\DR-VoxSplit.iss
; Requires Inno Setup 6+ (https://jrsoftware.org/isinfo.php)
;
; Expects the payload to already be staged at:
;   installer\windows\payload\'DR-VoxSplit.vst3\Contents\...
; (built VST3 bundle + demucs.exe, demucs-gpu.exe, htdemucs.safetensors
; copied alongside the plugin binary in Contents\x86_64-win - see
; SeparationEngine::locateBinaries(), which looks for these files next to
; the plugin's own executable file.)

#define MyAppName "DR-VoxSplit"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "DR"
#define MyAppURL "https://example.com"

[Setup]
AppId={{6E6C6F41-2D48-4F0A-9C2E-6D4444565358}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=LICENSE.txt
OutputDir=..\..\dist
OutputBaseFilename=DR-VoxSplit-Setup-x64
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern

[Files]
Source: "payload\'DR-VoxSplit.vst3\*"; DestDir: "{app}\'DR-VoxSplit.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: filesandordirs; Name: "{app}\'DR-VoxSplit.vst3"

[Messages]
WelcomeLabel2=This will install {#MyAppName} on your computer.%n%nThe plugin will be installed to the common VST3 folder so it's picked up by any VST3-compatible DAW (Ableton Live, FL Studio, Cubase, Studio One, REAPER, etc.). Rescan your DAW's plugin list after installing if it doesn't appear automatically.
