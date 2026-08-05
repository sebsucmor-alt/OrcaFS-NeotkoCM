; [1] PACK_SOURCE_DIR = compile-time only (e.g. .\build\Snapmaker_Orca). [2] INSTALL_DIR_RUNTIME = runtime install dir (default .\ = $EXEDIR).
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!define PRODUCT_NAME "SnapMaker-NeotkoCM"
!define PRODUCT_PUBLISHER "Neotko"
!define PRODUCT_WEB_SITE "https://github.com/Snapmaker/OrcaSlicer"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
!define PRODUCT_UNINST_ROOT_KEY "HKLM"
!define PRODUCT_INSTALL_KEY "Software\${PRODUCT_PUBLISHER}\${PRODUCT_NAME}"

!ifndef VERSION
    !define VERSION "2.3.5"
!endif

!ifndef SOURCE_DIR
    !define SOURCE_DIR ".\build\Snapmaker_Orca"
!endif
!define PACK_SOURCE_DIR "${SOURCE_DIR}"

; 64-bit app: use PROGRAMFILES64 so default path is C:\Program Files\SnapMaker-NeotkoCM, not (x86)
!define INSTALL_DIR_RUNTIME "$PROGRAMFILES64\SnapMaker-NeotkoCM"
InstallDir "${INSTALL_DIR_RUNTIME}"

!ifndef OUTPUT_FILE
    !define OUTPUT_FILE "SnapMaker-NeotkoCM_Windows_Installer_V${VERSION}.exe"
!endif

; License page: show LICENSE.txt from repo root (same dir as this .nsi)
!ifndef LICENSE_FILE
    !define LICENSE_FILE ".\LICENSE.txt"
!endif

RequestExecutionLevel admin

; No /SOLID to avoid "Internal compiler error #12345: error mmapping datablock"
SetCompressor lzma

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey "Comments" "Snapmaker Orca is an open source slicer for FDM printers"
VIAddVersionKey "CompanyName" "${PRODUCT_PUBLISHER}"
VIAddVersionKey "LegalCopyright" "Copyright (C) ${PRODUCT_PUBLISHER}"
VIAddVersionKey "FileDescription" "${PRODUCT_NAME} ${VERSION} Installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "InternalName" "${PRODUCT_NAME}"
VIAddVersionKey "LegalTrademarks" ""
VIAddVersionKey "OriginalFilename" "${OUTPUT_FILE}"

; Installer and uninstaller icon: set by build_and_pack.bat via /DICON_FILE=path (e.g. Snapmaker_Orca.ico or snapmaker.ico)
!ifdef ICON_FILE
    !define MUI_ICON "${ICON_FILE}"
    !define MUI_UNICON "${ICON_FILE}"
!else
    !define MUI_ICON ".\resources\images\Snapmaker_Orca.ico"
    !define MUI_UNICON ".\resources\images\Snapmaker_Orca.ico"
!endif

!define MUI_WELCOMEPAGE_TITLE "Welcome to ${PRODUCT_NAME} Setup"
!define MUI_WELCOMEPAGE_TEXT "This wizard will guide you through the installation of ${PRODUCT_NAME} ${VERSION}.$\r$\n$\r$\nClick Next to continue."
!insertmacro MUI_PAGE_WELCOME

!ifdef LICENSE_FILE
    !define MUI_LICENSEPAGE_CHECKBOX
    !insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!endif

!insertmacro MUI_PAGE_COMPONENTS

!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose the folder in which to install ${PRODUCT_NAME}."
!insertmacro MUI_PAGE_DIRECTORY

!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Run ${PRODUCT_NAME}"
!define MUI_FINISHPAGE_RUN_FUNCTION "LaunchApp"
!define MUI_FINISHPAGE_LINK "Visit ${PRODUCT_NAME} website"
!define MUI_FINISHPAGE_LINK_LOCATION "${PRODUCT_WEB_SITE}"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Name "${PRODUCT_NAME} ${VERSION}"
OutFile "${OUTPUT_FILE}"

