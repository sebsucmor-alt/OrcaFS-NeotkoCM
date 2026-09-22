// NEOTKO_NEOSTROKE_TAG s335 — la ventana de "NeoStroke — Advanced".
//
// Hasta s334 los quince mandos avanzados vivían en fila dentro de la pestaña Quality, debajo de los
// básicos. Eran una pared de campos que no se lee y que además empuja el resto de la página hacia
// abajo. Ahora la pestaña deja SÓLO un botón y todo lo avanzado vive aquí:
//
//   ┌─ NeoStrokeAdvancedDialog ──────────────────────────────────────────┐
//   │  ┌─ mandos (scroll) ───────────┐  ┌─ visor de caminos ──────────┐  │
//   │  │  NeoStroke — Advanced       │  │  NeoArachnePreviewPanel     │  │
//   │  │   · cordón, anchos, overlap │  │  en modo Gate::NeoStroke    │  │
//   │  │   · patín, jitter…          │  │                             │  │
//   │  │  Classic (experimental)     │  │  (el mismo lienzo que tenía │  │
//   │  │   · outer/inner line width  │  │   NeoArachne en la pestaña) │  │
//   │  └─────────────────────────────┘  └─────────────────────────────┘  │
//   └────────────────────────────────────────────────────────────────────┘
//
// 🔑 El optgroup apunta al `m_config` VIVO de la pestaña, no a una copia: así un cambio aquí es el
//    mismo cambio que si se hubiera tecleado en la pestaña — se marca sucio, dispara `toggle_line`
//    y las flechitas de "modificado" salen solas. No hay Aceptar/Cancelar que valga ni copia que
//    sincronizar, que es justo donde se pierden los valores (ver s335 punto 2).
// 🚨 Por eso el diálogo replica los cinco callbacks que `Page::new_optgroup` le pone a un optgroup
//    de pestaña (`m_on_change`, `m_get_initial_config`, `m_get_sys_config`, `have_sys_config`). Sin
//    `m_get_sys_config`/`have_sys_config` el grupo no sabe comparar contra el preset del sistema y
//    las flechitas naranjas de "modificado" no aparecen.
#ifndef slic3r_GUI_NeoStrokeAdvancedDialog_hpp_
#define slic3r_GUI_NeoStrokeAdvancedDialog_hpp_

#include "GUI_Utils.hpp"

#include <memory>
#include <vector>

namespace Slic3r { namespace GUI {

class Tab;
class ConfigOptionsGroup;
class NeoArachnePreviewPanel;

using ConfigOptionsGroupShp = std::shared_ptr<ConfigOptionsGroup>;

class NeoStrokeAdvancedDialog : public DPIDialog
{
public:
    explicit NeoStrokeAdvancedDialog(wxWindow* parent, Tab* tab);
    ~NeoStrokeAdvancedDialog() override;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    // Monta un grupo y le engancha los callbacks de pestaña. `keys` son claves de `PrintConfig`.
    ConfigOptionsGroupShp make_group(wxWindow* parent, const wxString& title,
                                     const std::vector<std::string>& keys);

    Tab*                               m_tab = nullptr;
    std::vector<ConfigOptionsGroupShp> m_groups;
    NeoArachnePreviewPanel*            m_preview = nullptr;
};

}} // namespace Slic3r::GUI

#endif
