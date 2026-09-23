// NEOTKO_NEOSTROKE_TAG s335 — ver la cabecera para el porqué de esta ventana.
#include "NeoStrokeAdvancedDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "OptionsGroup.hpp"
#include "Tab.hpp"
#include "NeoArachnePreviewPanel.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>

namespace Slic3r { namespace GUI {

// Los mandos avanzados (quince de s332 + tres de s336), en el MISMO orden en el que estaban en la pestaña (s332). El orden
// no es decorativo: va de lo que decide el cordón a lo que decide el camino, y así se lee.
// 🚨 `neostroke_wall_eat` y `neostroke_perimeter` NO están: se fueron con NeoWall en s335.
static const std::vector<std::string>& ns_advanced_keys()
{
    static const std::vector<std::string> keys = {
        "neostroke_bead_min_pct",       // el cordón más fino que el cabezal saca de verdad
        "neostroke_width_ref",          // la referencia de todos los %
        "neostroke_max_width_pct",
        "neostroke_detail_min_pct",
        "neostroke_max_stroke_width",
        "neostroke_corner_hooks",
        "neostroke_curve_overlap",      // 0 = curva de overlap apagada
        "neostroke_overlap_width_end",
        "neostroke_overlap_straight",
        "neostroke_overlap_turn_min",
        "neostroke_overlap_turn_max",
        "neostroke_overlap_span",
        "neostroke_layer_jitter",
        "neostroke_skate",
        "neostroke_skate_detour",
        // s336 (2_47) — pruebas: las tres mejoras de camino, apagadas = 2_46
        "neostroke_continuous_turns",
        "neostroke_offset_lines",
        "neostroke_variable_k",
    };
    return keys;
}

// NEOTKO_NEOSTROKE_TAG s335 — el ancho de muro de CLASSIC, aquí dentro y a propósito.
// NeoStroke planifica el interior sobre "la isla menos la banda que se come el muro", y esa banda
// es `ext_perimeter_flow.spacing()`, o sea `outer_wall_line_width`. Tocarlo mueve TODOS los caminos
// de golpe, y hasta hoy había que irse a otra página de la pestaña y laminar para ver el efecto.
// Con el visor al lado se ve al momento. Son las claves de siempre de Orca, no unas nuevas: lo que
// se toque aquí es exactamente lo que se habría tocado en "Quality → Line width".
static const std::vector<std::string>& ns_classic_width_keys()
{
    static const std::vector<std::string> keys = {
        "outer_wall_line_width",
        "inner_wall_line_width",
    };
    return keys;
}

ConfigOptionsGroupShp NeoStrokeAdvancedDialog::make_group(wxWindow* parent, const wxString& title,
                                                          const std::vector<std::string>& keys)
{
    // 🚨 `is_tab_opt = true`: es lo que hace que el grupo dibuje la columna de las flechitas
    //    (modificado / volver al valor del sistema). Con `false` sale un formulario pelado.
    auto optgroup = std::make_shared<ConfigOptionsGroup>(parent, title, m_tab->get_config(),
                                                         /*is_tab_opt=*/true);
    optgroup->set_config_category_and_type(title, m_tab->type());
    // 🚨 `label_width` se deja EN EL DEFECTO (20 em), el mismo que usa `Page::new_optgroup` en la
    //    pestaña. Es un ancho en EM, no en píxeles: con el em de una pantalla Retina son ya ~460 px.
    //    Subirlo "para que quepan las etiquetas largas" ensancha la columna y empuja los campos
    //    fuera de la ventana, que es lo contrario de lo que se busca.

    Tab* tab = m_tab;
    // Los MISMOS callbacks que `Page::new_optgroup` pone en la pestaña. Ver la cabecera.
    optgroup->m_on_change = [tab](t_config_option_key opt_key, boost::any value) {
        tab->update_dirty();
        tab->on_value_change(opt_key, value);
    };
    optgroup->m_get_initial_config = [tab]() {
        return tab->get_presets()->get_selected_preset().config;
    };
    optgroup->m_get_sys_config = [tab]() {
        return tab->get_presets()->get_selected_preset_parent()->config;
    };
    optgroup->have_sys_config = [tab]() {
        return tab->get_presets()->get_selected_preset_parent() != nullptr;
    };

    for (const std::string& key : keys)
        optgroup->append_single_option_line(key, "quality_settings_wall_generator#neostroke");

    optgroup->activate();
    // 🚨 El modo MÁS ALTO del fork (comSimple / comAdvanced / comDevelop — aquí NO hay `comExpert`).
    //    Los mandos de NeoStroke son `comAdvanced`, así que con el modo del usuario en Simple la
    //    ventana saldría vacía. Aquí dentro se enseña todo: es LA ventana avanzada.
    optgroup->update_visibility(comDevelop);
    optgroup->reload_config();
    return optgroup;
}

NeoStrokeAdvancedDialog::NeoStrokeAdvancedDialog(wxWindow* parent, Tab* tab)
    : DPIDialog(parent, wxID_ANY, _L("NeoStroke — Advanced"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_tab(tab)
{
    SetBackgroundColour(*wxWHITE);

    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* cols = new wxBoxSizer(wxHORIZONTAL);

    // ── columna izquierda: los mandos, con scroll ───────────────────────────
    // Con scroll porque son quince más los dos de Classic: en un portátil de 13" no caben, y una
    // ventana que no se puede encoger es peor que la pared de campos que esto viene a quitar.
    auto* left = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    left->SetScrollRate(0, 10);
    left->SetBackgroundColour(*wxWHITE);
    auto* left_sizer = new wxBoxSizer(wxVERTICAL);

    m_groups.push_back(make_group(left, _L("NeoStroke — Advanced"), ns_advanced_keys()));
    left_sizer->Add(m_groups.back()->sizer, 0, wxEXPAND | wxALL, 4);

    left_sizer->Add(new wxStaticLine(left), 0, wxEXPAND | wxTOP | wxBOTTOM, 6);

    {
        auto* note = new wxStaticText(left, wxID_ANY,
            _L("Experimental — Classic's wall width is what NeoStroke has to plan around. "
               "Narrow it and the interior gets more room; widen it and the strokes move inwards. "
               "These are Orca's own line width settings, shown here so the change and its effect "
               "on the paths are visible side by side."));
        note->Wrap(360);
        left_sizer->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    }

    m_groups.push_back(make_group(left, _L("Classic wall width (experimental)"), ns_classic_width_keys()));
    left_sizer->Add(m_groups.back()->sizer, 0, wxEXPAND | wxALL, 4);

    left->SetSizer(left_sizer);
    left->FitInside();
    // El ancho lo pide el CONTENIDO (etiqueta + campo del grupo), medido, no un número a ojo. Sólo
    // el alto se deja libre: para eso está el scroll.
    left->SetMinSize(wxSize(left_sizer->CalcMin().GetWidth(), -1));
    cols->Add(left, 0, wxEXPAND | wxALL, 6);

    // ── columna derecha: el visor de caminos ────────────────────────────────
    // Es el MISMO panel que pintaba Edge Closure en la pestaña, con la puerta cambiada. Lo que
    // dibuja son las entidades que deja `NeoArachne::Plan::run`, y ese `run` ya despacha a
    // NeoStroke cuando `wall_generator` lo dice: no hay un segundo motor que mantener.
    m_preview = new NeoArachnePreviewPanel(this, tab, NeoArachnePreviewPanel::Gate::NeoStroke);
    // Suelo del visor: por debajo de esto el lienzo no da para leer un camino. El resto del ancho
    // que sobre se lo queda él (proporción 1), porque la columna de mandos ya tiene el suyo fijo.
    m_preview->SetMinSize(wxSize(FromDIP(420), FromDIP(520)));
    cols->Add(m_preview, 1, wxEXPAND | wxALL, 6);

    root->Add(cols, 1, wxEXPAND);
    root->Add(CreateSeparatedButtonSizer(wxCLOSE), 0, wxEXPAND | wxALL, 8);

    // 🔑 No hay Aceptar/Cancelar: el optgroup escribe en el `m_config` vivo de la pestaña, así que
    //    cada tecla ya está aplicada. Cerrar es cerrar, no confirmar.
    SetEscapeId(wxID_CLOSE);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); }, wxID_CLOSE);

