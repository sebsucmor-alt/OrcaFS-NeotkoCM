// NEOTKO_SVGDECAL_TAG — F1: PolygonMask2D (grid-accelerated) must answer exactly like a brute
// force walk over every edge, with the same orientation rule (outers CCW, holes CW, nonzero).
#include "catch_main.hpp"
#include "libslic3r/PolygonMask2D.hpp"
#include "libslic3r/Point.hpp"

#include <cmath>
#include <random>
#include <vector>

using namespace Slic3r;

namespace {

using Ring = std::vector<Vec2f>;

float side(const Vec2f &a, const Vec2f &b, const Vec2f &p)
{
    return (b.x() - a.x()) * (p.y() - a.y()) - (p.x() - a.x()) * (b.y() - a.y());
}

struct BruteMask
{
    std::vector<Vec2f> a, b;

    BruteMask(const std::vector<Ring> &rings, const std::vector<bool> &hole)
    {
        for (size_t r = 0; r < rings.size(); ++r) {
            const Ring &g = rings[r];
            const size_t n = g.size();
            if (n < 3)
                continue;
            double area2 = 0.;
            for (size_t i = 0, j = n - 1; i < n; j = i++)
                area2 += double(g[j].x()) * g[i].y() - double(g[i].x()) * g[j].y();
            const bool reverse = hole[r] ? area2 > 0. : area2 < 0.;
            for (size_t k = 0; k < n; ++k) {
                const size_t i = reverse ? n - 1 - k : k;
                const size_t j = reverse ? (i == 0 ? n - 1 : i - 1) : (i + 1) % n;
                a.push_back(g[i]);
                b.push_back(g[j]);
            }
        }
    }

    bool inside(const Vec2f &p) const
    {
        int w = 0;
        for (size_t e = 0; e < a.size(); ++e) {
            if (a[e].y() <= p.y()) {
                if (b[e].y() > p.y() && side(a[e], b[e], p) > 0.f)
                    ++w;
            } else if (b[e].y() <= p.y() && side(a[e], b[e], p) < 0.f)
                --w;
        }
        return w != 0;
    }

    bool crosses(const Vec2f &p, const Vec2f &q) const
    {
        for (size_t e = 0; e < a.size(); ++e) {
            const float d1 = side(a[e], b[e], p), d2 = side(a[e], b[e], q);
            const float d3 = side(p, q, a[e]), d4 = side(p, q, b[e]);
            if (((d1 > 0.f && d2 < 0.f) || (d1 < 0.f && d2 > 0.f)) && ((d3 > 0.f && d4 < 0.f) || (d3 < 0.f && d4 > 0.f)))
                return true;
        }
        return false;
    }

    bool vertex_in_triangle(const Vec2f &t1, const Vec2f &t2, const Vec2f &t3) const
    {
        for (const Vec2f &p : a) {
            const float d1 = side(t1, t2, p), d2 = side(t2, t3, p), d3 = side(t3, t1, p);
            const bool neg = d1 < 0.f || d2 < 0.f || d3 < 0.f;
            const bool pos = d1 > 0.f || d2 > 0.f || d3 > 0.f;
            if (!(neg && pos))
                return true;
        }
        return false;
    }
};

Ring circle(float cx, float cy, float r, int n, bool clockwise)
{
    Ring g;
    for (int i = 0; i < n; ++i) {
        const float t = (clockwise ? -1.f : 1.f) * 6.2831853f * float(i) / float(n);
        g.emplace_back(cx + r * std::cos(t), cy + r * std::sin(t));
    }
    return g;
}

// Counts the queries where the grid and the brute force disagree.
int mismatches(const std::vector<Ring> &rings, const std::vector<bool> &hole)
{
    PolygonMask2D mask;
    mask.build(rings, hole);
    const BruteMask ref(rings, hole);

    std::mt19937 rng(7);
    std::uniform_real_distribution<float> pos(-150.f, 150.f), step(-20.f, 20.f);
    int bad = 0;
    for (int i = 0; i < 20000; ++i) {
        const Vec2f p(pos(rng), pos(rng));
        const Vec2f q = p + Vec2f(step(rng), step(rng));
        const Vec2f r = p + Vec2f(step(rng), step(rng));
        if (mask.inside(p.x(), p.y()) != ref.inside(p))
            ++bad;
        if (mask.segment_crosses_boundary(p.x(), p.y(), q.x(), q.y()) != ref.crosses(p, q))
            ++bad;
        if (mask.any_vertex_in_triangle(p.x(), p.y(), q.x(), q.y(), r.x(), r.y()) != ref.vertex_in_triangle(p, q, r))
            ++bad;
    }
    return bad;
}

} // namespace

