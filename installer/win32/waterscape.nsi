; ============================================================================
; Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
; installer/win32/waterscape.nsi - Alternative NSIS 3 Scriptable Windows Recipe
;
; Usage:
;   makensis -DBUILD_DIR=..\..\build-win64 -DBRAND_DIR=..\..\build-win64\brand waterscape.nsi
; ============================================================================

Unicode True
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef APP_NAME
  !define APP_NAME "Waterscape Browser"
!endif
!ifndef APP_VERSION
  !define APP_VERSION "2.0.0"
!endif
!ifndef APP_PUBLISHER
  !define APP_PUBLISHER "Arcradient Project"
!endif
!ifndef BUILD_DIR
  !define BUILD_DIR "..\..\build-win64"
!endif
!ifndef BRAND_DIR
  !define BRAND_DIR "${BUILD_DIR}\brand"
!endif
!ifndef OUT_INSTALLER
  !define OUT_INSTALLER "${BUILD_DIR}\Waterscape-Setup-${APP_VERSION}.exe"
!endif

Name "${APP_NAME} ${APP_VERSION}"
OutFile "${OUT_INSTALLER}"
InstallDir "$LOCALAPPDATA\Programs\Waterscape"
InstallDirRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Waterscape" "InstallLocation"
RequestExecutionLevel user
BrandingText "Waterscape ${APP_VERSION} · Arcradient Pure C11 Engine"

!if /FileExists "${BRAND_DIR}\waterscape.ico"
  !define MUI_ICON "${BRAND_DIR}\waterscape.ico"
  !define MUI_UNICON "${BRAND_DIR}\waterscape.ico"
!endif
!if /FileExists "${BRAND_DIR}\wizard_banner.bmp"
  !define MUI_HEADERIMAGE
  !define MUI_HEADERIMAGE_BITMAP "${BRAND_DIR}\wizard_banner.bmp"
!endif

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\waterscape.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch Waterscape Browser now"

; Wizard Pages
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

; Uninstaller Pages
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ----------------------------------------------------------------------------
; Installer Sections
; ----------------------------------------------------------------------------
Section "Waterscape Core & Hardware Acceleration" SecCore
  SectionIn RO
  SetOutPath "$INSTDIR"

  !if /FileExists "${BUILD_DIR}\waterscape.exe"
    File "${BUILD_DIR}\waterscape.exe"
  !else
    File /oname=waterscape.exe "${BUILD_DIR}\arcradient.exe"
  !endif

  File /nonfatal "${BUILD_DIR}\arcr_*.dll"
  File /nonfatal "${BUILD_DIR}\ws_plugin_*.dll"
  File /nonfatal "${BRAND_DIR}\waterscape.ico"

  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Standard HKCU Add/Remove Programs Metadata
  WriteRegStr   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Waterscape" "DisplayName" "Waterscape Web Browser"
  WriteRegStr   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Waterscape" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Waterscape" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Waterscape" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Waterscape" "DisplayIcon" "$INSTDIR\waterscape.exe,0"
  WriteRegStr   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Waterscape" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Waterscape" "QuietUninstallString" "$\"$INSTDIR\uninstall.exe$\" /S"
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Waterscape" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Waterscape" "NoRepair" 1

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Waterscape" "EstimatedSize" "$0"
SectionEnd

Section "Official Extensions (Shield, Dark Mode, Reader)" SecAddons
  SetOutPath "$INSTDIR\addons"
  File /nonfatal "${BUILD_DIR}\addons\*.wsx"
  SetOutPath "$INSTDIR"
SectionEnd

Section "Desktop & Start Menu Shortcuts" SecShortcuts
  CreateDirectory "$SMPROGRAMS\Waterscape"
  CreateShortcut "$SMPROGRAMS\Waterscape\Waterscape.lnk" "$INSTDIR\waterscape.exe" "" "$INSTDIR\waterscape.exe" 0
  CreateShortcut "$DESKTOP\Waterscape.lnk" "$INSTDIR\waterscape.exe" "" "$INSTDIR\waterscape.exe" 0
