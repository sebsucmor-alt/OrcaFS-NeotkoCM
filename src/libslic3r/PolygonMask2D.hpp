// A set of 2D rings (outer contours and holes) prepared for fast queries, used by TriangleSelector's
// PolygonSetProjectionCursor. The old PolygonProjectionCursor walked the whole polygon for every
// test (point inside, edge crossing, vertex in triangle), once per facet and per subdivision, and
// was called once per ring: a few SVG shapes with holes meant several full passes over the object.
// Here the rings go into a uniform grid once, so each query only looks at the edges near it.
//
// Fill rule: nonzero winding, with every outer ring forced counter-clockwise and every hole
// clockwise. Overlapping outers stay filled (union), a hole is empty unless another outer covers
// it. Coordinates are any 2D frame with Y down or up (only consistency matters).
//
// Header-only and dependency-free on purpose (std only, point types are read through .x()/.y()):
// it is tested on its own outside the build, and it adds nothing to libslic3r's link.
#ifndef slic3r_PolygonMask2D_hpp_
#define slic3r_PolygonMask2D_hpp_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Slic3r {

class PolygonMask2D
{
public:
    PolygonMask2D() = default;

    // rings[i] is a closed ring (last point connects back to the first, not repeated);
    // is_hole[i] says whether it is a hole. Rings with fewer than 3 points are skipped.
    template<class Rings>
    void build(const Rings &rings, const std::vector<bool> &is_hole)
    {
        m_ax.clear(); m_ay.clear(); m_bx.clear(); m_by.clear();
        m_vx.clear(); m_vy.clear();
        m_min_x = m_min_y = 0.f;
        m_max_x = m_max_y = -1.f;

        bool first = true;
        for (size_t r = 0; r < rings.size(); ++r) {
            const auto &ring = rings[r];
            const size_t n = ring.size();
            if (n < 3)
                continue;
            // Signed area (shoelace) to orient the ring: outers positive, holes negative.
            double area2 = 0.;
            for (size_t i = 0, j = n - 1; i < n; j = i++)
                area2 += double(ring[j].x()) * double(ring[i].y()) - double(ring[i].x()) * double(ring[j].y());
            const bool hole    = r < is_hole.size() && is_hole[r];
            const bool reverse = hole ? area2 > 0. : area2 < 0.;
            for (size_t k = 0; k < n; ++k) {
                const size_t i = reverse ? n - 1 - k : k;
                const size_t j = reverse ? (i == 0 ? n - 1 : i - 1) : (i + 1) % n;
                const float ax = float(ring[i].x()), ay = float(ring[i].y());
                const float bx = float(ring[j].x()), by = float(ring[j].y());
                m_ax.push_back(ax); m_ay.push_back(ay);
                m_bx.push_back(bx); m_by.push_back(by);
                m_vx.push_back(ax); m_vy.push_back(ay);
                if (first) {
                    m_min_x = m_max_x = ax;
                    m_min_y = m_max_y = ay;
                    first = false;
                }
                m_min_x = std::min(m_min_x, ax); m_max_x = std::max(m_max_x, ax);
                m_min_y = std::min(m_min_y, ay); m_max_y = std::max(m_max_y, ay);
            }
        }
        build_grid();
    }

    bool empty() const { return m_ax.empty(); }

    bool bbox_overlaps(float min_x, float min_y, float max_x, float max_y) const
    {
        return !empty() && max_x >= m_min_x && min_x <= m_max_x && max_y >= m_min_y && min_y <= m_max_y;
    }

    // Nonzero winding number of the point.
    bool inside(float px, float py) const
    {
        if (empty() || px < m_min_x || px > m_max_x || py < m_min_y || py > m_max_y)
            return false;
        int winding = 0;
        const int row = row_of(py);
        for (uint32_t k = m_row_start[row]; k < m_row_start[row + 1]; ++k) {
            const uint32_t e = m_row_edges[k];
            const float ax = m_ax[e], ay = m_ay[e], bx = m_bx[e], by = m_by[e];
            if (ay <= py) {
                if (by > py && side(ax, ay, bx, by, px, py) > 0.f)
                    ++winding;
            } else {
                if (by <= py && side(ax, ay, bx, by, px, py) < 0.f)
                    --winding;
            }
        }
        return winding != 0;
    }

