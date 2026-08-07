// NEOTKO_HAE_TAG — see HeightCurve.hpp.

#include "HeightCurve.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace Slic3r {
namespace HeightAdaptive {

static bool parse_double(const std::string &s, double &out)
{
    // Trim; an empty field is not a number (strtod would happily return 0 for "").
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return false;
    size_t e = s.find_last_not_of(" \t");
    const std::string t = s.substr(b, e - b + 1);

    try {
        size_t consumed = 0;
        const double v  = std::stod(t, &consumed);
        if (consumed != t.size() || ! std::isfinite(v))
            return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

static std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string              cur;
    std::istringstream       is(s);
    while (std::getline(is, cur, sep))
        out.emplace_back(cur);
    return out;
}

static void sort_and_dedup(std::vector<Node> &nodes)
{
    // Stable sort keeps the written order among nodes sharing a z; of each such group we
    // keep the LAST, so re-writing a node at the same height overrides the older one
    // instead of being silently ignored.
    std::stable_sort(nodes.begin(), nodes.end());
    std::vector<Node> out;
    out.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++ i)
        if (i + 1 == nodes.size() || nodes[i + 1].z != nodes[i].z)
            out.emplace_back(nodes[i]);
    nodes = std::move(out);
}

HeightCurve HeightCurve::parse(const std::string &serialized, Interp default_interp)
{
    HeightCurve curve(default_interp);
    if (serialized.find_first_not_of(" \t") == std::string::npos)
        return curve; // empty ⇒ effect off

    // s247 REVISION — the interpolation mode was originally fixed by the effect and never
    // stored (plan §2). It is now stored WITH the curve, as an optional leading token, because
    // the mode turned out to be a property of the individual curve, not of the parameter:
    // whether a smooth width ramp is safe depends on how steep THAT curve is, not on the fact
    // that it drives sparse infill width. See the misalignment estimate in the gizmo.
    // Back-compatible: a curve written before this (no token) keeps the effect's default, so
    // existing 3mf files evaluate exactly as they did.
    std::vector<std::string> fields = split(serialized, '|');
    Interp interp = default_interp;
    if (! fields.empty()) {
        size_t b = fields[0].find_first_not_of(" \t");
        std::string head = b == std::string::npos ? std::string() : fields[0].substr(b);
        size_t e = head.find_last_not_of(" \t");
        if (e != std::string::npos)
            head = head.substr(0, e + 1);
        if (head == "smooth" || head == "stepped") {
            interp = (head == "smooth") ? Interp::Smooth : Interp::Stepped;
            fields.erase(fields.begin());
        }
    }
    curve.m_interp = interp;

    std::vector<Node> nodes;
    for (const std::string &field : fields) {
        if (field.find_first_not_of(" \t") == std::string::npos)
            continue; // tolerate a stray trailing separator
        const std::vector<std::string> parts = split(field, ':');
        if (parts.size() < 2 || parts.size() > 3)
            return HeightCurve(interp); // malformed ⇒ whole curve is a no-op
        Node n;
        if (! parse_double(parts[0], n.z) || ! parse_double(parts[1], n.value))
            return HeightCurve(interp);
        if (parts.size() == 3 && ! parse_double(parts[2], n.tension))
            return HeightCurve(interp);
        n.tension = std::clamp(n.tension, 0., 1.);
        nodes.emplace_back(n);
    }

    sort_and_dedup(nodes);
    curve.m_nodes = std::move(nodes);
    return curve;
}

std::string HeightCurve::serialize() const
{
    if (m_nodes.empty())
        return {};
    // Mode token first — always written, so a curve never depends on the reader guessing right.
    std::string out = (m_interp == Interp::Stepped) ? "stepped" : "smooth";
    char        buf[128];
    for (const Node &n : m_nodes) {
        if (m_interp == Interp::Stepped)
            // Tension is meaningless for a step; writing it would only invite someone to
            // believe it does something.
            std::snprintf(buf, sizeof(buf), "|%.4g:%.4g", n.z, n.value);
        else
            std::snprintf(buf, sizeof(buf), "|%.4g:%.4g:%.3g", n.z, n.value, n.tension);
        out += buf;
    }
    return out;
}

void HeightCurve::set_nodes(std::vector<Node> nodes)
{
    for (Node &n : nodes)
        n.tension = std::clamp(n.tension, 0., 1.);
    sort_and_dedup(nodes);
    m_nodes = std::move(nodes);
}

double HeightCurve::at(double z, double fallback) const
{
    if (m_nodes.empty())
        return fallback; // 🔑 strict no-op
    if (m_nodes.size() == 1)
        return m_nodes.front().value;
    // Clamped outside the drawn range, in both modes.
    if (z <= m_nodes.front().z)
        return m_nodes.front().value;
    if (z >= m_nodes.back().z)
        return m_nodes.back().value;
    return m_interp == Interp::Stepped ? this->eval_stepped(z) : this->eval_smooth(z);
}

double HeightCurve::eval_stepped(double z) const
{
    // Last node at or below z. upper_bound gives the first node strictly above.
    auto it = std::upper_bound(m_nodes.begin(), m_nodes.end(), z,
                               [](double v, const Node &n) { return v < n.z; });
    return (it == m_nodes.begin() ? *it : *(it - 1)).value;
}

double HeightCurve::eval_smooth(double z) const
{
    const size_t n = m_nodes.size();

    // Segment containing z (guaranteed to exist: z is strictly inside the range).
    auto it = std::upper_bound(m_nodes.begin(), m_nodes.end(), z,
                               [](double v, const Node &n) { return v < n.z; });
    const size_t i  = size_t(std::distance(m_nodes.begin(), it)) - 1;
    const Node & p0 = m_nodes[i];
    const Node & p1 = m_nodes[i + 1];

    const double h = p1.z - p0.z;
    if (h <= 0.)
        return p1.value; // coincident nodes were de-duplicated, but never divide blind

    // Secant slopes of every segment; delta[k] is the slope between node k and k+1.
    auto delta = [&](size_t k) -> double {
        const double dz = m_nodes[k + 1].z - m_nodes[k].z;
        return dz > 0. ? (m_nodes[k + 1].value - m_nodes[k].value) / dz : 0.;
    };

    // Fritsch–Carlson monotone tangents, computed only for the two endpoints we need.
    auto tangent = [&](size_t k) -> double {
        if (k == 0)
            return delta(0);
        if (k == n - 1)
            return delta(n - 2);
        const double d0 = delta(k - 1), d1 = delta(k);
        if (d0 * d1 <= 0.)
            return 0.; // local extremum ⇒ flat tangent, this is what kills overshoot
        // Harmonic-mean form, limited to 3× the smaller secant (the Fritsch–Carlson bound).
        const double m = (d0 + d1) / 2.;
        const double lim = 3. * std::min(std::abs(d0), std::abs(d1));
        return std::copysign(std::min(std::abs(m), lim), d0);
    };

    const double d = delta(i);
    // Tension blends each endpoint tangent toward the plain secant. tension 0 on BOTH ends
    // ⇒ both tangents equal the secant ⇒ the segment is exactly a straight line, which is
    // what makes "tension 0" a predictable, verifiable value rather than an approximation.
    const double m0 = d + p0.tension * (tangent(i) - d);
    const double m1 = d + p1.tension * (tangent(i + 1) - d);

    const double t  = (z - p0.z) / h;
    const double t2 = t * t;
    const double t3 = t2 * t;
    // Hermite basis.
    return (2. * t3 - 3. * t2 + 1.) * p0.value + (t3 - 2. * t2 + t) * h * m0 +
           (-2. * t3 + 3. * t2) * p1.value + (t3 - t2) * h * m1;
}

} // namespace HeightAdaptive
} // namespace Slic3r
