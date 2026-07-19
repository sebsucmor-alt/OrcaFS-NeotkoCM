#include "ExpertGCodeReprocessor.hpp"

#include <boost/nowide/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <vector>
#include <limits>
#include <cmath>
#include <algorithm>
#include <cctype>

namespace Slic3r {

namespace {

using json = nlohmann::json;

// Mirrors the layer-bucketing logic GCodeViewer.cpp uses to build its m_layers (grouping
// Extrude moves by contiguous Z), but keyed on MoveVertex::gcode_id (the final line number in
// the exported file, valid after GCodeProcessor::synchronize_moves()) instead of move index, and
// standalone so it can run outside the GUI/GCodeViewer. Layer indices are 0-based, in Z order.
struct LayerLineRange
{
    unsigned int first_gcode_id;
    unsigned int last_gcode_id;
};

std::vector<LayerLineRange> build_layer_line_ranges(const GCodeProcessorResult &gcode_result)
{
    std::vector<LayerLineRange> layers;
    constexpr float EPSILON = 1e-4f;
    float last_z = std::numeric_limits<float>::quiet_NaN();

    for (const auto &move : gcode_result.moves) {
        if (move.type != EMoveType::Extrude)
            continue;

        const float z = move.position.z();
        if (layers.empty() || std::isnan(last_z) || z < last_z - EPSILON || last_z + EPSILON < z) {
            layers.push_back({ move.gcode_id, move.gcode_id });
            last_z = z;
        } else {
            layers.back().last_gcode_id = move.gcode_id;
        }
    }

    return layers;
}

struct SpeedRule
{
    bool enabled{ false };
    int layer_from{ -1 };
    int layer_to{ -1 }; // -1 == "to end of file"
    int value{ 100 };   // percent
};

struct FanRule
{
    bool enabled{ false };
    int layer_from{ -1 };
    int layer_to{ -1 }; // -1 == "to end of file"
    int value{ 255 };   // raw M106 S value, 0-255
};

// Force-rewrites a bare Orca-emitted fan line to `value`. Recognizes "M106 S<digits>[trailing]"
// (rewrites just the number, keeps any trailing comment) and bare "M107" (fan off — rewritten to
// an equivalent M106 Svalue, since "override to nonzero" has to replace the command itself).
// Indented/foreign lines are left alone on purpose: this only touches Orca's own plain emissions.
bool try_override_fan_line(const std::string &line, int value, std::string &out)
{
    const size_t end = line.find_last_not_of(" \t\r\n");
    const std::string trimmed = (end == std::string::npos) ? std::string() : line.substr(0, end + 1);

    if (trimmed == "M107") {
        out = "M106 S" + std::to_string(value) + " ; NEOTKO_GCODE_REPROCESSOR fan_override (was M107)";
        return true;
    }

    static const std::string prefix = "M106 S";
    if (trimmed.compare(0, prefix.size(), prefix) != 0)
        return false;

    size_t digits_end = prefix.size();
    while (digits_end < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[digits_end])))
        ++digits_end;
    if (digits_end == prefix.size())
        return false; // "M106 S" with no number following — malformed, leave untouched

    out = "M106 S" + std::to_string(value) + " ; NEOTKO_GCODE_REPROCESSOR fan_override" + trimmed.substr(digits_end);
    return true;
}

// WipeTower::toolchange() (WipeTower.cpp:784) unconditionally emits a bare "M220 S100" on every
// toolchange that goes through the wipe tower — backup/restore (M220 B / M220 R) is dead code
// (#if 0 upstream), so nothing restores the previous feedrate factor. Any speed_multiplier rule
// whose range spans a toolchange gets silently reset the instant that line runs. Our own
// insertions always carry a trailing " ; NEOTKO_..." comment, so a bare match here is
// unambiguously WipeTower's, never one of ours (this scan runs over the pristine file, before
// any of our insertions exist).
bool is_bare_m220_s100(const std::string &line)
{
    const size_t end = line.find_last_not_of(" \t\r\n");
    const std::string trimmed = (end == std::string::npos) ? std::string() : line.substr(0, end + 1);
    return trimmed == "M220 S100";
}

