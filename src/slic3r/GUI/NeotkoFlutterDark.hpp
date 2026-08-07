#ifndef slic3r_GUI_NeotkoFlutterDark_hpp_
#define slic3r_GUI_NeotkoFlutterDark_hpp_

#include <string>

namespace Slic3r { namespace GUI {

// NEOTKO_FLUTTERDARK_TAG s252 — provisional dark mode for Snapmaker's Home/Device pages.
//
// Those pages are not ours: they are a compiled Flutter app (resources/web/flutter_web,
// auto-updated from Snapmaker's server) drawn on a canvas, so there is no CSS to restyle
// and no theme to switch — their own ThemeVM is short-circuited to light, and the dark
// screens simply are not in the web build. Verified in s252: neither their theme
// preferences, nor the OS dark setting, nor spoofing an iPhone user agent changes a pixel.
//
// What DOES work: that bundle carries its palette as ~224 constants of the literal form
//     new A.F(<ARGB as decimal int>)
// and we are the ones serving the file. So we rewrite those integers on the way out: the
// app still believes it is painting its usual white and a dark color comes out. Because we
// only touch palette constants, photos, the printer webcam and filament spool images pass
// through untouched — which is exactly what a blanket CSS invert filter got wrong.
//
// Hooked into HttpServer::ResponseFile::write_response, the single point every served file
// goes through, so it covers Home, Device and the other Flutter pages at once.
//
// PROVISIONAL: to be dropped the day Snapmaker ships their own dark mode. It patches a
// third-party artifact, so it is deliberately written to fail safe — if the palette is not
// recognized (new Flutter version, different Color representation, WASM build), nothing is
// modified and the page stays light. Never break their app to tint it.
//
// Known limitation: a single white serves as both page background and as label on colored
// buttons, and a value-level remap cannot tell the two apart, so "+ NewProject" ends up with
// dark text on blue. Keeping pure white untouched was tried and is worse — the sidebar then
// stays white with light-grey text on it (unreadable).
class NeotkoFlutterDark
{
public:
    // Cached copy of the app's dark flag. GUI_App::dark_mode() reaches into AppKit on macOS
    // and into app_config elsewhere, and the HTTP server answers on its own thread, so the
    // GUI thread pushes the value here instead of the server pulling it. Called from
    // WebView::CreateWebView (startup) and WebView::RecreateAll (theme change).
    static void set_dark(bool dark);
    static bool is_dark();

    // Rewrites the palette of Snapmaker's Flutter bundle in `content`, in place.
    // No-op (returns false, `content` untouched) unless dark mode is on, the file is the
    // bundle, and the palette is recognized. Results are cached: the bundle is ~5 MB and is
    // re-requested on every page load.
    static bool maybe_patch(const std::string& file_path, std::string& content);
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_NeotkoFlutterDark_hpp_
