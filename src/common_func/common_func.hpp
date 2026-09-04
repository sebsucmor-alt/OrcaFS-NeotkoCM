#ifndef _common_func_hppp_
#define _common_func_hppp_
#include <iostream>


#define SLIC3R_APP_NAME "Snapmaker Orca"
// SLIC3R_APP_KEY stays "Snapmaker_Orca": it also names the gettext catalogs
// (resources/i18n/*/Snapmaker_Orca.mo), so renaming it would kill every translation.
#define SLIC3R_APP_KEY "Snapmaker_Orca"
// NEOTKO — the per-user data directory (%AppData%\<key> on Windows,
// ~/Library/Application Support/<key> on macOS) must NOT be shared with the official
// Snapmaker Orca: both apps would read and write the same config, presets and user
// profiles and silently overwrite each other. See migrate_legacy_data_dir() in GUI_App.cpp,
// which copies the old shared folder across on first run so nobody loses their presets.
#define SLIC3R_APP_DATA_KEY "SnapMaker-NeotkoCM"
#define SLIC3R_APP_LEGACY_DATA_KEY "Snapmaker_Orca"
#define SLIC3R_VERSION "01.10.01.50"
#define Snapmaker_VERSION "2.3.5"
// Upstream Snapmaker #775 (5417538a1c): las U1 con firmware < 1.6.0 ya no se soportan.
// Sólo lo consume AboutDialog.cpp para escribir el texto informativo; no cierra ninguna puerta.
#define MIN_FIRM_VER "1.6.0"
#ifndef GIT_COMMIT_HASH
#define GIT_COMMIT_HASH "0000000" // 0000000 means uninitialized
#endif
#define SLIC3R_BUILD_ID "2.3.5"
// #define SLIC3R_RC_VERSION "01.10.01.50"
#define BBL_RELEASE_TO_PUBLIC 1
#define BBL_INTERNAL_TESTING 0
#define ORCA_CHECK_GCODE_PLACEHOLDERS 0

namespace common 
{
	std::string get_pc_name();

	std::string get_flutter_version();

	std::string get_profile_version();

	std::string getMachineId();

	std::string getLocalArea();

	std::string getLanguage();

    } // namespace common

#endif