// Reads only what Phase 1 understands. Unknown "type" values and unknown keys inside a rule
// object are silently skipped (forward compatibility) rather than treated as errors.
std::vector<SpeedRule> parse_speed_rules(const std::string &rules_json)
{
    std::vector<SpeedRule> rules;
    if (rules_json.empty())
        return rules;

    json root;
    try {
        root = json::parse(rules_json);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "ExpertGCodeReprocessor: failed to parse rules JSON: " << ex.what();
        return rules;
    }

    if (!root.contains("rules") || !root["rules"].is_array())
        return rules;

    for (const auto &r : root["rules"]) {
        if (!r.is_object() || !r.contains("type") || !r["type"].is_string())
            continue;
        if (r["type"].get<std::string>() != "speed_multiplier")
            continue; // unknown/other rule type: not Phase 1's job, skip
        if (!r.contains("enabled") || !r["enabled"].is_boolean() || !r["enabled"].get<bool>())
            continue;
        if (!r.contains("layer_from") || !r["layer_from"].is_number_integer())
            continue;
        if (!r.contains("value") || !r["value"].is_number_integer())
            continue;

        SpeedRule rule;
        rule.enabled = true;
        rule.layer_from = r["layer_from"].get<int>();
        rule.layer_to = (r.contains("layer_to") && r["layer_to"].is_number_integer()) ? r["layer_to"].get<int>() : -1;
        rule.value = r["value"].get<int>();
        rules.push_back(rule);
    }

    return rules;
}

std::vector<FanRule> parse_fan_rules(const std::string &rules_json)
{
    std::vector<FanRule> rules;
    if (rules_json.empty())
        return rules;

    json root;
    try {
        root = json::parse(rules_json);
    } catch (const std::exception &) {
        return rules; // parse_speed_rules already logs the parse error for this same blob
    }

    if (!root.contains("rules") || !root["rules"].is_array())
        return rules;

    for (const auto &r : root["rules"]) {
        if (!r.is_object() || !r.contains("type") || !r["type"].is_string())
            continue;
        if (r["type"].get<std::string>() != "fan_override")
            continue;
        if (!r.contains("enabled") || !r["enabled"].is_boolean() || !r["enabled"].get<bool>())
            continue;
        if (!r.contains("layer_from") || !r["layer_from"].is_number_integer())
            continue;
        if (!r.contains("value") || !r["value"].is_number_integer())
            continue;

        FanRule rule;
        rule.enabled = true;
        rule.layer_from = r["layer_from"].get<int>();
        rule.layer_to = (r.contains("layer_to") && r["layer_to"].is_number_integer()) ? r["layer_to"].get<int>() : -1;
        rule.value = std::clamp(r["value"].get<int>(), 0, 255);
        rules.push_back(rule);
    }

    return rules;
}

} // namespace

