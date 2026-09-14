#define MyAppName "Amalgam Launcher"
#ifndef MyAppVersion
#error A release version must be supplied with /DMyAppVersion=<version>
#endif
#define MyAppVerName MyAppName + " " + MyAppVersion
#ifndef MyFileVersion
#define MyFileVersion MyAppVersion
#endif
#define MyAppPublisher "Amalgam"
#ifndef SourceDir
#error A validated release staging directory must be supplied with /DSourceDir=<directory>
#endif
#ifndef OutputDir
#define OutputDir "..\dist\installer"
#endif
#ifndef OutputBaseFilename
#define OutputBaseFilename "AmalgamLauncher-" + MyAppVersion + "-Setup"
#endif

[Setup]
AppId={{B4D03A61-7F06-4E44-9F42-5B6C0B1C2F9E}
AppName={#MyAppName}
AppVerName={#MyAppVerName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\AmalgamLauncher
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
CloseApplications=yes
RestartApplications=no
Uninstallable=yes
UninstallDisplayIcon={app}\amalgam_launcher.exe
UninstallDisplayName={#MyAppName}
ChangesAssociations=no
VersionInfoVersion={#MyFileVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} Installer
VersionInfoCopyright=Copyright (c) 2026 Amalgam
LicenseFile={#SourceDir}\LICENSE.txt
InfoBeforeFile={#SourceDir}\INSTALL-INFO.txt
WizardImageFile=AmalgamInstallerWizard.bmp
WizardSmallImageFile=AmalgamInstallerSmall.bmp
SetupIconFile=amalgam.ico
; Amalgam brand purple (#6C3CE1) for modern wizard header
WizardImageBackColor=clBlack

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"


[Types]
Name: "full"; Description: "Amalgam Launcher + local AI"; Flags: iscustom
Name: "core"; Description: "Amalgam Launcher only"

[Components]
Name: "launcher"; Description: "Amalgam Launcher (core files)"; Types: full core; Flags: fixed
Name: "ai"; Description: "Amalgam AI - runtime included; models download from Settings (~10.2 GB)"; Types: full

[Tasks]
Name: "desktopicon"; Description: "Create desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: checkedonce
Name: "startmenu"; Description: "Create Start menu shortcuts"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\amalgam_launcher.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\amalgam.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\bridges\*.jar"; DestDir: "{app}\bridges"; Flags: ignoreversion
Source: "{#SourceDir}\bedrock\AmalgamBedrockClient.mcaddon"; DestDir: "{app}\bedrock"; Flags: ignoreversion
Source: "{#SourceDir}\launcher.json.template"; DestDir: "{app}"; Flags: ignoreversion onlyifdoesntexist
Source: "{#SourceDir}\prerequisites.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\component-manifest.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\sbom.cdx.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\release.sha256"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\tools\ai-bootstrap.ps1"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "{#SourceDir}\tools\ai-bootstrap.cmd"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "{#SourceDir}\ai\ai-package-manifest.json"; DestDir: "{app}\ai"; Flags: ignoreversion
Source: "{#SourceDir}\ai\ai-manifest.json"; DestDir: "{app}\ai"; Flags: ignoreversion
Source: "{#SourceDir}\ai\knowledge\*"; DestDir: "{app}\ai\knowledge"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\legal\TERMS.txt"; DestDir: "{app}\legal"; Flags: ignoreversion
Source: "{#SourceDir}\legal\PRIVACY.txt"; DestDir: "{app}\legal"; Flags: ignoreversion
Source: "{#SourceDir}\legal\AI_TERMS.txt"; DestDir: "{app}\legal"; Flags: ignoreversion
Source: "{#SourceDir}\legal\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}\legal"; Flags: ignoreversion
Source: "{#SourceDir}\runtimes\ai\llama\*.exe"; DestDir: "{app}\runtimes\ai\llama"; Flags: ignoreversion
Source: "{#SourceDir}\runtimes\ai\llama\*.dll"; DestDir: "{app}\runtimes\ai\llama"; Flags: ignoreversion
Source: "{#SourceDir}\runtimes\ai\sd\*.exe"; DestDir: "{app}\runtimes\ai\sd"; Flags: ignoreversion
Source: "{#SourceDir}\runtimes\ai\sd\*.dll"; DestDir: "{app}\runtimes\ai\sd"; Flags: ignoreversion
Source: "{#SourceDir}\USER-GUIDE.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\RELEASE-NOTES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\branding\*"; DestDir: "{app}\branding"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\LICENSE.txt"; DestDir: "{app}\legal"; Flags: ignoreversion

[Dirs]
Name: "{app}\bridges"
Name: "{app}\bedrock"
Name: "{app}\instances"
Name: "{app}\assets"
Name: "{app}\runtimes\java"
Name: "{app}\runtimes\ai\llama"
Name: "{app}\runtimes\ai\sd"
Name: "{app}\branding"
Name: "{app}\legal"
Name: "{app}\ai"
Name: "{app}\tools"
Name: "{localappdata}\Amalgam\AI\Models\brain"
Name: "{localappdata}\Amalgam\AI\Models\art"
Name: "{localappdata}\Amalgam\AI\Downloads"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\amalgam_launcher.exe"; WorkingDir: "{app}"; Tasks: startmenu

Name: "{group}\User Guide"; Filename: "{app}\USER-GUIDE.md"; Tasks: startmenu
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"; Tasks: startmenu
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\amalgam_launcher.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\amalgam_launcher.exe"; Description: "Launch {#MyAppName}"; WorkingDir: "{app}"; Flags: postinstall skipifsilent unchecked

[Code]
// ══════════════════════════════════════════════════════════════════════════
// Amalgam Launcher Installer — Custom Wizard Code
// Brand color: #6C3CE1 (Amalgam purple)
// ══════════════════════════════════════════════════════════════════════════

var
  AIMsgPage: TWizardPage;
  AIAck: TNewCheckBox;
  HwCheckPage: TWizardPage;
  PrivPage: TWizardPage;
  PrivAck: TNewCheckBox;

// ── Welcome page customization ───────────────────────────────────────────
procedure InitializeWizard();
var
  InfoText: TNewStaticText;
  HwText: TNewStaticText;
  PrivText: TNewStaticText;
begin
  // ── AI Information Page ──────────────────────────────────────────────────
  AIMsgPage := CreateCustomPage(wpSelectComponents, 'Amalgam AI',
    'Local AI for coding, vision, and art generation.');

  InfoText := TNewStaticText.Create(AIMsgPage);
  InfoText.Parent := AIMsgPage.Surface;
  InfoText.AutoSize := False;
  InfoText.WordWrap := True;
  InfoText.SetBounds(0, ScaleY(8), AIMsgPage.SurfaceWidth, ScaleY(200));
  InfoText.Caption :=
    'Amalgam AI runs locally on your PC. No cloud API or external software is required.' + #13#10 + #13#10 +
    'AI Brain: Chat, reasoning, coding, project analysis, and live Minecraft vision.' + #13#10 +
    'AI Art: Generate textures, FancyMenu art, item concepts, and GUI graphics.' + #13#10 + #13#10 +
    'Models: ~10.2 GB download (installed later from Settings > AI)' + #13#10 +
    'Recommended: 16+ GB RAM, 6+ GB VRAM, ~25 GB free disk' + #13#10 + #13#10 +
    'The AI runtime is included with this installer. Models download from Settings > AI.' + #13#10 +
    'Skip AI now and install it later without reinstalling Amalgam.' + #13#10 + #13#10 +
    'AI output may be incorrect. Review generated code and content before use.';

  AIAck := TNewCheckBox.Create(AIMsgPage);
  AIAck.Parent := AIMsgPage.Surface;
  AIAck.Caption := 'I understand and will review AI output before use.';
  AIAck.SetBounds(0, ScaleY(216), AIMsgPage.SurfaceWidth, ScaleY(24));

  // ── Hardware Requirements Page ───────────────────────────────────────────
  HwCheckPage := CreateCustomPage(AIMsgPage.ID, 'Hardware Requirements',
    'Minimum requirements for Amalgam AI.');

  HwText := TNewStaticText.Create(HwCheckPage);
  HwText.Parent := HwCheckPage.Surface;
  HwText.AutoSize := False;
  HwText.WordWrap := True;
  HwText.SetBounds(0, ScaleY(8), HwCheckPage.SurfaceWidth, ScaleY(150));
  HwText.Caption :=
    'Amalgam detects hardware during AI setup.' + #13#10 + #13#10 +
    'Minimum for comfortable AI use:' + #13#10 +
    '  16 GB system RAM (32 GB recommended)' + #13#10 +
    '  4 GB VRAM (8+ GB for art generation)' + #13#10 +
    '  Vulkan-compatible GPU (AMD, NVIDIA, Intel)' + #13#10 +
    '  ~25 GB free disk space for models' + #13#10 + #13#10 +
    'CPU-only fallback is supported but AI art generation will be slower.' + #13#10 + #13#10 +
    'The installer will verify your hardware and download the best models for your system.';

  // ── Privacy / Data Page ──────────────────────────────────────────────────
  PrivPage := CreateCustomPage(wpLicense, 'Your Data & Privacy',
    'How Amalgam handles your information.');

  PrivText := TNewStaticText.Create(PrivPage);
  PrivText.Parent := PrivPage.Surface;
  PrivText.AutoSize := False;
  PrivText.WordWrap := True;
  PrivText.SetBounds(0, ScaleY(8), PrivPage.SurfaceWidth, ScaleY(180));
  PrivText.Caption :=
    'LOCAL AI: Chat, coding, vision, and art run on your PC. Your data stays on your machine.' + #13#10 + #13#10 +
    'ONLINE FEATURES: The following require network access:' + #13#10 +
    '  Microsoft account sign-in (device-code flow, no password stored)' + #13#10 +
    '  Mod/content provider searches (Modrinth, CurseForge)' + #13#10 +
    '  Essentials social features (friends, invites, sessions)' + #13#10 +
    '  Software updates and AI model downloads' + #13#10 + #13#10 +
    'Saved tokens are DPAPI-protected for this Windows account.' + #13#10 +
    'See Start Menu > Legal for full Terms, Privacy, and AI policies.';

  PrivAck := TNewCheckBox.Create(PrivPage);
  PrivAck.Parent := PrivPage.Surface;
  PrivAck.Caption := 'I understand that Java Edition launch requires a valid Microsoft account.';
  PrivAck.SetBounds(0, ScaleY(196), PrivPage.SurfaceWidth, ScaleY(24));

end;

// ── Page transition logic ────────────────────────────────────────────────
procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpWelcome then begin
    WizardForm.WelcomeLabel1.Caption := 'Welcome to Amalgam';
    WizardForm.WelcomeLabel2.Caption :=
      'Your unified Minecraft platform.' + #13#10 +
      'Launch, discover modpacks, play with friends, and create with AI.';
  end
  else if CurPageID = wpInstalling then
    WizardForm.StatusLabel.Caption := 'Installing Amalgam...'
  else if CurPageID = wpFinished then
    WizardForm.FinishedLabel.Caption :=
      'Amalgam is installed and ready to launch.' + #13#10 +
      'Install AI models from Settings > AI after launch (~10.2 GB).';
end;

// ── Progress display during file copy ───────────────────────────────────
procedure CurInstallProgressChanged(CurProgress, MaxProgress: Integer);
var
  Pct: Integer;
begin
  if MaxProgress <= 0 then Exit;
  Pct := (CurProgress * 100) div MaxProgress;
  WizardForm.StatusLabel.Caption := Format('Installing Amalgam... %d%%', [Pct]);
end;

// ── Next button validation ──────────────────────────────────────────────
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = PrivPage.ID) and (not WizardSilent()) and not PrivAck.Checked then begin
    MsgBox('Please confirm that you understand the Microsoft account requirement.', mbError, MB_OK);
    Result := False;
  end;
  if (CurPageID = AIMsgPage.ID) and (not WizardSilent()) and not AIAck.Checked then begin
    MsgBox('Please confirm you understand and will review AI output before use.', mbError, MB_OK);
    Result := False;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  // Silent uninstall must remain unattended. Never show the destructive
  // user-data prompt in /VERYSILENT mode; core application files are still
  // removed by Inno Setup, while profiles and worlds stay protected.
  if (CurUninstallStep = usUninstall) and (not UninstallSilent()) and
     (MsgBox(
       'Do you also want to remove Amalgam AI models, profiles, modpacks, worlds, servers, backups, and downloaded content?' + #13#10 + #13#10 +
       'This permanently deletes Amalgam data for this Windows account. Files outside Amalgam folders are not removed.',
       mbConfirmation, MB_YESNO) = IDYES) then begin
    DelTree(ExpandConstant('{localappdata}\Amalgam'), True, True, True);
    DelTree(ExpandConstant('{localappdata}\instances'), True, True, True);
    DelTree(ExpandConstant('{localappdata}\modpacks'), True, True, True);
    DelTree(ExpandConstant('{localappdata}\backups'), True, True, True);
    DelTree(ExpandConstant('{app}\instances'), True, True, True);
    DelTree(ExpandConstant('{app}\assets'), True, True, True);
    // Runtime-written configuration in the install directory is Amalgam data
    // too; remove it only on the confirmed destructive path.
    DeleteFile(ExpandConstant('{app}\launcher.json'));
    DeleteFile(ExpandConstant('{app}\launcher.json.bak'));
    DeleteFile(ExpandConstant('{app}\downloads.json'));
    DeleteFile(ExpandConstant('{app}\imgui.ini'));
    DelTree(ExpandConstant('{app}\logs'), True, True, True);
  end;
end;

// ── Ready-to-install summary ────────────────────────────────────────────
function UpdateReadyMemo(Space, NewLine, MemoUserInfo, MemoDir, MemoType,
  MemoComponents, MemoGroup, MemoTasks: String): String;
begin
  Result :=
    'Amalgam is ready to install.' + NewLine + NewLine +
    MemoDir + NewLine + NewLine +
    'AI models can be installed from Settings > AI after launch (~10.2 GB).' + NewLine + NewLine +
    'Included: Launcher, AI runtime, bridges, branding, legal documents.' + NewLine + NewLine +
    'Shortcuts:' + NewLine + MemoTasks;
end;
