; Team Launcher (portage C++) — script Inno Setup 6.
;
; Repris du script du launcher C# (installer.iss a la racine), avec les
; corrections que le passage au C++ impose ou rend possibles :
;
;  1. Plus aucune dependance .NET. L'ancien script embarquait `*.deps.json`,
;     `*.runtimeconfig.json` et une fonction `IsDotNet8Installed` qui n'etait
;     JAMAIS appelee : rien ne verifiait donc le runtime, et l'installation
;     reussissait sur une machine sans .NET pour echouer au premier lancement.
;     Ici il n'y a que l'executable et SDL2.dll.
;  2. `InitializeSetup` faisait un `taskkill /f` inconditionnel sur
;     TeamLauncher.exe, AVANT meme que l'utilisateur ait vu le premier ecran,
;     et donc aussi quand il annulait ensuite l'installation. Tuer de force
;     pouvait perdre la configuration en cours d'ecriture. On s'appuie
;     desormais sur `CloseApplications`, qui demande poliment la fermeture et
;     ne propose la force qu'en dernier recours.
;  3. `Source: "dist\*.dll"` prenait aveuglement tout ce qui trainait dans le
;     dossier de sortie. Les fichiers sont nommes un par un.
;  4. Desinstallation : les donnees utilisateur (instances, comptes, mondes)
;     sont dans %LOCALAPPDATA%\TeamLauncher et ne sont PAS supprimees sans
;     que l'utilisateur le demande explicitement. L'ancien script n'en disait
;     rien du tout.
;
; NON SIGNE : l'utilisateur n'a pas de certificat de signature de code
; (decision du 27/09/2026). SmartScreen affichera donc un avertissement au
; premier lancement d'un installeur telecharge. C'est attendu, ce n'est pas
; un defaut du script.

#define MyAppName "Team Launcher"
#define MyAppPublisher "Team Launcher"
#define MyAppURL "https://github.com/teamstarwars-dev/Team-Luncher-"
#define MyAppExeName "TeamLauncher.exe"
; Meme AppId que le launcher C# : une installation existante est donc mise a
; jour et non dupliquee dans « Applications installees ».
#define MyAppId "{{B5E3A8D2-7F4A-4E9C-A1D3-6B2E8F0C9D5A}"

; SourceDir et MyAppVersion sont passes par make-installer.ps1 :
;   ISCC /DBuildDir=... /DMyAppVersion=... TeamLauncher.iss
#ifndef BuildDir
  #define BuildDir "..\build"
#endif
#ifndef MyAppVersion
  #define MyAppVersion "6.0.0"
#endif
#ifndef OutDir
  #define OutDir "..\..\installer-output"