    // True if the open segment [a,b] properly crosses any ring edge.
    bool segment_crosses_boundary(float ax, float ay, float bx, float by) const
    {
        if (!bbox_overlaps(std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by)))
            return false;
        int c0, r0, c1, r1;
        cell_range(std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by), c0, r0, c1, r1);
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c) {
                const int cell = r * m_nx + c;
                for (uint32_t k = m_cell_start[cell]; k < m_cell_start[cell + 1]; ++k) {
                    const uint32_t e = m_cell_edges[k];
                    if (segments_cross(ax, ay, bx, by, m_ax[e], m_ay[e], m_bx[e], m_by[e]))
                        return true;
                }
            }
        return false;
    }

    // True if any ring vertex lies inside the triangle (either winding).
    bool any_vertex_in_triangle(float x1, float y1, float x2, float y2, float x3, float y3) const
    {
        const float min_x = std::min({ x1, x2, x3 }), max_x = std::max({ x1, x2, x3 });
        const float min_y = std::min({ y1, y2, y3 }), max_y = std::max({ y1, y2, y3 });
        if (!bbox_overlaps(min_x, min_y, max_x, max_y))
            return false;
        int c0, r0, c1, r1;
        cell_range(min_x, min_y, max_x, max_y, c0, r0, c1, r1);
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c) {
                const int cell = r * m_nx + c;
                for (uint32_t k = m_vcell_start[cell]; k < m_vcell_start[cell + 1]; ++k) {
                    const uint32_t v = m_vcell_verts[k];
                    if (point_in_triangle(m_vx[v], m_vy[v], x1, y1, x2, y2, x3, y3))
                        return true;
                }
            }
        return false;
    }

    size_t edge_count() const { return m_ax.size(); }