Section "Main program" SecMain
    SectionIn RO

    Call EnsureSnapmakerNotRunning

    SetOutPath "$INSTDIR"
    
    DetailPrint "Installing ${PRODUCT_NAME}..."
    DetailPrint "Target dir: $INSTDIR"
    
    DetailPrint "Copying files..."
    
    ; PACK_SOURCE_DIR = compile time only. At runtime this File extracts from embedded payload to $INSTDIR. Exclude include and lib dirs.
    File /r /x "*.pdb" /x "*.ilk" /x "*.exp" /x "*.lib" /x "*.obj" /x "*.idb" /x "*.tlog" /x "*.h" /x "*.hpp" /x "*.c" /x "*.cpp" /x "*.cxx" /x "*.cc" /x "*.vcxproj" /x "*.vcxproj.filters" /x "*.sln" /x "*.cmake" /x "*.py" /x "*.md" /x "*.vcxproj.user" /x "CMakeFiles" /x "RelWithDebInfo" /x "Debug" /x "MinSizeRel" /x ".vs" /x "vcpkg_installed" /x "*.dir" /x "include\*" /x "lib\*" "${PACK_SOURCE_DIR}\*.*"
    
    IfFileExists "$INSTDIR\snapmaker-orca.exe" 0 extract_error
    
    DetailPrint "Creating uninstaller..."
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    
    DetailPrint "Writing registry..."
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\Uninstall.exe"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\snapmaker-orca.exe"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "HelpLink" "${PRODUCT_WEB_SITE}"
    WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "NoModify" 1
    WriteRegDWORD ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "NoRepair" 1

    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_INSTALL_KEY}" "Version" "${VERSION}"
    WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_INSTALL_KEY}" "InstallPath" "$INSTDIR"

    ; URL protocols. These only ever carry "open this model in the slicer" links from web
    ; sites — nothing to do with login or the Snapmaker cloud (see is_orca_open() in
    ; libslic3r/Utils.hpp), so it is safe for the two apps not to fight over them.
    ;
    ; neotkocm:// is ours and is always registered. snapmaker-orca:// and Snapmaker_Orca://
    ; are SHARED with the official Snapmaker Orca: we claim them only if nobody has, so a
    ; user with just our build still gets working web links, and a user who also runs the
    ; official app keeps theirs pointing where they expect.
    DetailPrint "Registering URL protocols..."
    SetRegView 64

    WriteRegStr HKLM "Software\Classes\neotkocm" "" "URL:${PRODUCT_NAME}"
    WriteRegStr HKLM "Software\Classes\neotkocm" "URL Protocol" ""
    WriteRegStr HKLM "Software\Classes\neotkocm\shell\open\command" "" '"$INSTDIR\snapmaker-orca.exe" "%1"'

    ReadRegStr $R2 HKLM "Software\Classes\snapmaker-orca\shell\open\command" ""
    StrCmp $R2 "" 0 skip_scheme_lower
        DetailPrint "Claiming unowned protocol snapmaker-orca://"
        WriteRegStr HKLM "Software\Classes\snapmaker-orca" "" "URL:Snapmaker Orca"
        WriteRegStr HKLM "Software\Classes\snapmaker-orca" "URL Protocol" ""
        WriteRegStr HKLM "Software\Classes\snapmaker-orca\shell\open\command" "" '"$INSTDIR\snapmaker-orca.exe" "%1"'
        Goto done_scheme_lower
    skip_scheme_lower:
        DetailPrint "Protocol snapmaker-orca:// belongs to another install - leaving it alone"
    done_scheme_lower:

    ReadRegStr $R2 HKLM "Software\Classes\Snapmaker_Orca\shell\open\command" ""
    StrCmp $R2 "" 0 skip_scheme_upper
        DetailPrint "Claiming unowned protocol Snapmaker_Orca://"
        WriteRegStr HKLM "Software\Classes\Snapmaker_Orca" "" "URL:Snapmaker Orca"
        WriteRegStr HKLM "Software\Classes\Snapmaker_Orca" "URL Protocol" ""
        WriteRegStr HKLM "Software\Classes\Snapmaker_Orca\shell\open\command" "" '"$INSTDIR\snapmaker-orca.exe" "%1"'
        Goto done_scheme_upper
    skip_scheme_upper:
        DetailPrint "Protocol Snapmaker_Orca:// belongs to another install - leaving it alone"
    done_scheme_upper:

    SetRegView 32

    DetailPrint "Installation complete!"
    Goto end_section
    
    extract_error:
        MessageBox MB_OK|MB_ICONSTOP "Installation failed: snapmaker-orca.exe was not found in the package. The installer may be corrupted."
        Abort
    
    end_section:
SectionEnd

Section "Desktop shortcut" SecDesktop
    DetailPrint "Creating desktop shortcut..."
    SetShellVarContext current
    CreateShortcut "$DESKTOP\${PRODUCT_NAME}.lnk" "$INSTDIR\snapmaker-orca.exe" "" "$INSTDIR\snapmaker-orca.exe" 0
    SetShellVarContext all
SectionEnd

