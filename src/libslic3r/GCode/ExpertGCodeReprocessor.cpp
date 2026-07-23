#include "ExpertGCodeReprocessor.hpp"

#include <boost/nowide/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include "../LocalesUtils.hpp"

#include <vector>
#include <limits>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <utility>

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

// NEOTKO_GCODE_REPROCESSOR schema v2 (s215): "global" (default, unchanged Phase 1/3 behaviour) or
// "by_tool" (rule only takes effect while a specific extruder is the active one).
enum class RuleMode { Global, ByTool };

// One toolchange as recorded by GCodeProcessor: gcode_id is the literal "T<n>" line number,
// new_tool is the extruder being switched TO. Verified directly against
// GCodeProcessor::process_T()/store_move_vertex() (GCodeProcessor.cpp) — m_extruder_id is
// reassigned to the new tool BEFORE the Tool_change vertex is stored, so there is no ambiguity
// about "old vs new" here, and gcode_id is m_line_id at the time that exact line was parsed, not
// an adjacent line.
struct ToolChangeEvent
{
    unsigned int gcode_id;
    unsigned char new_tool;
};

std::vector<ToolChangeEvent> build_tool_change_events(const GCodeProcessorResult &gcode_result)
{
    std::vector<ToolChangeEvent> events;
    for (const auto &move : gcode_result.moves) {
        if (move.type == EMoveType::Tool_change)
            events.push_back({ move.gcode_id, move.extruder_id });
    }
    return events;
}

// Inclusive 1-based gcode line range.
struct LineWindow
{
    unsigned int start;
    unsigned int end;
    bool valid() const { return start <= end; }
};

// Every stretch of the WHOLE file (not yet clipped to any rule's layer range) where `target_tool`
// is the active extruder. Boundaries are placed exactly on the safe insertion points: a window
// opens the line right AFTER the T<n> that switches us into target_tool (after the physical swap,
// before that tool's own wipe-tower prime/wipe — those already carry the new extruder_id, see
// header comment), and closes the line right BEFORE the next T<n> that switches us away (so
// inserting at window.end+1 always lands ON that T<n> line, pushing it down, never touching it).
// `initial_tool` covers whatever tool is active before the first toolchange in the file, without
// assuming a literal "T0" line exists at the very start.
std::vector<LineWindow> build_tool_windows(const std::vector<ToolChangeEvent> &tool_changes,
                                            unsigned char initial_tool, unsigned char target_tool,
                                            unsigned int eof_line)
{
    std::vector<LineWindow> windows;
    unsigned char active_tool = initial_tool;
    unsigned int window_start = 1;
    bool open = (active_tool == target_tool);

    for (const ToolChangeEvent &ev : tool_changes) {
        if (open) {
            const LineWindow w{ window_start, ev.gcode_id - 1 };
            if (w.valid())
                windows.push_back(w);
            open = false;
        }
        if (ev.new_tool == target_tool) {
            window_start = ev.gcode_id + 1;
            open = true;
        }
        active_tool = ev.new_tool;
    }
    if (open) {
        const LineWindow w{ window_start, eof_line };
        if (w.valid())
            windows.push_back(w);
    }
    return windows;
}

// NEOTKO_GCODE_REPROCESSOR "Avoid Wipetower" (s216i): every gcode line range tagged erWipeTower,
// across the WHOLE file (not clipped to any rule yet — that happens later via subtract_windows()
// against whatever compute_active_windows() already produced for a given rule). Deliberately scans
// ALL moves, not just EMoveType::Extrude like build_layer_line_ranges() above — MoveVertex::
// extrusion_role is stamped on every move type by store_move_vertex() (GCodeProcessor.cpp), and the
// Role tag is sticky (stays erWipeTower until the next Role comment), so the travel/wipe moves
// right at the toolchange boundary carry it too — exactly the moves a "safe to touch" boundary
// needs to exclude along with the tower extrusion itself. gcode_id is non-decreasing across
// `moves` (m_line_id only ever resets once, before the whole file is processed), so a plain
// contiguous-run grouping is safe, same idea as build_layer_line_ranges() but keyed on this role
// instead of Z.
std::vector<LineWindow> build_wipetower_windows(const GCodeProcessorResult &gcode_result)
{
    std::vector<LineWindow> windows;
    bool in_wipetower = false;

    for (const auto &move : gcode_result.moves) {
        const bool is_wt = (move.extrusion_role == erWipeTower);
        if (is_wt && !in_wipetower)
            windows.push_back({ move.gcode_id, move.gcode_id });
        else if (is_wt)
            windows.back().end = move.gcode_id;
        in_wipetower = is_wt;
    }

    return windows;
}

// Interval subtraction: removes every range in `subtract` from `windows`, splitting a window in
// two if a subtracted range falls strictly inside it, trimming an edge if it only overlaps one
// side, and dropping the window entirely if a subtracted range fully covers it. Applied once per
// `subtract` entry so multiple wipe-tower visits landing inside the same original window each
// further split whatever is left. `sub.start - 1` never underflows: every LineWindow::start in
// this file is >=1 by construction (1-based lines — see build_tool_windows() above), and that
// subtraction only runs guarded by `sub.start > w.start`, which already forces sub.start >= 2.
std::vector<LineWindow> subtract_windows(const std::vector<LineWindow> &windows, const std::vector<LineWindow> &subtract)
{
    std::vector<LineWindow> result = windows;

    for (const LineWindow &sub : subtract) {
        std::vector<LineWindow> next;
        for (const LineWindow &w : result) {
            if (sub.end < w.start || sub.start > w.end) {
                next.push_back(w); // no overlap
                continue;
            }
            if (sub.start > w.start)
                next.push_back({ w.start, sub.start - 1 }); // left remainder
            if (sub.end < w.end)
                next.push_back({ sub.end + 1, w.end }); // right remainder
            // else: `sub` fully covers `w` — dropped, neither remainder emitted
        }
        result = std::move(next);
    }

    result.erase(std::remove_if(result.begin(), result.end(), [](const LineWindow &w) { return !w.valid(); }), result.end());
    return result;
}

// Resolves a rule's [layer_from, layer_to] to the list of active windows it should actually
// touch: one window spanning the whole range for "global", or every per-tool window from
// build_tool_windows() clipped into that same range for "by_tool". Clipping can only move a tool
// window's edges further inward (toward the interior of that tool's own active stretch) — it can
// never push an edge past the toolchange it's already anchored to, so this can't produce an
// insertion point inside a toolchange regardless of what layer_from/layer_to the user picks.
//
// `for_insertion` selects which of the two DIFFERENT end-bound conventions Phase 1/3 already
// shipped with stays byte-identical for global-mode rules:
// - true  (speed_multiplier, z_offset — anything that INSERTS a restore command): end resolves to
//   layers[layer_to + 1].first_gcode_id - 1, so window.end+1 lands exactly where Phase 1 always
//   inserted its restore — right before the next layer's first real extrusion, letting the
//   override ride through the inter-layer travel/Z-hop in between. layer_to==-1 or "last layer"
//   still resolves to layers.back().last_gcode_id (end of file), also unchanged.
// - false (fan_override — pure in-place REWRITE, no insertion): end stays the tight
//   layers[layer_to].last_gcode_id, identical to Phase 3's original inclusive scan bound.
// By-tool windows reuse whichever bound as their clip ceiling, but in practice a real toolchange
// almost always sits well inside it, so the distinction rarely changes by-tool behaviour either
// way — it only matters for the (global, or a tool active straight through a layer_to boundary)
// cases that Phase 1/3 already defined.
std::vector<LineWindow> compute_active_windows(RuleMode mode, int tool, int layer_from, int layer_to,
                                                const std::vector<LayerLineRange> &layers,
                                                const std::vector<ToolChangeEvent> &tool_changes,
                                                unsigned char initial_tool, bool for_insertion)
{
    std::vector<LineWindow> result;
    if (layer_from < 0 || static_cast<size_t>(layer_from) >= layers.size())
        return result; // out-of-range reference to a layer that no longer exists in this slice

    const unsigned int range_start = layers[layer_from].first_gcode_id;
    unsigned int range_end;
    if (layer_to >= 0 && static_cast<size_t>(layer_to) < layers.size()) {
        if (for_insertion) {
            const size_t next_layer = static_cast<size_t>(layer_to) + 1;
            range_end = (next_layer < layers.size())
                ? layers[next_layer].first_gcode_id - 1
                : layers.back().last_gcode_id;
        } else {
            range_end = layers[layer_to].last_gcode_id;
        }
    } else {
        range_end = layers.back().last_gcode_id;
    }
    if (range_start > range_end)
        return result;

    if (mode == RuleMode::Global) {
        result.push_back({ range_start, range_end });
        return result;
    }

    if (tool < 0 || tool > static_cast<int>(std::numeric_limits<unsigned char>::max()))
        return result; // malformed by_tool rule with no usable tool id: no-op rather than guess

    const std::vector<LineWindow> tool_windows = build_tool_windows(
        tool_changes, initial_tool, static_cast<unsigned char>(tool), layers.back().last_gcode_id);
    for (const LineWindow &tw : tool_windows) {
        const LineWindow clipped{ std::max(tw.start, range_start), std::min(tw.end, range_end) };
        if (clipped.valid())
            result.push_back(clipped);
    }
    return result;
}

// Reads "mode" ("global" default/unrecognized, or "by_tool") and, only when by_tool, "tool"
// (0-based extruder index; missing/malformed => -1, which compute_active_windows() treats as a
// no-op rather than guessing a tool).
RuleMode parse_rule_mode(const json &r)
{
    return (r.contains("mode") && r["mode"].is_string() && r["mode"].get<std::string>() == "by_tool")
        ? RuleMode::ByTool : RuleMode::Global;
}

int parse_rule_tool(const json &r, RuleMode mode)
{
    if (mode != RuleMode::ByTool)
        return -1;
    if (!r.contains("tool") || !r["tool"].is_number_integer())
        return -1;
    return r["tool"].get<int>();
}

struct SpeedRule
{
    bool enabled{ false };
    RuleMode mode{ RuleMode::Global };
    int tool{ -1 };
    int layer_from{ -1 };
    int layer_to{ -1 }; // -1 == "to end of file"
    int value{ 100 };   // percent
    bool avoid_wipetower{ false }; // NEOTKO_GCODE_REPROCESSOR "Avoid Wipetower" (s216i)
};

struct FanRule
{
    bool enabled{ false };
    RuleMode mode{ RuleMode::Global };
    int tool{ -1 };
    int layer_from{ -1 };
    int layer_to{ -1 }; // -1 == "to end of file"
    int value{ 255 };   // raw M106 S value, 0-255
    bool avoid_wipetower{ false }; // NEOTKO_GCODE_REPROCESSOR "Avoid Wipetower" (s216i)
};

// NEOTKO_GCODE_REPROCESSOR Phase 2: SET_GCODE_OFFSET Z=<value> at the start of each active
// window, SET_GCODE_OFFSET Z=0 at its end. Absolute Z= rather than relative Z_ADJUST: nothing
// else in this codebase ever calls SET_GCODE_OFFSET (grepped before writing this), so there is no
// external baseline to preserve, and Z_ADJUST=0 would be a no-op (it means "add zero to whatever
// is already set", not "reset to zero") — Z=0 is the only form that unconditionally clears back
// to neutral. Also more robust to a stray unpaired insertion: an orphaned Z=0 restore always
// lands on a known-good value, where an orphaned inverse Z_ADJUST would have left the offset
// permanently skewed. No MOVE=1: Klipper only changes the target of the *next* Z move, it doesn't
// inject an immediate corrective move mid-layer. Clamped to [-0.3, 0.3]mm (s215, user request):
// keeps any misconfiguration in the "underextrusion / overly-aggressive ironing" failure range
// rather than anything that could physically crash the machine.
struct OffsetRule
{
    bool enabled{ false };
    RuleMode mode{ RuleMode::Global };
    int tool{ -1 };
    int layer_from{ -1 };
    int layer_to{ -1 }; // -1 == "to end of file"
    double value{ 0.0 }; // mm, absolute SET_GCODE_OFFSET Z value, clamped to [-0.3, 0.3]
    bool avoid_wipetower{ false }; // NEOTKO_GCODE_REPROCESSOR "Avoid Wipetower" (s216i)
};

// NEOTKO_GCODE_REPROCESSOR: "flow_multiplier" — M221 S<value>, same insertion-based mechanics as
// speed_multiplier (M220): apply at window start, restore to 100 at window end. Unlike speed,
// there is no known WipeTower-side M221 reset to defend against — grepped WipeTower2.cpp/
// NeoWipeTower.cpp for "M221"/"flow_override" before writing this, nothing analogous to
// speed_override() exists for flow, so no reapply-after-toolchange-reset scan is needed here.
struct FlowRule
{
    bool enabled{ false };
    RuleMode mode{ RuleMode::Global };
    int tool{ -1 };
    int layer_from{ -1 };
    int layer_to{ -1 }; // -1 == "to end of file"
    int value{ 100 };   // percent, M221 S<value>, clamped [20, 200]
    bool avoid_wipetower{ false }; // NEOTKO_GCODE_REPROCESSOR "Avoid Wipetower" (s216i)
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

// Historically WipeTower2::toolchange()/NeoWipeTower (the real engines for non-BBL machines,
// WipeTower.cpp is BBL-only) unconditionally emitted a bare "M220 S100" on every toolchange,
// silently resetting any active speed_multiplier override the instant that line ran. Fixed at the
// source in s214: both classes now skip that reset entirely when neotko_libre_mode is on
// (`if (!m_neotko_libre_mode) writer.speed_override(100);`), which is always true here since this
// reprocessor only ever runs under LibreMode — so on the current Klipper/U1 target this scan
// should no longer find anything (speed_override_backup()/restore(), i.e. M220 B/R, are also
// no-ops on Klipper). Left in place as defense-in-depth for other flavour/mode combinations. Our
// own insertions always carry a trailing " ; NEOTKO_..." comment, so a bare match here is
// unambiguously not one of ours (this scan runs over the pristine file, before any of our
// insertions exist).
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
        rule.mode = parse_rule_mode(r);
        rule.tool = parse_rule_tool(r, rule.mode);
        rule.layer_from = r["layer_from"].get<int>();
        rule.layer_to = (r.contains("layer_to") && r["layer_to"].is_number_integer()) ? r["layer_to"].get<int>() : -1;
        rule.value = r["value"].get<int>();
        rule.avoid_wipetower = r.value("avoid_wipetower", false);
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
        rule.mode = parse_rule_mode(r);
        rule.tool = parse_rule_tool(r, rule.mode);
        rule.layer_from = r["layer_from"].get<int>();
        rule.layer_to = (r.contains("layer_to") && r["layer_to"].is_number_integer()) ? r["layer_to"].get<int>() : -1;
        rule.value = std::clamp(r["value"].get<int>(), 0, 255);
        rule.avoid_wipetower = r.value("avoid_wipetower", false);
        rules.push_back(rule);
    }

    return rules;
}

// NEOTKO_GCODE_REPROCESSOR Phase 2. Same forward-compat posture as the other two parsers: unknown
// "type"/keys skipped, not errors. "value" accepts any JSON number (fan/speed stay integer-only,
// unchanged) since a Z-offset delta is meaningfully fractional (e.g. -0.03).
std::vector<OffsetRule> parse_offset_rules(const std::string &rules_json)
{
    std::vector<OffsetRule> rules;
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
        if (r["type"].get<std::string>() != "z_offset")
            continue;
        if (!r.contains("enabled") || !r["enabled"].is_boolean() || !r["enabled"].get<bool>())
            continue;
        if (!r.contains("layer_from") || !r["layer_from"].is_number_integer())
            continue;
        if (!r.contains("value") || !r["value"].is_number())
            continue;

        OffsetRule rule;
        rule.enabled = true;
        rule.mode = parse_rule_mode(r);
        rule.tool = parse_rule_tool(r, rule.mode);
        rule.layer_from = r["layer_from"].get<int>();
        rule.layer_to = (r.contains("layer_to") && r["layer_to"].is_number_integer()) ? r["layer_to"].get<int>() : -1;
        rule.value = std::clamp(r["value"].get<double>(), -0.3, 0.3);
        rule.avoid_wipetower = r.value("avoid_wipetower", false);
        rules.push_back(rule);
    }

    return rules;
}

// Same shape as parse_speed_rules, "flow_multiplier" instead of "speed_multiplier". Clamped to
// [20, 200] the same way fan is clamped to [0, 255] — a hand-edited or stale JSON can never smuggle
// an out-of-range M221 value past this, regardless of what the UI slider itself already enforces.
std::vector<FlowRule> parse_flow_rules(const std::string &rules_json)
{
    std::vector<FlowRule> rules;
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
        if (r["type"].get<std::string>() != "flow_multiplier")
            continue;
        if (!r.contains("enabled") || !r["enabled"].is_boolean() || !r["enabled"].get<bool>())
            continue;
        if (!r.contains("layer_from") || !r["layer_from"].is_number_integer())
            continue;
        if (!r.contains("value") || !r["value"].is_number_integer())
            continue;

        FlowRule rule;
        rule.enabled = true;
        rule.mode = parse_rule_mode(r);
        rule.tool = parse_rule_tool(r, rule.mode);
        rule.layer_from = r["layer_from"].get<int>();
        rule.layer_to = (r.contains("layer_to") && r["layer_to"].is_number_integer()) ? r["layer_to"].get<int>() : -1;
        rule.value = std::clamp(r["value"].get<int>(), 20, 200);
        rule.avoid_wipetower = r.value("avoid_wipetower", false);
        rules.push_back(rule);
    }

    return rules;
}

// NEOTKO_GCODE_REPROCESSOR: top-level (not per-rule) master switch — the panel's "ON/OFF" toggle
// (s215, replaces the old "Apply" button ceremony, see run_expert_gcode_reprocessor() below).
// Missing/malformed => true, so rules saved before this toggle existed keep behaving exactly as
// before — nothing to migrate.
bool reprocessor_globally_enabled(const std::string &rules_json)
{
    if (rules_json.empty())
        return true;
    try {
        const json root = json::parse(rules_json);
        return root.is_object() ? root.value("enabled", true) : true;
    } catch (const std::exception &) {
        return true; // parse_speed_rules already logs the parse error for this same blob
    }
}

} // namespace

bool run_expert_gcode_reprocessor(const std::string &path, const DynamicPrintConfig &config,
                                   const GCodeProcessorResult &gcode_result)
{
    if (!config.has("expert_gcode_reprocessor_rules"))
        return false;

    const std::string rules_json = config.opt_string("expert_gcode_reprocessor_rules");
    if (!reprocessor_globally_enabled(rules_json))
        return false; // master ON/OFF toggle is OFF: skip entirely, regardless of what rules exist

    const std::vector<SpeedRule> rules = parse_speed_rules(rules_json);
    const std::vector<FanRule> fan_rules = parse_fan_rules(rules_json);
    const std::vector<OffsetRule> offset_rules = parse_offset_rules(rules_json);
    const std::vector<FlowRule> flow_rules = parse_flow_rules(rules_json);
    if (rules.empty() && fan_rules.empty() && offset_rules.empty() && flow_rules.empty())
        return false;

    const std::vector<LayerLineRange> layers = build_layer_line_ranges(gcode_result);
    if (layers.empty())
        return false;

    // Only needed by by_tool rules, but cheap enough (one linear scan already done for `layers`
    // above) to always compute: gcode_id/extruder_id of every Tool_change move, plus whichever
    // tool is active before the first one of those (see build_tool_windows()).
    const std::vector<ToolChangeEvent> tool_changes = build_tool_change_events(gcode_result);
    const unsigned char initial_tool = gcode_result.moves.empty() ? 0 : gcode_result.moves.front().extruder_id;

    // NEOTKO_GCODE_REPROCESSOR "Avoid Wipetower" (s216i): only consumed by rules with
    // avoid_wipetower==true, but computed unconditionally here — same "cheap enough, always
    // compute" posture as tool_changes above.
    const std::vector<LineWindow> wipetower_windows = build_wipetower_windows(gcode_result);

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
        std::vector<LineWindow> windows = compute_active_windows(
            rule.mode, rule.tool, rule.layer_from, rule.layer_to, layers, tool_changes, initial_tool,
            /*for_insertion=*/true);
        if (rule.avoid_wipetower)
            windows = subtract_windows(windows, wipetower_windows);

        for (const LineWindow &w : windows) {
            insertions.emplace_back(w.start,
                                     "M220 S" + std::to_string(rule.value) + " ; NEOTKO_GCODE_REPROCESSOR speed_multiplier start\n");
            const unsigned int restore_before_line = w.end + 1;
            insertions.emplace_back(restore_before_line, "M220 S100 ; NEOTKO_GCODE_REPROCESSOR speed_multiplier restore\n");

            // Re-apply after every WipeTower toolchange reset inside [w.start, restore_before_line).
            // See is_bare_m220_s100() above for why a bare match is safe to trust here (and why it
            // should no longer actually fire on the current Klipper/LibreMode target). Rewritten
            // IN PLACE (not inserted after) so the dead WipeTower directive never lingers next to
            // our own — one M220 per toolchange, not two.
            for (unsigned int gcode_line = w.start; gcode_line < restore_before_line; ++gcode_line) {
                const size_t idx = static_cast<size_t>(gcode_line - 1);
                if (idx >= lines.size())
                    break;
                if (is_bare_m220_s100(lines[idx]))
                    lines[idx] = "M220 S" + std::to_string(rule.value) +
                                 " ; NEOTKO_GCODE_REPROCESSOR speed_multiplier re-apply after toolchange reset (was M220 S100)";
            }
        }
    }

    // Fan override: pure in-place rewrite, no boundary insertions and no "restore" needed — once
    // we stop touching lines past a window, Orca's own M106/M107 emissions resume untouched on
    // their own. Global mode: one window == the old Phase-3 behaviour, capped at
    // layers.back().last_gcode_id even for layer_to==-1 so this never reaches into the end
    // G-code's own cooldown M107. By-tool: same cap, narrowed to that tool's own windows.
    bool fan_modified = false;
    for (const FanRule &rule : fan_rules) {
        std::vector<LineWindow> windows = compute_active_windows(
            rule.mode, rule.tool, rule.layer_from, rule.layer_to, layers, tool_changes, initial_tool,
            /*for_insertion=*/false);
        if (rule.avoid_wipetower)
            windows = subtract_windows(windows, wipetower_windows);

        for (const LineWindow &w : windows) {
            for (unsigned int gcode_line = w.start; gcode_line <= w.end; ++gcode_line) {
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
    }

    // Z-offset (Phase 2): insertion-based like speed. Restore is a plain absolute Z=0, not the
    // inverse of `value` — see OffsetRule above for why that's both simpler and safer here.
    for (const OffsetRule &rule : offset_rules) {
        std::vector<LineWindow> windows = compute_active_windows(
            rule.mode, rule.tool, rule.layer_from, rule.layer_to, layers, tool_changes, initial_tool,
            /*for_insertion=*/true);
        if (rule.avoid_wipetower)
            windows = subtract_windows(windows, wipetower_windows);

        const std::string applied = Slic3r::float_to_string_decimal_point(rule.value, 4);
        for (const LineWindow &w : windows) {
            insertions.emplace_back(w.start,
                                     "SET_GCODE_OFFSET Z=" + applied + " ; NEOTKO_GCODE_REPROCESSOR z_offset start\n");
            insertions.emplace_back(w.end + 1,
                                     "SET_GCODE_OFFSET Z=0 ; NEOTKO_GCODE_REPROCESSOR z_offset restore\n");
        }
    }

    // Flow override: insertion-based like speed, restore to a fixed 100 (not an inverse — same
    // reasoning as z_offset: M221 has no baseline outside this feature worth preserving). No
    // reapply-after-toolchange-reset scan — see FlowRule above, nothing resets M221 mid-toolchange.
    for (const FlowRule &rule : flow_rules) {
        std::vector<LineWindow> windows = compute_active_windows(
            rule.mode, rule.tool, rule.layer_from, rule.layer_to, layers, tool_changes, initial_tool,
            /*for_insertion=*/true);
        if (rule.avoid_wipetower)
            windows = subtract_windows(windows, wipetower_windows);

        for (const LineWindow &w : windows) {
            insertions.emplace_back(w.start,
                                     "M221 S" + std::to_string(rule.value) + " ; NEOTKO_GCODE_REPROCESSOR flow_multiplier start\n");
            insertions.emplace_back(w.end + 1,
                                     "M221 S100 ; NEOTKO_GCODE_REPROCESSOR flow_multiplier restore\n");
        }
    }

    if (insertions.empty() && !fan_modified)
        return false;

    // Stable sort by target line so multiple insertions land in source order (matters when two
    // rules/windows insert at the exact same line, e.g. a speed and an offset rule sharing the
    // same by-tool window boundary); descending so we can splice from the back of the file
    // forward without invalidating earlier line numbers.
    std::stable_sort(insertions.begin(), insertions.end(),
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
