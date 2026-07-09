; ============================================================
;  Aegisy Client - Inno Setup 安装脚本
;  用法：先跑 package-windows.bat 生成 dist\AegisyClient\，
;        再用 Inno Setup 编译本文件，产出安装程序 exe。
;  下载 Inno Setup: https://jrsoftware.org/isdl.php
; ============================================================

#define MyAppName "Aegisy Client"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Aegisy"
#define MyAppURL "https://www.aegisy.cc"
#define MyAppExeName "AegisyClient.exe"

[Setup]
AppId={{A1E5C7D2-3B4F-4E8A-9C1D-6F2A8B3E5D7C}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={autopf}\AegisyClient
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; 不需要管理员权限：装到当前用户目录
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=dist
OutputBaseFilename=AegisyClientSetup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; 若有图标可取消注释：
; SetupIconFile=resources\app.ico

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
; 简体中文：Inno Setup 默认不带 ChineseSimplified.isl，需先下载放到
; Inno Setup 安装目录的 Languages\ 下，再取消下面这行注释：
; Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加图标:"

[Files]
; 打包 package-windows.bat 收集好的整个目录
Source: "dist\AegisyClient\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "立即运行 {#MyAppName}"; Flags: nowait postinstall skipifsilent