Section "Start menu shortcut" SecStartMenu
    DetailPrint "Creating start menu shortcut..."
    CreateDirectory "$SMPROGRAMS\${PRODUCT_NAME}"
    CreateShortcut "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk" "$INSTDIR\snapmaker-orca.exe" "" "$INSTDIR\snapmaker-orca.exe" 0
    CreateShortcut "$SMPROGRAMS\${PRODUCT_NAME}\Uninstall.lnk" "$INSTDIR\Uninstall.exe" "" "$INSTDIR\Uninstall.exe" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "Install ${PRODUCT_NAME} and all required files."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Create a desktop shortcut for ${PRODUCT_NAME}."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "Create a start menu shortcut for ${PRODUCT_NAME}."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"

    DetailPrint "Uninstalling ${PRODUCT_NAME}..."
    
    ; Do NOT taskkill by image name: the official Snapmaker Orca ships the same
    ; snapmaker-orca.exe filename, so /IM would kill THEIR running app too. Test the
    ; exe in $INSTDIR for a write lock instead — that only ever matches our install.
    DetailPrint "Checking for running processes..."
    Call un.EnsureSnapmakerNotRunning

    ; Only our own shortcut. "Snapmaker Orca.lnk" belongs to the official install and
    ; must never be deleted by us.
    DetailPrint "Removing desktop shortcut..."
    SetShellVarContext current
    Delete "$DESKTOP\${PRODUCT_NAME}.lnk"
    SetShellVarContext all
    
    DetailPrint "Removing start menu shortcut..."
    RMDir /r "$SMPROGRAMS\${PRODUCT_NAME}"
    
    DetailPrint "Removing install directory..."
    RMDir /r /REBOOTOK "$INSTDIR"
    
    RMDir "$INSTDIR"
    
    ; The snapmaker-orca:// and Snapmaker_Orca:// protocol keys are SHARED with the
    ; official Snapmaker Orca. Deleting them unconditionally broke deep links for users
    ; who kept the official app installed. Only remove a key if it still points at OUR
    ; exe; if the official install (or a reinstall of it) owns it, leave it alone.
    DetailPrint "Removing registry entries..."
    SetRegView 64
    DeleteRegKey HKLM "Software\Classes\neotkocm"
    ReadRegStr $R1 HKLM "Software\Classes\snapmaker-orca\shell\open\command" ""
    StrCmp $R1 '"$INSTDIR\snapmaker-orca.exe" "%1"' 0 keep_scheme_lower
        DeleteRegKey HKLM "Software\Classes\snapmaker-orca"
    keep_scheme_lower:
    ReadRegStr $R1 HKLM "Software\Classes\Snapmaker_Orca\shell\open\command" ""
    StrCmp $R1 '"$INSTDIR\snapmaker-orca.exe" "%1"' 0 keep_scheme_upper
        DeleteRegKey HKLM "Software\Classes\Snapmaker_Orca"
    keep_scheme_upper:
    SetRegView 32
    DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}"
    DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_INSTALL_KEY}"
    DeleteRegKey HKCU "${PRODUCT_INSTALL_KEY}"
    
    DetailPrint "Uninstall complete!"
SectionEnd

Function LaunchApp
    ExecShell "open" "$INSTDIR\snapmaker-orca.exe"
FunctionEnd

; Prevent overwriting locked files when THIS install's own snapmaker-orca.exe is running.
; We test the exe in $INSTDIR for a write lock rather than matching by process name:
; the official Snapmaker Orca shares the exe name but lives in another folder, so it never
; locks our file and must not trigger this prompt. If $INSTDIR\snapmaker-orca.exe cannot be
; opened for writing, a running instance is holding it; if it doesn't exist yet (fresh install)
; or opens fine, we are clear to proceed.
Function EnsureSnapmakerNotRunning
    snapmaker_check_loop:
        IfFileExists "$INSTDIR\snapmaker-orca.exe" 0 snapmaker_idle
        ClearErrors
        FileOpen $0 "$INSTDIR\snapmaker-orca.exe" a
        IfErrors snapmaker_in_use
        FileClose $0
        Goto snapmaker_idle
    snapmaker_in_use:
        IfSilent snapmaker_silent snapmaker_prompt
    snapmaker_silent:
        SetErrorLevel 7
        Quit
    snapmaker_prompt:
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "SnapMaker-NeotkoCM is still running.$\r$\nClose the program, then click Retry, or Cancel to exit the installer." IDRETRY snapmaker_check_loop
        Abort
    snapmaker_idle:
FunctionEnd

; Uninstaller copy of the above. NSIS keeps installer and uninstaller code separate, so
; an uninstaller-side function must be declared with the "un." prefix and its own labels.
Function un.EnsureSnapmakerNotRunning
    un_snapmaker_check_loop:
        IfFileExists "$INSTDIR\snapmaker-orca.exe" 0 un_snapmaker_idle
        ClearErrors
        FileOpen $0 "$INSTDIR\snapmaker-orca.exe" a
        IfErrors un_snapmaker_in_use
        FileClose $0
        Goto un_snapmaker_idle
    un_snapmaker_in_use:
        IfSilent un_snapmaker_silent un_snapmaker_prompt
    un_snapmaker_silent:
        SetErrorLevel 7
        Quit
    un_snapmaker_prompt:
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "SnapMaker-NeotkoCM is still running.$\r$\nClose the program, then click Retry, or Cancel to exit the uninstaller." IDRETRY un_snapmaker_check_loop
        Abort
    un_snapmaker_idle:
FunctionEnd

Function .onInit

    Call EnsureSnapmakerNotRunning

    ReadRegStr $R0 ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString"
    StrCmp $R0 "" done

    MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION \
    "${PRODUCT_NAME} is already installed.$\n$\nClick OK to uninstall the old version, or Cancel to abort." \
    IDOK uninst
    Abort
    
    uninst:
        ClearErrors
        ExecWait '$R0 _?=$INSTDIR'
        
        IfErrors no_remove_uninstaller done
        no_remove_uninstaller:
    
    done:
FunctionEnd
