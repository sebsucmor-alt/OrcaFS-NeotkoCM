// NEOTKO_NEOSTROKE_TAG s342 — ajustes por isla: lector y escritor del texto. Ver NeoStrokeIslands.hpp.
#include "NeoStrokeIslands.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace Slic3r { namespace NeoArachne {

const std::string* NsIslandOverride::find(const std::string& key) const
{
    for (const auto& kv : values)
        if (kv.first == key)
            return &kv.second;
    return nullptr;
}

void NsIslandOverride::set(const std::string& key, const std::string& value)
{
    for (auto& kv : values)
        if (kv.first == key) {
            kv.second = value;
            return;
        }
    values.emplace_back(key, value);
}

void NsIslandOverride::erase(const std::string& key)
{
    values.erase(std::remove_if(values.begin(), values.end(),
                                [&](const std::pair<std::string, std::string>& kv) { return kv.first == key; }),
                 values.end());
}

// 🚨 Si se añade una clave aquí, tiene que tener su rama en `apply_island_override` (NeoStroke.cpp).
//    Fuera a propósito: `neostroke_width_ref` (es la UNIDAD de los %: el relleno y el paso 4 la usan para toda la
//    capa), `neostroke_layer_jitter`, `neostroke_skate`, `neostroke_skate_detour` (orden y viajes, del objeto).
const std::vector<std::string>& island_override_keys()
{
    static const std::vector<std::string> keys = {
        "neostroke_min_width_pct",    "neostroke_max_width_pct",    "neostroke_detail_min_pct",
        "neostroke_bead_min_pct",     "neostroke_max_bead_pct",     "neostroke_max_stroke_width",
        "neostroke_band_mm",          "neostroke_curve_overlap",    "neostroke_lane_overlap",
        "neostroke_lead_in",          "neostroke_end_at_junctions", "neostroke_continuous_turns",
        "neostroke_variable_k",       "neostroke_corner_hooks",
    };
    return keys;
}

bool is_island_override_key(const std::string& key)
{
    const auto& k = island_override_keys();
    return std::find(k.begin(), k.end(), key) != k.end();
}

static std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos)
        return {};
    const size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

static bool to_double(const std::string& s, double& out)
{
    const std::string t = trim(s);
    if (t.empty())
        return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && *end == '\0';
}

static std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else
            cur += c;
    }
    out.push_back(cur);
    return out;
}

std::vector<NsIslandOverride> parse_island_overrides(const std::string& text, size_t* skipped)
{
    std::vector<NsIslandOverride> out;
    size_t bad = 0;
    for (const std::string& raw : split(text, '\n')) {
        const std::string line = trim(raw);
        if (line.empty())
            continue;
        const std::vector<std::string> f = split(line, '|');
        NsIslandOverride o;
        if (f.size() < 3 || !to_double(f[1], o.x) || !to_double(f[2], o.y)) {
            ++bad;
            continue;
        }
        o.name = trim(f[0]);
        if (f.size() >= 4)
            for (const std::string& kv : split(f[3], ';')) {
                if (trim(kv).empty())
                    continue;
                const size_t eq = kv.find('=');
                const std::string k = eq == std::string::npos ? std::string() : trim(kv.substr(0, eq));
                const std::string v = eq == std::string::npos ? std::string() : trim(kv.substr(eq + 1));
                if (k.empty() || v.empty() || !is_island_override_key(k)) {
                    ++bad;
                    continue;
                }
                o.set(k, v);
            }
        out.push_back(std::move(o));
    }
    if (skipped)
        *skipped = bad;
    return out;
}

static std::string clean_name(const std::string& s)
{
    std::string out;
    for (char c : s)
        if (c != '|' && c != ';' && c != '=' && c != '\n' && c != '\r')
            out += c;
    return trim(out);
}

std::string write_island_overrides(const std::vector<NsIslandOverride>& list)
{
    std::string out;
    char num[64];
    for (const NsIslandOverride& o : list) {
        if (!out.empty())
            out += '\n';
        out += clean_name(o.name);
        // 4 decimales = 0.1 µm: sobra para caer dentro de una letra y no ensucia el 3MF.
        snprintf(num, sizeof(num), "|%.4f|%.4f|", o.x, o.y);
        out += num;
        bool first = true;
        for (const auto& kv : o.values) {
            if (!is_island_override_key(kv.first))
                continue;
            if (!first)
                out += ';';
            out += kv.first + "=" + kv.second;
            first = false;
        }
    }
    return out;
}

}} // namespace Slic3r::NeoArachne
