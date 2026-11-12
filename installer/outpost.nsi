; Outpost installer (per-user, no admin needed). Built by CI with makensis.
; Silent install: Outpost-Setup.exe /S      Silent uninstall: uninstall.exe /S
Unicode true
!define VERSION "1.0.2"
!define APP "Outpost"
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\Outpost"
Name "${APP} ${VERSION}"
OutFile "..\dist\Outpost-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\Outpost"
InstallDirRegKey HKCU "${UNINST}" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
Icon "..\res\outpost.ico"
UninstallIcon "..\res\outpost.ico"
BrandingText "larosa023"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Outpost"
VIAddVersionKey "FileDescription" "Outpost installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "larosa023"
VIAddVersionKey "LegalCopyright" "larosa023"

!include "MUI2.nsh"
!define MUI_ICON "..\res\outpost.ico"
!define MUI_UNICON "..\res\outpost.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\Outpost.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open Outpost"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Outpost"
  SetOutPath "$INSTDIR"
  ; close the window if it's open (servers keep running in their hosts)
  IfFileExists "$INSTDIR\Outpost.exe" 0 +3
    ExecWait '"$INSTDIR\Outpost.exe" --quit'
    Sleep 800
  File "..\dist\Outpost.exe"
  File "..\README.md"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\Outpost.lnk" "$INSTDIR\Outpost.exe"
  WriteRegStr HKCU "${UNINST}" "DisplayName" "Outpost"
  WriteRegStr HKCU "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST}" "Publisher" "larosa023"
  WriteRegStr HKCU "${UNINST}" "DisplayIcon" "$INSTDIR\Outpost.exe"
  WriteRegStr HKCU "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINST}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegStr HKCU "${UNINST}" "URLInfoAbout" "https://github.com/larosa023/Outpost"
  WriteRegDWORD HKCU "${UNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  ; stop the servers properly (saves the worlds) and close the window
  ExecWait '"$INSTDIR\Outpost.exe" --cli stop-all'
  ExecWait '"$INSTDIR\Outpost.exe" --quit'
  Sleep 800
  Delete "$INSTDIR\Outpost.exe"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Outpost.lnk"
  DeleteRegKey HKCU "${UNINST}"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "Outpost"
  IfSilent done
  MessageBox MB_YESNO|MB_ICONQUESTION "Also delete your servers, worlds and backups?$\r$\n($LOCALAPPDATA\Outpost)" /SD IDNO IDNO done
    RMDir /r "$LOCALAPPDATA\Outpost"
  done:
SectionEnd
