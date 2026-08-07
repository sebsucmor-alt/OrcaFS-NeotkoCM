#include "GUI_ObjectSettings.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_Factories.hpp"
#include "Tab.hpp"
#include "MainFrame.hpp"

#include "OptionsGroup.hpp"
#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "Plater.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Model.hpp"

#include <boost/algorithm/string.hpp>

#include "I18N.hpp"
#include "ConfigManipulation.hpp"

#include <wx/wupdlock.h>

namespace Slic3r
{
namespace GUI
{

OG_Settings::OG_Settings(wxWindow* parent, const bool staticbox) :
    m_parent(parent)
{
    //BBS
    wxString title = "";
    //wxString title = staticbox ? " " : ""; // temporary workaround - #ys_FIXME
    m_og = std::make_shared<ConfigOptionsGroup>(parent, title);
}

bool OG_Settings::IsShown()
{
    return m_og->sizer->IsEmpty() ? false : m_og->sizer->IsShown(size_t(0));
}

void OG_Settings::Show(const bool show)
{
    m_og->Show(show);
}

void OG_Settings::Hide()
{
    Show(false);
}

void OG_Settings::UpdateAndShow(const bool show)
{
    Show(show);
//    m_parent->Layout();
}

wxSizer* OG_Settings::get_sizer()
{
    return m_og->sizer;
}



ObjectSettings::ObjectSettings(wxWindow* parent) :
#if !NEW_OBJECT_SETTING
    OG_Settings(parent, true)
#else
    m_parent(parent)
#endif
{
#if !NEW_OBJECT_SETTING
    m_og->activate();
    m_og->set_name(_(L("Additional process preset")));

    m_settings_list_sizer = new wxBoxSizer(wxVERTICAL);
    m_og->sizer->Add(m_settings_list_sizer, 1, wxEXPAND | wxLEFT, 5);

    m_bmp_delete = ScalableBitmap(parent, "cross");
    m_bmp_delete_focus = ScalableBitmap(parent, "cross_focus");
#endif
}

#if !NEW_OBJECT_SETTING // BBS: new object settings

bool ObjectSettings::update_settings_list()
{
    m_settings_list_sizer->Clear(true);
    m_og_settings.resize(0);

    auto objects_ctrl   = wxGetApp().obj_list();
    auto objects_model  = wxGetApp().obj_list()->GetModel();
    auto config         = wxGetApp().obj_list()->config();

    const auto item = objects_ctrl->GetSelection();

    if (!item || !objects_model->IsSettingsItem(item) || !config || objects_ctrl->multiple_selection())
        return false;

    const bool is_object_settings = objects_model->GetItemType(objects_model->GetParent(item)) == itObject;
    SettingsFactory::Bundle cat_options = SettingsFactory::get_bundle(&config->get(), is_object_settings);

    if (!cat_options.empty())
    {
	    std::vector<std::string> categories;
        categories.reserve(cat_options.size());

        auto extra_column = [config, this](wxWindow* parent, const Line& line)
		{
			auto opt_key = (line.get_options())[0].opt_id;  //we assume that we have one option per line

			auto btn = new ScalableButton(parent, wxID_ANY, m_bmp_delete);
            btn->SetBackgroundColour(parent->GetBackgroundColour());
            btn->SetToolTip(_(L("Remove parameter")));

            btn->SetBitmapFocus(m_bmp_delete_focus.bmp());
            btn->SetBitmapHover(m_bmp_delete_focus.bmp());

			btn->Bind(wxEVT_BUTTON, [opt_key, config, this](wxEvent &event) {
                wxGetApp().plater()->take_snapshot(from_u8((boost::format("Delete Option %s") % opt_key).str()).ToStdString());
				config->erase(opt_key);
                wxGetApp().obj_list()->changed_object();
                wxTheApp->CallAfter([this]() {
                    wxWindowUpdateLocker noUpdates(m_parent);
                    update_settings_list(); 
                    m_parent->Layout(); 
                });
			});
			return btn;
		};

        for (auto& cat : cat_options)
        {
            categories.push_back(cat.first);

            auto optgroup = std::make_shared<ConfigOptionsGroup>(m_og->ctrl_parent(), _(cat.first), config, false, extra_column);
            optgroup->label_width = 15;
            optgroup->sidetext_width = 5;

            optgroup->m_on_change = [this, config](const t_config_option_key& opt_id, const boost::any& value) {
                                    this->update_config_values(config);
                                    wxGetApp().obj_list()->changed_object(); };

            // call back for rescaling of the extracolumn control
            optgroup->rescale_extra_column_item = [this](wxWindow* win) {
                auto *ctrl = dynamic_cast<ScalableButton*>(win);
                if (ctrl == nullptr)
                    return;
                ctrl->SetBitmap_(m_bmp_delete);
                ctrl->SetBitmapFocus(m_bmp_delete_focus.bmp()); 
                ctrl->SetBitmapHover(m_bmp_delete_focus.bmp());
            };

            const bool is_extruders_cat = cat.first == "Extruders";
            for (auto& opt : cat.second)
            {
                Option option = optgroup->get_option(opt);
                option.opt.width = 12;
                if (is_extruders_cat)
                    option.opt.max = wxGetApp().extruders_edited_cnt();
                optgroup->append_single_option_line(option);
            }
            optgroup->activate();
            for (auto& opt : cat.second)
                optgroup->get_field(opt)->m_on_change = [optgroup](const std::string& opt_id, const boost::any& value) {
                    // first of all take a snapshot and then change value in configuration
                    wxGetApp().plater()->take_snapshot(from_u8((boost::format("Change Option %s")% opt_id).str()).ToStdString());
                    optgroup->on_change_OG(opt_id, value);
                };

            optgroup->reload_config();

            m_settings_list_sizer->Add(optgroup->sizer, 0, wxEXPAND | wxALL, 0);
            m_og_settings.push_back(optgroup);
        }

        if (!categories.empty()) {
            objects_model->UpdateSettingsDigest(item, categories);
            update_config_values(config);
        }
    }
    else
    {
        objects_ctrl->select_item(objects_model->Delete(item));
        return false;
    }
    return true;
}

#else
bool ObjectSettings::update_settings_list()
{
    if (!wxGetApp().is_editor())
        return false;
    if (!wxGetApp().mainframe->IsShown())
        return false;

    auto objects_ctrl   = wxGetApp().obj_list();
    auto objects_model  = wxGetApp().obj_list()->GetModel();

    wxDataViewItemArray items;
    objects_ctrl->GetSelections(items);

    std::map<ObjectBase *, ModelConfig*> plate_configs;
    std::map<ObjectBase *, ModelConfig*> object_configs;
    bool is_plate_settings = false;
    bool is_object_settings = false;
    bool is_volume_settings = false;
    bool is_layer_range_settings = false;
    bool is_layer_root = false;
    ModelObject * parent_object = nullptr;
    for (auto item : items) {
        auto type = objects_model->GetItemType(item);
        if (type != itPlate && type != itObject && type != itVolume && type != itLayerRoot && type != itLayer) {
            continue;
        }
        int plate_id = objects_model->GetPlateIdByItem(item);
        PartPlateList& ppl = wxGetApp().plater()->get_partplate_list();
        if (plate_id < 0 || plate_id >= ppl.get_plate_count()) {
            plate_id = ppl.get_curr_plate_index();
        }
        assert(plate_id >= 0 && plate_id < ppl.get_plate_count());
        static ModelConfig cfg;
        cfg.assign_config(*ppl.get_plate(plate_id)->config());
        if (type == itPlate) {
            is_plate_settings = true;
            plate_configs.emplace(ppl.get_plate(plate_id), &cfg);
            break;
        }

        const int obj_idx = objects_model->GetObjectIdByItem(item);
        assert(obj_idx >= 0);
        auto object = wxGetApp().model().objects[obj_idx];
        if (type == itObject) {
            is_object_settings = true;
            plate_configs.emplace(ppl.get_plate(plate_id), &cfg);
            object_configs.emplace(object, &object->config);
        } 
        else if(type == itVolume){
            is_volume_settings = true;
            if (parent_object && parent_object != object)
                return false;
            plate_configs.emplace(ppl.get_plate(plate_id), &cfg);
            parent_object = object;
            const int vol_idx = objects_model->GetVolumeIdByItem(item);
            assert(vol_idx >= 0);
            auto volume = object->volumes[vol_idx];
            object_configs.emplace(volume, &volume->config);
        }
        else if(type == itLayer){
            is_layer_range_settings = true;
            if (parent_object && parent_object != object)
                return false;
            plate_configs.emplace(ppl.get_plate(plate_id), &cfg);
            parent_object = object;

            t_layer_height_range height_range = objects_model->GetLayerRangeByItem(item);
            object_configs.emplace( (ObjectBase*)(&object->layer_config_ranges.at(height_range)), &object->layer_config_ranges.at(height_range) );
        }
        else if (type == itLayerRoot) {
            is_layer_root = true;
        }
    }

    auto tab_plate = dynamic_cast<TabPrintPlate*>(wxGetApp().get_plate_tab());
    auto tab_object = dynamic_cast<TabPrintModel*>(wxGetApp().get_model_tab());
    auto tab_volume = dynamic_cast<TabPrintModel*>(wxGetApp().get_model_tab(true));
    auto tab_layer = dynamic_cast<TabPrintModel*>(wxGetApp().get_layer_tab());

    if (is_plate_settings) {
        tab_plate->set_model_config(plate_configs);
        tab_object->set_model_config({});
        tab_volume->set_model_config({});
        tab_layer->set_model_config({});
        ;// m_tab_active = tab_plate;
    }
    else if (is_object_settings) {
        tab_plate->set_model_config(plate_configs);
        tab_object->set_model_config(object_configs);
        tab_volume->set_model_config({});
        tab_layer->set_model_config({});
        //m_tab_active = tab_object;
    }   
    else if (is_volume_settings) {
        tab_plate->set_model_config(plate_configs);
        tab_object->set_model_config({ {parent_object, &parent_object->config} });
        tab_volume->set_model_config(object_configs);
        tab_layer->set_model_config({});
        //m_tab_active = tab_volume;
    }
    else if (is_layer_range_settings) {
        tab_plate->set_model_config(plate_configs);
        tab_object->set_model_config({ {parent_object, &parent_object->config} });
        tab_volume->set_model_config({});
        tab_layer->set_model_config(object_configs);
        //m_tab_active = tab_layer;
    }    
    else {
        tab_plate->set_model_config({});
        tab_object->set_model_config({});
        tab_volume->set_model_config({});
        tab_layer->set_model_config({});
        //m_tab_active = nullptr;
    }
    ((ParamsPanel*) tab_object->GetParent())->set_active_tab(nullptr);
    return true;
}

#endif

bool ObjectSettings::add_missed_options(ModelConfig* config_to, const DynamicPrintConfig& config_from)
{
    const DynamicPrintConfig& print_config = wxGetApp().plater()->printer_technology() == ptFFF ?
                                             wxGetApp().preset_bundle->prints.get_edited_preset().config :
                                             wxGetApp().preset_bundle->sla_prints.get_edited_preset().config;
    bool is_added = false;

    for (auto opt_key : config_from.diff(print_config))
        if (!config_to->has(opt_key)) {
            config_to->set_key_value(opt_key, config_from.option(opt_key)->clone());
            is_added = true;
        }

    return is_added;
}

void ObjectSettings::update_config_values(ModelConfig* config)
{
    const auto objects_model        = wxGetApp().obj_list()->GetModel();
    const auto item                 = wxGetApp().obj_list()->GetSelection();
    const auto printer_technology   = wxGetApp().plater()->printer_technology();
    const bool is_object_settings   = objects_model->GetItemType(objects_model->GetParent(item)) == itObject;

    if (!item || !objects_model->IsSettingsItem(item) || !config)
        return;

    // update config values according to configuration hierarchy
    DynamicPrintConfig  main_config   = printer_technology == ptFFF ?
                                        wxGetApp().preset_bundle->prints.get_edited_preset().config :
                                        wxGetApp().preset_bundle->sla_prints.get_edited_preset().config;

    auto load_config = [this, config, &main_config]()
    {
        /* Additional check for overrided options.
         * There is a case, when some options should to be added, 
         * to avoid check loop in the next configuration update
         */
        bool is_added = add_missed_options(config, main_config);

        // load checked values from main_config to config
        config->apply_only(main_config, config->keys(), true);
        // Initialize UI components with the config values.
#if !NEW_OBJECT_SETTING
        for (auto og : m_og_settings)
            og->reload_config();
#else
#endif
        // next config check
        update_config_values(config);

        if (is_added) {
// #ysFIXME - Delete after testing! Very likely this CallAfret is no needed
//            wxTheApp->CallAfter([this]() {
                wxWindowUpdateLocker noUpdates(m_parent);
                update_settings_list();
                m_parent->Layout();
//            });
        }
    };

#if !NEW_OBJECT_SETTING
    auto toggle_field = [this](const t_config_option_key & opt_key, bool toggle, int opt_index)
    {
        Field* field = nullptr;
        for (auto og : m_og_settings) {
            field = og->get_fieldc(opt_key, opt_index);
            if (field != nullptr)
                break;
        }
        if (field)
            field->toggle(toggle);
    };
#else
    auto toggle_field = [this](const t_config_option_key & opt_key, bool toggle, int opt_index)
    {
        m_tab_active->toggle_option(opt_key, toggle, opt_index);
    };
#endif

    //BBS: change local config to DynamicPrintConfig
    ConfigManipulation config_manipulation(load_config, toggle_field, nullptr, nullptr, &(config->get()));

    config_manipulation.set_is_BBL_Printer(wxGetApp().preset_bundle->is_bbl_vendor());

    if (!is_object_settings)
    {
        const int obj_idx = objects_model->GetObjectIdByItem(item);
        assert(obj_idx >= 0);
        // for object's part first of all update konfiguration from object 
        main_config.apply(wxGetApp().model().objects[obj_idx]->config.get(), true);
        // and then from its own config
    }

    main_config.apply(config->get(), true);
    printer_technology == ptFFF  ?  config_manipulation.update_print_fff_config(&main_config) :
                                    config_manipulation.update_print_sla_config(&main_config) ;

    printer_technology == ptFFF  ?  config_manipulation.toggle_print_fff_options(&main_config) :
                                    config_manipulation.toggle_print_sla_options(&main_config) ;

    // NEOTKO_HAE_TAG s247 — §13 of docs/FUTURE/HEIGHT_ADAPTIVE_EFFECTS_PLAN.md: when a Height
    // Adaptive Effects curve is active on this object, the gizmo becomes the OWNER of that
    // setting for that object, and the per-object row for it goes inert.
    //
    // 🚨 This is COMMUNICATION, NOT ENFORCEMENT (§13.4). A 3mf from another version, a script,
    // or a height-range modifier can still put a value in that key. The guarantee lives at the
    // point of consumption, where the curve already wins: LayerRegion::flow() overrides the
    // region's sparse_infill_line_width, and PrintObject::make_slices() evaluates the curve
    // instead of the object's xy_*_compensation. This greying-out only explains to the user
    // why the number they would type has no effect — it is not what makes it have no effect.
    // Implementation order matters and was respected: engine precedence first, lock second.
    //
    // Runs LAST on purpose: toggle_print_fff_options() above re-enables rows, so anything we
    // disable has to come after it.
    //
    // ⚠️ This must stay OUT of the global Print Settings tab (§13.1): that tab belongs to the
    // PRESET, shared by every object on the plate. Greying it there because object A has a
    // curve would also grey it for B, C and D, which have none — a visual lie plus a broken
    // legitimate edit. All three lock points of §13.1 (per-object settings, that object's
    // height range modifiers, per-volume overrides) are per-object and all three come through
    // this very function, so one pass covers them.
    {
        const int obj_idx = objects_model->GetObjectIdByItem(item);
        const auto& model_objects = wxGetApp().model().objects;
        if (obj_idx >= 0 && obj_idx < (int)model_objects.size()) {
            const ModelObject* mo = model_objects[obj_idx];
            auto curve_active = [mo](const char* curve_key) {
                if (!mo->config.has(curve_key))
                    return false;
                const auto* opt = dynamic_cast<const ConfigOptionString*>(mo->config.option(curve_key));
                return opt != nullptr && !opt->value.empty();
            };
            // curve key -> the setting it takes ownership of. Keep in sync with the effect
            // registry in GLGizmoHeightAdaptiveEffects::effects().
            static const struct { const char* curve; const char* owned; } kOwnership[] = {
                { "neotko_hae_xy_contour",   "xy_contour_compensation" },
                { "neotko_hae_xy_hole",      "xy_hole_compensation"    },
                { "neotko_hae_infill_width", "sparse_infill_line_width" },
            };
            for (const auto& pair : kOwnership)
                if (curve_active(pair.curve))
                    // No-op when that row isn't in this panel (get_fieldc returns nullptr), so
                    // this is safe for the object, volume and layer-range variants alike.
                    toggle_field(pair.owned, false, 0);
        }
    }
}

void ObjectSettings::UpdateAndShow(const bool show)
{
#if !NEW_OBJECT_SETTING
    OG_Settings::UpdateAndShow(show ? update_settings_list() : false);
#else
    update_settings_list();
#endif
}

void ObjectSettings::msw_rescale()
{
#if !NEW_OBJECT_SETTING
    m_bmp_delete.msw_rescale();
    m_bmp_delete_focus.msw_rescale();

    for (auto group : m_og_settings)
        group->msw_rescale();
#endif
}

void ObjectSettings::sys_color_changed()
{
#if !NEW_OBJECT_SETTING
    m_og->sys_color_changed();

    for (auto group : m_og_settings)
        group->sys_color_changed();
#endif
}

} //namespace GUI
} //namespace Slic3r 