    SetSizer(root);
    // 🚨 `SetSizeHints` fija el mínimo A PARTIR DEL CONTENIDO. NADA de `SetMinSize` con números a
    //    ojo DESPUÉS: la columna de mandos mide ~460 px de etiqueta más el campo, así que un mínimo
    //    de 880 queda por debajo de lo que hace falta y el que paga es el visor, que se estruja.
    root->SetSizeHints(this);
    Fit();
    CentreOnParent();

    // 🚨 EL TEMA. Es lo que hace TODO diálogo de la casa (BedShapeDialog, AboutDialog…): fondo
    //    blanco arriba y, al final y con todos los hijos ya creados, `UpdateDlgDarkUI`, que recorre
    //    el árbol entero recoloreando etiquetas, botones y casillas. Sin esta llamada, en modo
    //    oscuro las etiquetas salen gris sobre gris y los botones del visor, vacíos.
    // 🚨 AL FINAL a propósito: recorre los hijos que EXISTEN al llamarla. Si se sube antes de crear
    //    el visor, la mitad derecha se queda sin tema.
    wxGetApp().UpdateDlgDarkUI(this);

    // 🔑 Que `Tab::decorate()` encuentre NUESTROS campos: es lo que enciende las flechitas naranjas
    //    de "modificado" y el candado del valor del sistema. Sin esto los mandos siguen guardando
    //    bien, pero dejan de DECIR que están tocados — y no saber qué se ha tocado es exactamente
    //    lo que se vino a arreglar en s335.
    for (const auto& g : m_groups)
        m_tab->m_neotko_extra_optgroups.push_back(g);
    m_tab->update_changed_ui();
}

NeoStrokeAdvancedDialog::~NeoStrokeAdvancedDialog()
{
    // 🚨 Desregistrar SIEMPRE, y antes de que los grupos mueran: `decorate()` corre en cada cambio
    //    de preset y seguiría entrando aquí con punteros a campos de una ventana ya cerrada.
    auto& reg = m_tab->m_neotko_extra_optgroups;
    reg.clear();
}

void NeoStrokeAdvancedDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    for (auto& g : m_groups)
        g->msw_rescale();
    m_preview->SetMinSize(wxSize(FromDIP(420), FromDIP(520)));
    if (wxSizer* s = GetSizer())
        s->SetSizeHints(this);
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
