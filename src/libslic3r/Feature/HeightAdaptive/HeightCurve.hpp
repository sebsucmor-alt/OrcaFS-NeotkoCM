// NEOTKO_HAE_TAG — Height Adaptive Effects: generic Z → value curve engine.
// docs/FUTURE/HEIGHT_ADAPTIVE_EFFECTS_PLAN.md
//
// A HeightCurve is a small list of nodes {z, value, tension} that the user draws in the
// gizmo over the object's REAL layer bands, serialized into a single coString object config
// key so it travels with the model (undo/redo + 3mf) by construction — see §4 of the plan
// and the s238 lesson [[bug_colormix_orphan_recipe_gc_undo]]: state that lives outside the
// model ends up orphaned.
//
// The interpolation mode is a property of the EFFECT, never of the user (§2): an effect that
// requires hard steps (Sparse Infill Line Width, §6.2) must not be able to produce a smooth
// ramp that would land every infill line on air.
//
// 🔑 Golden rule (§3): an empty curve must be a strict no-op. Every consumer asks
// `at(z, fallback)` and gets `fallback` back verbatim when the curve is empty.

#ifndef slic3r_HeightCurve_hpp_
#define slic3r_HeightCurve_hpp_

#include <string>
#include <vector>

namespace Slic3r {
namespace HeightAdaptive {

enum class Interp {
    // Monotone cubic Hermite (Fritsch–Carlson) with a per-segment tension that blends
    // between the plain secant (tension 0 ⇒ exactly linear) and the full monotone tangents
    // (tension 1). Monotone by construction: the curve never overshoots past its nodes,
    // so a curve drawn inside a legal range can never evaluate outside it.
    Smooth,
    // Hard step: the value of the last node at or below z. One transition layer per node.
    Stepped,
};

struct Node {
    double z       = 0.;   // mm, absolute slice Z
    double value   = 0.;   // effect units (mm, %, …) — the effect owns the meaning
    double tension = 0.5;  // [0,1], Smooth only; ignored by Stepped

    bool operator<(const Node &o) const { return this->z < o.z; }
};

class HeightCurve
{
public:
    HeightCurve() = default;
    explicit HeightCurve(Interp interp) : m_interp(interp) {}

    // Parse "[mode|]z:value[:tension]|…", where the optional leading `mode` token is "smooth"
    // or "stepped" and overrides `interp` (which is then only a fallback for curves written
    // before the token existed). Whitespace is tolerated, nodes are
    // sorted by z and exact-duplicate z values are collapsed (last one wins) so a Stepped
    // curve can never have two candidate values for the same layer. A malformed field makes
    // the WHOLE curve empty: a curve we cannot fully understand must be a no-op, never a
    // half-applied guess.
    static HeightCurve parse(const std::string &serialized, Interp interp);

    void set_interp(Interp i) { m_interp = i; }

    // Round-trips through parse(). Values are trimmed to a fixed number of decimals so the
    // serialized form is stable and the config key does not churn the undo stack on redraw.
    std::string serialize() const;

    bool   empty() const { return m_nodes.empty(); }
    Interp interp() const { return m_interp; }

    const std::vector<Node> &nodes() const { return m_nodes; }

    // Replaces the node list (sorting + de-duplicating as parse() does). For the gizmo.
    void set_nodes(std::vector<Node> nodes);

    // 🔑 The single evaluation entry point. Empty curve ⇒ `fallback` returned untouched.
    // Outside the node range the curve is CLAMPED (holds the first / last value): a curve
    // is a statement about the heights the user drew on, not an extrapolation.
    double at(double z, double fallback) const;

private:
    std::vector<Node> m_nodes;
    Interp            m_interp = Interp::Smooth;

    double eval_smooth(double z) const;
    double eval_stepped(double z) const;
};

} // namespace HeightAdaptive
} // namespace Slic3r

#endif // slic3r_HeightCurve_hpp_
