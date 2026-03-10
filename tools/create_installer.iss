#define AppName "ObeyCraft Launcher"
#define AppVersion "1.0"
#define AppPublisher "ObiJello"
#define AppExeName "ObeyCraftLauncher.exe"
#define BuildDir "..\cmake-build-release\bin"

[Setup]
AppId={{F3A7C2B1-8D4E-4F9A-B631-2E5C7D8A1F03}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\ObeyCraft
DisableProgramGroupPage=yes
OutputDir=..\cmake-build-release
OutputBaseFilename=ObeyCraftLauncherInstaller
SetupIconFile=..\assets\launcher\logo.ico
Compression=lzma
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#AppExeName}
PrivilegesRequired=lowest

[Files]
Source: "{#BuildDir}\{#AppExeName}";        DestDir: "{app}";           Flags: ignoreversion
Source: "{#BuildDir}\fonts\*";              DestDir: "{app}\fonts\";    Flags: ignoreversion recursesubdirs
Source: "{#BuildDir}\launcher\logo.png";    DestDir: "{app}\launcher\"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}";  Filename: "{app}\{#AppExeName}"

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent
