; Papertrail's Windows installer. build-package.sh runs this with makensis, defining:
;   VERSION   the version, such as 1.0.0
;   SOURCE    the folder holding the program as it's installed
;   OUTFILE   where the installer goes
;   FILES / UNFILES   lists of what's installed, which the uninstaller removes one by one
;
; It asks whether to install just for this user (no administrator rights, the default) or for everyone.
; Uninstalling removes only what was installed, never the user's library, notes or highlights, which live in
; their own AppData folder.

Unicode true
SetCompressor /SOLID lzma

!define APPNAME "Papertrail"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Papertrail"

!define MULTIUSER_EXECUTIONLEVEL Highest
!define MULTIUSER_MUI
!define MULTIUSER_INSTALLMODE_COMMANDLINE
!define MULTIUSER_INSTALLMODE_DEFAULT_CURRENTUSER
!define MULTIUSER_USE_PROGRAMFILES64
!define MULTIUSER_INSTALLMODE_INSTDIR "Papertrail"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_KEY "Software\Papertrail"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_VALUENAME "InstallLocation"
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_KEY "Software\Papertrail"
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_VALUENAME "InstallMode"
!include MultiUser.nsh
!include MUI2.nsh
!include x64.nsh

Name "${APPNAME}"
OutFile "${OUTFILE}"
BrandingText "${APPNAME} ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "LegalCopyright" "GNU Affero General Public License v3.0 or later"

!define MUI_ICON "papertrail.ico"
!define MUI_UNICON "papertrail.ico"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TEXT "This installs ${APPNAME} ${VERSION}, a library manager and reader for academic publications.$\r$\n$\r$\n${APPNAME} indexes your papers and books where they already are, and never changes them.$\r$\n$\r$\nClick Next to continue."
!insertmacro MUI_PAGE_WELCOME
!define MUI_LICENSEPAGE_TEXT_TOP "${APPNAME} is free software. You don't need to accept anything to use it."
!define MUI_LICENSEPAGE_BUTTON "Next >"
!define MUI_LICENSEPAGE_TEXT_BOTTOM "The licence gives you the right to use, study, share and change ${APPNAME}. The source code is at https://github.com/hachigodesu/Papertrail."
!insertmacro MUI_PAGE_LICENSE "${SOURCE}\licenses\LICENSE.txt"
!insertmacro MULTIUSER_PAGE_INSTALLMODE
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\papertrail.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open ${APPNAME}"
!insertmacro MUI_PAGE_FINISH

!define MUI_UNCONFIRMPAGE_TEXT_TOP "${APPNAME} will be removed from this computer. Your library, notes, highlights and backups are kept, and so are your documents, so nothing is lost if you install it again."
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP "${APPNAME} needs 64-bit Windows 10 or newer."
        Abort
    ${EndIf}
    !insertmacro MULTIUSER_INIT
    ${If} $MultiUser.InstallMode == "AllUsers"
        SetRegView 64
    ${EndIf}
FunctionEnd

Function un.onInit
    !insertmacro MULTIUSER_UNINIT
    ${If} $MultiUser.InstallMode == "AllUsers"
        SetRegView 64
    ${EndIf}
FunctionEnd

Section "${APPNAME}" SecApp
    SectionIn RO
    ; An earlier version's files are replaced, and any it had that this one doesn't are left for its own uninstaller list.
    !include "${FILES}"
    WriteUninstaller "$INSTDIR\uninstall.exe"

    CreateShortCut "$SMPROGRAMS\${APPNAME}.lnk" "$INSTDIR\papertrail.exe"

    WriteRegStr SHCTX "Software\Papertrail" "InstallLocation" "$INSTDIR"
    WriteRegStr SHCTX "Software\Papertrail" "InstallMode" "$MultiUser.InstallMode"
    WriteRegStr SHCTX "${UNINSTALL_KEY}" "DisplayName" "${APPNAME}"
    WriteRegStr SHCTX "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr SHCTX "${UNINSTALL_KEY}" "Publisher" "Papertrail contributors"
    WriteRegStr SHCTX "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\papertrail.exe"
    WriteRegStr SHCTX "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr SHCTX "${UNINSTALL_KEY}" "URLInfoAbout" "https://github.com/hachigodesu/Papertrail"
    WriteRegStr SHCTX "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe" /$MultiUser.InstallMode'
    WriteRegStr SHCTX "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /$MultiUser.InstallMode /S'
    WriteRegDWORD SHCTX "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD SHCTX "${UNINSTALL_KEY}" "NoRepair" 1
    WriteRegDWORD SHCTX "${UNINSTALL_KEY}" "EstimatedSize" ${SIZE_KB}
SectionEnd

Section /o "Desktop shortcut" SecDesktop
    CreateShortCut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\papertrail.exe"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecApp} "The program itself."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A shortcut on your desktop. There's always one in the Start menu."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
    ; Only what was installed, by name, so a folder chosen by mistake, such as C:\, loses nothing else.
    !include "${UNFILES}"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"

    Delete "$SMPROGRAMS\${APPNAME}.lnk"
    Delete "$DESKTOP\${APPNAME}.lnk"
    DeleteRegKey SHCTX "${UNINSTALL_KEY}"
    DeleteRegKey SHCTX "Software\Papertrail"
SectionEnd
