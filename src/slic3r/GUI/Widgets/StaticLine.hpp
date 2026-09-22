#ifndef slic3r_GUI_StaticLine_hpp_
#define slic3r_GUI_StaticLine_hpp_

#include "../wxExtensions.hpp"
#include "wx/window.h"

#include <functional>

class StaticLine : public wxWindow
{
public:
    StaticLine(wxWindow *parent, bool vertical = false, const wxString &label = {}, const wxString &icon = {});

public:
    void SetLabel(const wxString& label) override;

    void SetIcon(const wxString& icon);

    void SetLineColour(wxColour color);
    
    void Rescale();

    // NeotkoLIBRE_FOLD s330 - la cabecera hace de boton de plegado.
    void SetFoldable(bool foldable);
    void SetFolded(bool folded);
    void SetModifiedMark(bool modified);
    bool IsFoldable() const { return m_foldable; }
    std::function<void(bool alt_down)> on_toggle_fold { nullptr };

private:
    wxColour       lineColor;
    bool vertical;
    ScalableBitmap icon;

private:
    void paintEvent(wxPaintEvent& evt);

    // NeotkoLIBRE_FOLD s330
    void mouseUp(wxMouseEvent& evt);
    bool m_foldable {false};
    bool m_folded {false};
    bool m_modified {false};

    void messureSize();

    void render(wxDC &dc);

    DECLARE_EVENT_TABLE()
};

#endif // !slic3r_GUI_StaticLine_hpp_