private:
    static float side(float ax, float ay, float bx, float by, float px, float py)
    {
        return (bx - ax) * (py - ay) - (px - ax) * (by - ay);
    }

    static bool segments_cross(float p1x, float p1y, float p2x, float p2y, float p3x, float p3y, float p4x, float p4y)
    {
        const float d1 = side(p3x, p3y, p4x, p4y, p1x, p1y);
        const float d2 = side(p3x, p3y, p4x, p4y, p2x, p2y);
        const float d3 = side(p1x, p1y, p2x, p2y, p3x, p3y);
        const float d4 = side(p1x, p1y, p2x, p2y, p4x, p4y);
        return ((d1 > 0.f && d2 < 0.f) || (d1 < 0.f && d2 > 0.f)) && ((d3 > 0.f && d4 < 0.f) || (d3 < 0.f && d4 > 0.f));
    }

    static bool point_in_triangle(float px, float py, float x1, float y1, float x2, float y2, float x3, float y3)
    {
        const float d1 = side(x1, y1, x2, y2, px, py);
        const float d2 = side(x2, y2, x3, y3, px, py);
        const float d3 = side(x3, y3, x1, y1, px, py);
        const bool has_neg = d1 < 0.f || d2 < 0.f || d3 < 0.f;
        const bool has_pos = d1 > 0.f || d2 > 0.f || d3 > 0.f;
        return !(has_neg && has_pos);
    }

    int col_of(float x) const { return std::clamp(int((x - m_min_x) * m_inv_cw), 0, m_nx - 1); }
    int row_of(float y) const { return std::clamp(int((y - m_min_y) * m_inv_ch), 0, m_ny - 1); }

    void cell_range(float min_x, float min_y, float max_x, float max_y, int &c0, int &r0, int &c1, int &r1) const
    {
        c0 = col_of(min_x); c1 = col_of(max_x);
        r0 = row_of(min_y); r1 = row_of(max_y);
    }

    // Counting sort of (bucket, item) pairs into compressed rows: start[b]..start[b+1] in items.
    static void compress(int buckets, const std::vector<std::pair<int, uint32_t>> &pairs,
                         std::vector<uint32_t> &start, std::vector<uint32_t> &items)
    {
        start.assign(size_t(buckets) + 1, 0);
        for (const auto &p : pairs)
            ++start[size_t(p.first) + 1];
        for (int b = 0; b < buckets; ++b)
            start[size_t(b) + 1] += start[size_t(b)];
        items.assign(pairs.size(), 0);
        std::vector<uint32_t> fill(start.begin(), start.end() - 1);
        for (const auto &p : pairs)
            items[fill[size_t(p.first)]++] = p.second;
    }

    void build_grid()
    {
        const size_t edges = m_ax.size();
        if (edges == 0) {
            m_nx = m_ny = 1;
            m_inv_cw = m_inv_ch = 0.f;
            m_row_start.assign(2, 0);   m_row_edges.clear();
            m_cell_start.assign(2, 0);  m_cell_edges.clear();
            m_vcell_start.assign(2, 0); m_vcell_verts.clear();
            return;
        }
        // About one cell per edge, shaped like the bounding box, capped so huge SVGs stay small.
        const float w = std::max(m_max_x - m_min_x, 1e-6f);
        const float h = std::max(m_max_y - m_min_y, 1e-6f);
        const float cells = float(edges);
        m_nx = std::clamp(int(std::ceil(std::sqrt(cells * w / h))), 1, 512);
        m_ny = std::clamp(int(std::ceil(std::sqrt(cells * h / w))), 1, 512);
        m_inv_cw = float(m_nx) / w;
        m_inv_ch = float(m_ny) / h;

        std::vector<std::pair<int, uint32_t>> row_pairs, cell_pairs, vert_pairs;
        row_pairs.reserve(edges * 2);
        cell_pairs.reserve(edges * 4);
        vert_pairs.reserve(edges);
        for (uint32_t e = 0; e < uint32_t(edges); ++e) {
            int c0, r0, c1, r1;
            cell_range(std::min(m_ax[e], m_bx[e]), std::min(m_ay[e], m_by[e]),
                       std::max(m_ax[e], m_bx[e]), std::max(m_ay[e], m_by[e]), c0, r0, c1, r1);
            for (int r = r0; r <= r1; ++r) {
                row_pairs.emplace_back(r, e);
                for (int c = c0; c <= c1; ++c)
                    cell_pairs.emplace_back(r * m_nx + c, e);
            }
            vert_pairs.emplace_back(row_of(m_vy[e]) * m_nx + col_of(m_vx[e]), e);
        }
        compress(m_ny, row_pairs, m_row_start, m_row_edges);
        compress(m_nx * m_ny, cell_pairs, m_cell_start, m_cell_edges);
        compress(m_nx * m_ny, vert_pairs, m_vcell_start, m_vcell_verts);
    }

    // Edges as struct-of-arrays, oriented (outer CCW, hole CW). m_vx/m_vy = ring vertices.
    std::vector<float> m_ax, m_ay, m_bx, m_by;
    std::vector<float> m_vx, m_vy;
    float m_min_x = 0.f, m_min_y = 0.f, m_max_x = -1.f, m_max_y = -1.f;

    int   m_nx = 1, m_ny = 1;
    float m_inv_cw = 0.f, m_inv_ch = 0.f;
    std::vector<uint32_t> m_row_start, m_row_edges;      // edges per row band (winding test)
    std::vector<uint32_t> m_cell_start, m_cell_edges;    // edges per cell (crossing test)
    std::vector<uint32_t> m_vcell_start, m_vcell_verts;  // ring vertices per cell
};

} // namespace Slic3r

#endif // slic3r_PolygonMask2D_hpp_