TEST_CASE("PolygonMask2D: a hole is empty even when given with the outer's orientation", "[PolygonMask2D]")
{
    const std::vector<Ring> o = { circle(0.f, 0.f, 100.f, 333, false), circle(0.f, 0.f, 50.f, 240, false) };
    PolygonMask2D mask;
    mask.build(o, { false, true });
    CHECK(mask.inside(75.f, 0.f));
    CHECK_FALSE(mask.inside(0.f, 0.f));
    CHECK_FALSE(mask.inside(120.f, 0.f));
    REQUIRE(mismatches(o, { false, true }) == 0);
}

TEST_CASE("PolygonMask2D: overlapping outers are a union, another outer fills a hole", "[PolygonMask2D]")
{
    const std::vector<Ring> rings = { circle(-30.f, 0.f, 70.f, 200, false), circle(30.f, 0.f, 70.f, 200, true),
                                      circle(-30.f, 0.f, 30.f, 100, false) };
    const std::vector<bool> hole  = { false, false, true };
    PolygonMask2D mask;
    mask.build(rings, hole);
    // Inside the hole of the first circle but covered by the second one.
    CHECK(mask.inside(-10.f, 0.f));
    // Inside the hole and outside the second circle.
    CHECK_FALSE(mask.inside(-50.f, 0.f));
    REQUIRE(mismatches(rings, hole) == 0);
}

TEST_CASE("PolygonMask2D: concave rings match the brute force", "[PolygonMask2D]")
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> radius(0.3f, 1.f);
    std::vector<Ring> rings;
    std::vector<bool> hole;
    for (int k = 0; k < 12; ++k) {
        Ring g;
        for (int i = 0; i < 150; ++i) {
            const float t = 6.2831853f * float(i) / 150.f;
            const float r = 35.f * radius(rng);
            g.emplace_back(float(k % 4) * 60.f - 90.f + r * std::cos(t), float(k / 4) * 60.f - 60.f + r * std::sin(t));
        }
        rings.push_back(std::move(g));
        hole.push_back(k % 5 == 4);
    }
    REQUIRE(mismatches(rings, hole) == 0);
}

TEST_CASE("PolygonMask2D: rings with fewer than 3 points are skipped", "[PolygonMask2D]")
{
    const std::vector<Ring> rings = { Ring{ Vec2f(0.f, 0.f), Vec2f(1.f, 0.f) }, circle(10.f, 10.f, 40.f, 64, false) };
    PolygonMask2D mask;
    mask.build(rings, { false, false });
    CHECK(mask.edge_count() == 64);
    REQUIRE(mismatches(rings, { false, false }) == 0);
}

TEST_CASE("PolygonMask2D: empty mask answers no to everything", "[PolygonMask2D]")
{
    PolygonMask2D mask;
    mask.build(std::vector<Ring>{}, {});
    CHECK(mask.empty());
    CHECK_FALSE(mask.inside(0.f, 0.f));
    CHECK_FALSE(mask.segment_crosses_boundary(-1.f, -1.f, 1.f, 1.f));
    CHECK_FALSE(mask.any_vertex_in_triangle(-1.f, -1.f, 1.f, -1.f, 0.f, 1.f));
}