#endif

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
OutputDir={#OutDir}
OutputBaseFilename=TeamLauncher-{#MyAppVersion}-Setup
SetupIconFile=..\assets\TeamLauncher.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
; Installation par utilisateur par defaut : pas d'elevation demandee pour
; rien. Le bouton du dialogue permet de passer en « pour tous ».
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Demande la fermeture du launcher plutot que de le tuer (voir en-tete).
CloseApplications=yes
CloseApplicationsFilter={#MyAppExeName}
RestartApplications=no
LicenseFile=..\..\LICENSE.txt
; Le binaire fait ~2,7 Mo et SDL2.dll ~1,6 Mo : annonce honnete de la place.
ExtraDiskSpaceRequired=0

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "startmenuicon"; Description: "Créer un raccourci dans le menu Démarrer"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce

[Files]
Source: "{#BuildDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\SDL2.dll"; DestDir: "{app}"; Flags: ignoreversion
; SDK social Discord (amis, messages, vocal). CMake la depose a cote de
; l'executable quand third_party/discord_social_sdk est present ; elle est
; absente d'un build sans le SDK, ou tout compile et la page Amis explique
; ce qui manque. D'ou skipifsourcedoesntexist : on n'empeche pas
; d'empaqueter une version sans social.
Source: "{#BuildDir}\discord_partner_sdk.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
; default.env porte les reglages par defaut (URLs de services). Il est
; volontairement EXTERNE et jamais embarque dans le binaire, pour rester
; modifiable sans recompiler. Il peut etre absent : on ne bloque pas
; l'installation s'il manque.
;
; Ce qu'il ne porte PAS : la cle API CurseForge. Elle est injectee a la
; compilation et embarquee obfusquee (voir CMakeLists). La poser ici en
; clair la rendrait lisible d'un simple double-clic dans le dossier
; d'installation.
Source: "..\assets\default.env"; DestDir: "{app}\assets"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: startmenuicon
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"; Tasks: startmenuicon
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Le launcher ecrit son journal a cote de l'executable : sans cette ligne le
; dossier d'installation survivait a la desinstallation.
Type: files; Name: "{app}\launcher.log"
Type: dirifempty; Name: "{app}\assets"
Type: dirifempty; Name: "{app}"

[Messages]
french.WelcomeLabel2=Ceci va installer [name/ver] sur votre ordinateur.%n%nCette version est native : elle ne nécessite ni .NET, ni Java pour démarrer (Java reste nécessaire pour lancer le jeu, le launcher peut l'installer).
english.WelcomeLabel2=This will install [name/ver] on your computer.%n%nThis build is native: it needs neither .NET nor Java to start (Java is still required to run the game; the launcher can install it).

[Code]
// Les donnees utilisateur (instances, comptes, mondes, sauvegardes) vivent
// dans %LOCALAPPDATA%\TeamLauncher, hors du dossier d'installation. Elles ne
// sont JAMAIS supprimees automatiquement : une desinstallation n'est souvent
// qu'une etape vers une reinstallation.
//
// ACCIDENT DU 27/09/2026, a ne pas reproduire : une premiere version posait
// la question avec MB_DEFBUTTON2 (« Non » par defaut) et un test a lance la
// desinstallation avec /SUPPRESSMSGBOXES. Inno Setup renvoie **IDYES** pour
// un MB_YESNO supprime, SANS tenir compte du bouton par defaut — les donnees
// reelles de la machine de test ont ete effacees, sans corbeille ni cliche
// instantane pour les recuperer.
//
// Deux garde-fous en consequence :
//   1. En mode silencieux (`UninstallSilent`), on ne supprime RIEN. Aucune
//      automatisation, aucun script, aucun deploiement ne peut detruire des
//      donnees sans qu'un humain ait clique.
//   2. Deux confirmations successives, la seconde nommant ce qui sera perdu.
//      Un clic distrait sur « Oui » ne suffit pas.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;

  // Garde-fou 1 : jamais de suppression sans interaction humaine.
  if UninstallSilent then
    Exit;

  DataDir := ExpandConstant('{localappdata}\TeamLauncher');
  if not DirExists(DataDir) then
    Exit;

  if MsgBox('Vos données (instances, comptes, mondes, sauvegardes) sont conservées :'
            + #13#10 + #13#10 + DataDir + #13#10 + #13#10
            + 'Voulez-vous les supprimer définitivement ?'
            + #13#10 + 'Cette action est IRRÉVERSIBLE (pas de corbeille).',
            mbConfirmation, MB_YESNO or MB_DEFBUTTON2) <> IDYES then
    Exit;

  // Garde-fou 2 : seconde confirmation, qui nomme ce qui va disparaitre.
  if MsgBox('Dernière confirmation.' + #13#10 + #13#10
            + 'Vont être supprimés définitivement : vos instances et leurs '
            + 'mods, vos mondes et leurs sauvegardes, vos comptes enregistrés '
            + 'et tous vos réglages.' + #13#10 + #13#10
            + 'Il n''y a aucun moyen de revenir en arrière.' + #13#10 + #13#10
            + 'Confirmer la suppression ?',
            mbError, MB_YESNO or MB_DEFBUTTON2) <> IDYES then
  begin
    MsgBox('Vos données ont été conservées.', mbInformation, MB_OK);
    Exit;
  end;

  DelTree(DataDir, True, True, True);
end;