bool run_expert_gcode_reprocessor(const std::string &path, const DynamicPrintConfig &config,
                                   const GCodeProcessorResult &gcode_result)
{
    if (!config.has("expert_gcode_reprocessor_rules"))
        return false;

    const std::string rules_json = config.opt_string("expert_gcode_reprocessor_rules");
    const std::vector<SpeedRule> rules = parse_speed_rules(rules_json);
    const std::vector<FanRule> fan_rules = parse_fan_rules(rules_json);
    if (rules.empty() && fan_rules.empty())
        return false;

    const std::vector<LayerLineRange> layers = build_layer_line_ranges(gcode_result);
    if (layers.empty())
        return false;

    // We need the raw file up front now (not just at the end) because the toolchange re-apply
    // pass below has to scan original line text within each rule's active range.
    std::vector<std::string> lines;
    {
        boost::nowide::ifstream in(path);
        if (!in.good()) {
            BOOST_LOG_TRIVIAL(error) << "ExpertGCodeReprocessor: failed to open " << path << " for reading";
            return false;
        }
        std::string line;
        while (std::getline(in, line))
            lines.push_back(line);
    }

    // insertions: (1-based gcode line number to insert BEFORE, text to insert)
    std::vector<std::pair<unsigned int, std::string>> insertions;

    for (const SpeedRule &rule : rules) {
        if (rule.layer_from < 0 || static_cast<size_t>(rule.layer_from) >= layers.size())
            continue; // out-of-range reference to a layer that no longer exists in this slice

        const unsigned int start_line = layers[rule.layer_from].first_gcode_id;
        insertions.emplace_back(start_line,
                                 "M220 S" + std::to_string(rule.value) + " ; NEOTKO_GCODE_REPROCESSOR speed_multiplier start\n");

        unsigned int restore_before_line;
        if (rule.layer_to >= 0 && static_cast<size_t>(rule.layer_to) < layers.size()) {
            // restore right after the last line of layer_to, i.e. before the first line of the next layer
            const size_t next_layer = static_cast<size_t>(rule.layer_to) + 1;
            restore_before_line = (next_layer < layers.size())
                ? layers[next_layer].first_gcode_id
                : layers.back().last_gcode_id + 1; // layer_to was the last layer: restore right after it
            insertions.emplace_back(restore_before_line, "M220 S100 ; NEOTKO_GCODE_REPROCESSOR speed_multiplier restore\n");
        } else {
            // No explicit end layer: restore at the very end of the file as a safety net so the
            // override never survives into a subsequent print.
            restore_before_line = layers.back().last_gcode_id + 1;
            insertions.emplace_back(restore_before_line, "M220 S100 ; NEOTKO_GCODE_REPROCESSOR speed_multiplier restore (end of file)\n");
        }

        // Re-apply after every WipeTower toolchange reset inside [start_line, restore_before_line).
        // See is_bare_m220_s100() above for why a bare match is safe to trust here. Rewritten
        // IN PLACE (not inserted after) so the dead WipeTower directive never lingers next to our
        // own — one M220 per toolchange, not two.
        for (unsigned int gcode_line = start_line; gcode_line < restore_before_line; ++gcode_line) {
            const size_t idx = static_cast<size_t>(gcode_line - 1);
            if (idx >= lines.size())
                break;
            if (is_bare_m220_s100(lines[idx]))
                lines[idx] = "M220 S" + std::to_string(rule.value) +
                             " ; NEOTKO_GCODE_REPROCESSOR speed_multiplier re-apply after toolchange reset (was M220 S100)";
        }
    }

    // Fan override: pure in-place rewrite, no boundary insertions and no "restore" needed —
    // once we stop touching lines past the range, Orca's own M106/M107 emissions resume
    // untouched on their own. Capped at layers.back().last_gcode_id even for layer_to==-1 ("to
    // end of file") so this never reaches into the end G-code's own cooldown M107.
    bool fan_modified = false;
    for (const FanRule &rule : fan_rules) {
        if (rule.layer_from < 0 || static_cast<size_t>(rule.layer_from) >= layers.size())
            continue;

        const unsigned int start_line = layers[rule.layer_from].first_gcode_id;
        const unsigned int end_line = (rule.layer_to >= 0 && static_cast<size_t>(rule.layer_to) < layers.size())
            ? layers[rule.layer_to].last_gcode_id
            : layers.back().last_gcode_id;

        for (unsigned int gcode_line = start_line; gcode_line <= end_line; ++gcode_line) {
            const size_t idx = static_cast<size_t>(gcode_line - 1);
            if (idx >= lines.size())
                break;
            std::string new_line;
            if (try_override_fan_line(lines[idx], rule.value, new_line)) {
                lines[idx] = new_line;
                fan_modified = true;
            }
        }
    }

    if (insertions.empty() && !fan_modified)
        return false;

    // Stable sort by target line so multiple insertions land in source order; descending so we
    // can splice from the back of the file forward without invalidating earlier line numbers.
    std::sort(insertions.begin(), insertions.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });

    for (const auto &[gcode_line, text] : insertions) {
        // gcode_id is 1-based; inserting at vector index (gcode_line - 1) puts it right before
        // that original line.
        const size_t idx = (gcode_line >= 1) ? static_cast<size_t>(gcode_line - 1) : 0;
        std::string clean_text = text.substr(0, text.size() - 1); // drop trailing '\n', getline already stripped it from `lines`
        lines.insert(lines.begin() + std::min(idx, lines.size()), clean_text);
    }

    boost::nowide::ofstream out(path, std::ios::trunc);
    if (!out.good()) {
        BOOST_LOG_TRIVIAL(error) << "ExpertGCodeReprocessor: failed to open " << path << " for writing";
        return false;
    }
    for (const std::string &line : lines)
        out << line << "\n";

    return true;
}

} // namespace Slic3r
