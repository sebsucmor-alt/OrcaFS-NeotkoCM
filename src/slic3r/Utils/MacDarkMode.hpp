#ifndef slic3r_MacDarkMode_hpp_
#define slic3r_MacDarkMode_hpp_

#include <wx/event.h>

namespace Slic3r {
namespace GUI {

#if __APPLE__
extern bool mac_dark_mode();
extern double mac_max_scaling_factor();
extern void set_miniaturizable(void * window);
void WKWebView_evaluateJavaScript(void * web, wxString const & script, void (*callback)(wxString const &));
void WKWebView_setTransparentBackground(void * web);
void set_tag_when_enter_full_screen(bool isfullscreen);
void set_title_colour_after_set_title(void * window);
void initGestures(void * view,  wxEvtHandler * handler);
void openFolderForFile(wxString const & file);
// NEOTKO_PRESENT_TAG 4-nov — pruebas de presentación del 3D (fotogramas que no llegan a pantalla).
void mac_gl_set_swap_interval(int interval);   // sobre el contexto CGL actual
void mac_ca_transaction_flush();               // [CATransaction flush]
#endif


} // namespace GUI
} // namespace Slic3r

#endif // MacDarkMode_h