SectionEnd

Section /o "Register Waterscape for HTML/HTTP/HTTPS" SecAssoc
  WriteRegStr HKCU "Software\Clients\StartMenuInternet\Waterscape" "" "Waterscape Web Browser"
  WriteRegStr HKCU "Software\Clients\StartMenuInternet\Waterscape\DefaultIcon" "" "$INSTDIR\waterscape.exe,0"
  WriteRegStr HKCU "Software\Clients\StartMenuInternet\Waterscape\shell\open\command" "" "$\"$INSTDIR\waterscape.exe$\" $\"%1$\""
  WriteRegStr HKCU "Software\Clients\StartMenuInternet\Waterscape\Capabilities" "ApplicationName" "Waterscape Web Browser"
  WriteRegStr HKCU "Software\Clients\StartMenuInternet\Waterscape\Capabilities" "ApplicationDescription" "Lightweight hardware-accelerated Pure C11 web browser"
  WriteRegStr HKCU "Software\Clients\StartMenuInternet\Waterscape\Capabilities\FileAssociations" ".htm" "WaterscapeHTML"
  WriteRegStr HKCU "Software\Clients\StartMenuInternet\Waterscape\Capabilities\FileAssociations" ".html" "WaterscapeHTML"
  WriteRegStr HKCU "Software\Clients\StartMenuInternet\Waterscape\Capabilities\URLAssociations" "http" "WaterscapeHTML"
  WriteRegStr HKCU "Software\Clients\StartMenuInternet\Waterscape\Capabilities\URLAssociations" "https" "WaterscapeHTML"
  WriteRegStr HKCU "Software\RegisteredApplications" "Waterscape" "Software\Clients\StartMenuInternet\Waterscape\Capabilities"
  WriteRegStr HKCU "Software\Classes\WaterscapeHTML" "" "Waterscape HTML Document"
  WriteRegStr HKCU "Software\Classes\WaterscapeHTML\DefaultIcon" "" "$INSTDIR\waterscape.exe,0"
  WriteRegStr HKCU "Software\Classes\WaterscapeHTML\shell\open\command" "" "$\"$INSTDIR\waterscape.exe$\" $\"%1$\""
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} "Waterscape Browser executable and on-demand Pure C11 engine modules."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAddons} "Bundled Shield Ad-Blocker, Dark Theme, and Reader View (.wsx) extensions."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecShortcuts} "Create Desktop and Start Menu shortcuts for Waterscape."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAssoc} "Register Waterscape as an optional handler for HTML files and HTTP/HTTPS links."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ----------------------------------------------------------------------------
; Uninstaller Section
; ----------------------------------------------------------------------------
Section "Uninstall"
  Delete "$INSTDIR\waterscape.exe"
  Delete "$INSTDIR\arcradient.exe"
  Delete "$INSTDIR\waterscape.ico"
  Delete "$INSTDIR\install_manifest.txt"
  Delete "$INSTDIR\arcr_*.dll"
  Delete "$INSTDIR\ws_plugin_*.dll"
  Delete "$INSTDIR\addons\*.wsx"
  RMDir  "$INSTDIR\addons"

  Delete "$DESKTOP\Waterscape.lnk"
  Delete "$SMPROGRAMS\Waterscape\Waterscape.lnk"
  RMDir  "$SMPROGRAMS\Waterscape"

  DeleteRegKey   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Waterscape"
  DeleteRegKey   HKCU "Software\Clients\StartMenuInternet\Waterscape"
  DeleteRegKey   HKCU "Software\Classes\WaterscapeHTML"
  DeleteRegValue HKCU "Software\RegisteredApplications" "Waterscape"

  Delete "$INSTDIR\uninstall.exe"
  RMDir  "$INSTDIR"
SectionEnd
