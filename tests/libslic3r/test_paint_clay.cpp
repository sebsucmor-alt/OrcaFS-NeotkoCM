// Clay Tool core (libslic3r/PaintClay.hpp) against the Python reference
// (docs/WIP/clay_r0/clay_r0.py) and the Editor's --clay-test: same sphere, same star, same volumes.
#include <catch2/catch.hpp>

#include <libslic3r/PaintClay.hpp>

#include <map>

using namespace Slic3r;

namespace {

// Sphere of radius 20: icosahedron split 4 times, as a float32 soup (like the prototype).
std::vector<float> sphere_soup()
{
    const double t = (1. + std::sqrt(5.)) / 2.;
    std::vector<std::array<double, 3>> V = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t },
                                             { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
    auto norm = [](std::array<double, 3> p) { const double l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]); return std::array<double, 3> { p[0] / l, p[1] / l, p[2] / l }; };
    for (auto &p : V) p = norm(p);
    std::vector<std::array<int, 3>> F = { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 },
                                          { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 },
                                          { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };
    for (int it = 0; it < 4; ++it) {
        std::map<long, int> mid;
        auto m = [&](int a, int b) {
            const long k = long(std::min(a, b)) * 100000 + std::max(a, b);
            auto i = mid.find(k);
            if (i != mid.end()) return i->second;
            V.push_back(norm({ V[a][0] + V[b][0], V[a][1] + V[b][1], V[a][2] + V[b][2] }));
            mid[k] = int(V.size()) - 1;
            return int(V.size()) - 1;
        };
        std::vector<std::array<int, 3>> G;
        for (const auto &f : F) {
            const int a = f[0], b = f[1], c = f[2];
            const int ab = m(a, b), ca = m(c, a), bc = m(b, c);
            G.push_back({ a, ab, ca }); G.push_back({ b, bc, ab }); G.push_back({ c, ca, bc }); G.push_back({ ab, bc, ca });
        }
        F.swap(G);
    }
    std::vector<float> soup;
    for (const auto &f : F) for (int i : f) for (int k = 0; k < 3; ++k) soup.push_back(float(V[i][k] * 20.));
    return soup;
}

clay::Ring star(double R, double r)
{
    clay::Ring out;
    for (int i = 0; i < 10; ++i) {
        const double a = M_PI * i / 5. + M_PI / 2., q = i % 2 == 0 ? R : r;
        out.push_back({ q * std::cos(a), q * std::sin(a) });
    }
    return out;
}

} // namespace

TEST_CASE("Clay: star over a sphere, closed and with the reference volume", "[PaintClay]")
{
    const std::vector<float> soup = sphere_soup();
    const clay::Mesh m = clay::from_soup(soup.data(), soup.size() / 9);
    const clay::Card card({ 0, 0, 20 }, { 0, 0, 1 }, { 0, 1, 0 });
    struct Case { clay::Profile pf; double h, f, vol; };
    for (const Case &c : { Case { clay::Profile::Flat, 1., 0.6, 203.8974 }, Case { clay::Profile::Round, 1., 1.5, 174.4368 } }) {
        clay::Params q;
        q.profile = c.pf; q.height = c.h; q.falloff = c.f;
        clay::Log lg;
        const clay::Mesh r = clay::bake(m, card, { star(12, 5) }, q, lg);
        REQUIRE(!r.f.empty());
        CHECK(lg.open_edges == 0);
        CHECK(std::abs(lg.volume_mm3 - c.vol) < 0.05);
    }
}

TEST_CASE("Clay: visibility callback replaces the z-buffer", "[PaintClay]")
{
    const std::vector<float> soup = sphere_soup();
    const clay::Mesh m = clay::from_soup(soup.data(), soup.size() / 9);
    const clay::Card card({ 0, 0, 20 }, { 0, 0, 1 }, { 0, 1, 0 });
    clay::Params q;
    q.profile = clay::Profile::Flat; q.height = 1.;
    q.face_visible = [](int) { return true; };   // a convex sphere: every face that faces the card is seen
    clay::Log lg;
    const clay::Mesh r = clay::bake(m, card, { star(12, 5) }, q, lg);
    REQUIRE(!r.f.empty());
    CHECK(lg.open_edges == 0);
    CHECK(std::abs(lg.volume_mm3 - 203.8974) < 0.05);
    q.face_visible = [](int) { return false; };
    clay::Log none;
    CHECK(clay::bake(m, card, { star(12, 5) }, q, none).f.empty());
}
