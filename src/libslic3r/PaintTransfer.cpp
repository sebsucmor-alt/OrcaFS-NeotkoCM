// NEOTKO_PAINTCARRY_TAG — see PaintTransfer.hpp.
#include "PaintTransfer.hpp"

#include "AABBTreeIndirect.hpp"
#include "Model.hpp"
#include "TriangleSelector.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <cmath>
#include <memory>
#include <functional>

#include <tbb/enumerable_thread_specific.h>

namespace Slic3r {
namespace PaintTransfer {

// Only a safety floor (mm, in target mesh coords): the resolution comes from the painting being
// copied (see TriangleSelector::resample), and the brush can paint finer than 0.05 with a high
// precision setting.
static constexpr float EDGE_LIMIT = 0.005f;

int effective_filament(const ModelVolume &volume)
{
    if (volume.config.has("extruder") && volume.config.opt_int("extruder") > 0)
        return volume.config.opt_int("extruder");
    const ModelObject *object = volume.get_object();
    if (object != nullptr && object->config.has("extruder") && object->config.opt_int("extruder") > 0)
        return object->config.opt_int("extruder");
    return 1;
}

bool has_painting(const ModelVolume &volume)
{
    return volume.is_fdm_support_painted() || volume.is_seam_painted() || volume.is_mm_painted() || volume.is_fuzzy_skin_painted();
}

bool worth_transferring(const std::vector<Source> &sources)
{
    for (const Source &src : sources)
        if (has_painting(*src.volume) || (!src.cutter && src.filament != sources.front().filament))
            return true;
    return false;
}

void append_object_sources(const ModelObject &object, bool cutter, std::vector<Source> &out)
{
    if (object.instances.empty())
        return;
    const Transform3d instance = object.instances.front()->get_matrix();
    for (const ModelVolume *volume : object.volumes)
        if (volume->is_model_part() && ! volume->mesh().empty()) {
            Source src;
            src.volume   = volume;
            src.to_world = instance * volume->get_matrix();
            src.filament = effective_filament(*volume);
            src.cutter   = cutter;
            out.push_back(src);
        }
}

namespace {

using Layer = FacetsAnnotation ModelVolume::*;

// The paint layers every Orca build has. The colour layer is also where the filament of each
// input goes.
const Layer COLOUR_LAYER = &ModelVolume::mmu_segmentation_facets;
const Layer LAYERS[]     = { &ModelVolume::supported_facets, &ModelVolume::seam_facets,
                             &ModelVolume::mmu_segmentation_facets, &ModelVolume::fuzzy_skin_facets };

struct PreparedSource
{
    const Source                 *src = nullptr;
    indexed_triangle_set          world_its; // same facet order as the volume mesh
    AABBTreeIndirect::Tree3f      tree;
    Transform3f                   to_local = Transform3f::Identity();
    float                         area_to_world = 1.f; // source mesh area -> world area
    Eigen::AlignedBox3f           bbox;                // of world_its, to skip sources that are far away
    std::vector<Vec3i32>          neighbors;           // edge neighbours of each facet, for the hint walk
};

struct Hit
{
    int   source = -1;
    int   facet  = -1;
    Vec3f local  = Vec3f::Zero(); // point on the source, in its volume mesh coords
};

} // namespace

struct Transfer::Impl
{
    std::vector<Source>         sources;
    std::vector<PreparedSource> prepared;
    // The painting of each source, per paint layer, loaded on first use.
    std::vector<std::vector<std::unique_ptr<TriangleSelector>>> selectors; // [layer][source]
    std::vector<bool>                                           loaded;    // [layer]
};

Transfer::Transfer(std::vector<Source> sources) : m(new Impl)
{
    m->sources = std::move(sources);
    m->prepared.resize(m->sources.size());
    for (size_t i = 0; i < m->sources.size(); ++i) {
        PreparedSource &p = m->prepared[i];
        p.src             = &m->sources[i];
        p.world_its       = m->sources[i].volume->mesh().its;
        its_transform(p.world_its, m->sources[i].to_world);
        p.tree          = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(p.world_its.vertices, p.world_its.indices);
        p.neighbors = its_face_neighbors(p.world_its);
        p.bbox.setEmpty();
        for (const Vec3f &v : p.world_its.vertices)
            p.bbox.extend(v);
        p.to_local      = m->sources[i].to_world.inverse().cast<float>();
        p.area_to_world = float(std::pow(std::abs(m->sources[i].to_world.linear().determinant()), 2. / 3.));
    }
    m->selectors.resize(std::size(LAYERS));
    m->loaded.assign(std::size(LAYERS), false);
}

Transfer::~Transfer() = default;

Stats transfer(const std::vector<Source> &sources, ModelVolume &target, const Transform3d &target_to_world, int base_filament,
               float reach)
{
    const auto t0 = std::chrono::steady_clock::now();
    Transfer   transfer(sources);
    const long prepare_ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    Stats      stats      = transfer.to(target, target_to_world, base_filament, nullptr, reach);
    stats.prepare_ms      = prepare_ms;
    return stats;
}

// Where the bits of each facet are in serialized painting: facet -> [begin, end).
static std::unordered_map<int, std::pair<int, int>> bits_by_facet(const TriangleSelector::TriangleSplittingData &data)
{
    std::vector<TriangleSelector::TriangleBitStreamMapping> by_start = data.triangles_to_split;
    std::sort(by_start.begin(), by_start.end(), [](const auto &l, const auto &r) { return l.bitstream_start_idx < r.bitstream_start_idx; });
    std::unordered_map<int, std::pair<int, int>> bits;
    bits.reserve(by_start.size());
    for (size_t i = 0; i < by_start.size(); ++i)
        bits[by_start[i].triangle_idx] = { by_start[i].bitstream_start_idx,
                                           i + 1 < by_start.size() ? by_start[i + 1].bitstream_start_idx : int(data.bitstream.size()) };
    return bits;
}

Stats Transfer::to(ModelVolume &target, const Transform3d &target_to_world, int base_filament, const std::vector<int> *facet_map,
                   float reach)
{
    Stats stats;
    const std::vector<Source>         &sources  = m->sources;
    const std::vector<PreparedSource> &prepared = m->prepared;
    if (sources.empty() || target.mesh().empty())
        return stats;
    // A facet copied as it is keeps the source's painting, so this only holds for one source that
    // prints with the target's filament (else its unpainted skin would need colouring).
    if (facet_map && (sources.size() != 1 || facet_map->size() != target.mesh().its.indices.size() ||
                      sources.front().filament != base_filament))
        facet_map = nullptr;
    std::vector<bool> copied;
    if (facet_map) {
        copied.resize(facet_map->size());
        for (size_t i = 0; i < facet_map->size(); ++i)
            copied[i] = (*facet_map)[i] >= 0 || (*facet_map)[i] == FACET_UNPAINTED;
    }

    // Two skins closer than this count as the same place, and the earlier source wins.
    const BoundingBoxf3 bbox = target.mesh().transformed_bounding_box(target_to_world);
    const float         tie  = std::max(1e-4f, float(bbox.size().norm()) * 1e-6f);
    // The result's skin lies on the skin of the sources (the boolean only nudges vertices a few
    // microns). Anything farther than this has no source and stays unpainted.
    const float         max_dist = std::max({ 0.05f, float(bbox.size().norm()) * 1e-3f, reach });
    // How far off a remembered source facet a point may lie and still take it without a search
    // (NEOTKO_REMESH_TAG: a remeshed skin never lies exactly on the old one).
    const float         hint_tol = std::max(tie, reach);
    const Transform3f   t2w  = target_to_world.cast<float>();
    const float         world_to_target_area = 1.f / float(std::pow(std::abs(target_to_world.linear().determinant()), 2. / 3.));

    // The sampler runs in parallel (TriangleSelector::resample): everything it touches is read
    // only, except this counter.
    // Per thread, summed at the end: a shared counter hit by every thread for every point costs
    // more than the counting is worth.
    tbb::enumerable_thread_specific<size_t> samples(0), searches(0), tie_searches(0);
    auto ms_since = [](std::chrono::steady_clock::time_point t) {
        return long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count());
    };
    // Nearest point on source `i`, searching its whole mesh.
    auto query = [&](size_t i, const Vec3f &world, float &d, int &facet_out, Vec3f &on_mesh) {
        size_t      facet = 0;
        const float d2    = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
            prepared[i].world_its.vertices, prepared[i].world_its.indices, prepared[i].tree, world, facet, on_mesh);
        if (d2 < 0.f)
            return false;
        d         = std::sqrt(d2);
        facet_out = int(facet);
        return true;
    };

    // The boolean only cuts along the line where the inputs meet, so nearly every triangle of the
    // result lies inside ONE source triangle, and so do its samples and its splits. Each thread
    // remembers the last source triangle it hit and tries it first: a point-in-triangle test in
    // place of a search over the whole mesh.
    struct HintCache { const void *owner = nullptr; uint64_t call = 0; int source = -1; int facet = -1; };
    static std::atomic<uint64_t> calls { 0 };
    const uint64_t               this_call = ++calls;
    thread_local HintCache       hint;
    // Distance of a recent point to the sources listed before the one it hit, for the tie check.
    struct TieCache { const void *owner = nullptr; uint64_t call = 0; int source = -1; Vec3f p = Vec3f::Zero(); float d = 0.f; };
    thread_local TieCache        tie_far;

    // Whether `world` lies on facet `facet` of source `src` (within the tie distance of its plane,
    // inside its edges); if so, the projected point and the distance.
    auto on_facet = [&](int src, int facet, const Vec3f &world, Vec3f &proj, float &dist, float tol) {
        const PreparedSource              &p = prepared[src];
        const stl_triangle_vertex_indices &f = p.world_its.indices[facet];
        const Vec3f a = p.world_its.vertices[f[0]], b = p.world_its.vertices[f[1]], c = p.world_its.vertices[f[2]];
        Vec3f       n   = (b - a).cross(c - a);
        const float len = n.norm();
        if (len < 1e-12f)
            return false;
        n /= len;
        dist = (world - a).dot(n);
        if (std::abs(dist) > tol)
            return false;
        proj = world - n * dist;
        // Barycentric coordinates of the projected point.
        const Vec3f v0 = b - a, v1 = c - a, v2 = proj - a;
        const float d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1), d20 = v2.dot(v0), d21 = v2.dot(v1);
        const float den = d00 * d11 - d01 * d01;
        if (std::abs(den) < 1e-20f)
            return false;
        const float v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den;
        return std::min({ 1.f - v - w, v, w }) >= -1e-4f;
    };

    auto on_hint = [&](const Vec3f &world, Hit &hit, float &d) {
        if (hint.owner != &prepared || hint.call != this_call || hint.source < 0)
            return false;
        const PreparedSource &p = prepared[hint.source];
        // The remembered facet first, then its edge neighbours: a triangle of the result that the
        // boolean re-triangulated can straddle two facets of the source.
        Vec3f proj;
        float dist  = 0.f;
        int   facet = hint.facet;
        if (! on_facet(hint.source, facet, world, proj, dist, hint_tol)) {
            facet = -1;
            for (int k = 0; k < 3 && facet < 0; ++k) {
                const int nb = p.neighbors[hint.facet](k);
                if (nb >= 0 && on_facet(hint.source, nb, world, proj, dist, hint_tol))
                    facet = nb;
            }
            if (facet < 0)
                return false;
        }
        // An earlier source on the same spot would win the tie: only skip the search when there
        // is none.
        if (hint.source > 0) {
            // The last point this thread checked was `tie_far.d` away from every earlier source;
            // this one is at least that minus how far it moved (triangle inequality). Only search
            // again when that bound no longer settles it.
            const float need = std::abs(dist) + tie;
            if (! (tie_far.owner == &prepared && tie_far.call == this_call && tie_far.source == hint.source &&
                   tie_far.d - (world - tie_far.p).norm() > need)) {
                float nearest = std::numeric_limits<float>::max();
                for (int i = 0; i < hint.source; ++i) {
                    // Cheap first: the box bounds the distance from below.
                    const float box_d = prepared[i].bbox.exteriorDistance(world);
                    if (box_d > need) {
                        nearest = std::min(nearest, box_d);
                        continue;
                    }
                    float e; int ef; Vec3f eon;
                    ++tie_searches.local();
                    if (query(size_t(i), world, e, ef, eon)) {
                        if (e <= need)
                            return false;
                        nearest = std::min(nearest, e);
                    }
                }
                tie_far = TieCache{ &prepared, this_call, hint.source, world, nearest };
            }
        }
        hit.source = hint.source;
        hit.facet  = facet;
        hit.local  = p.to_local * proj;
        d          = std::abs(dist);
        hint.facet = facet;
        return true;
    };

    auto closest = [&](const Vec3f &target_pt) {
        ++samples.local();
        const Vec3f world = t2w * target_pt;
        Hit         best;
        float       best_d = 0.f;
        if (on_hint(world, best, best_d))
            return best;
        ++searches.local();
        best_d = std::numeric_limits<float>::max();
        for (size_t i = 0; i < prepared.size(); ++i) {
            // A source whose box is already farther than the best hit cannot win.
            if (best.source >= 0 && prepared[i].bbox.exteriorDistance(world) > best_d)
                continue;
            float d; int facet; Vec3f on_mesh;
            if (! query(i, world, d, facet, on_mesh))
                continue;
            if (best.source < 0 || d < best_d - tie) {
                best_d      = d;
                best.source = int(i);
                best.facet  = facet;
                best.local  = prepared[i].to_local * on_mesh;
            }
        }
        if (best.source >= 0 && best_d > max_dist)
            best.source = -1;
        if (best.source >= 0)
            hint = HintCache{ &prepared, this_call, best.source, best.facet };
        return best;
    };

    for (size_t layer_idx = 0; layer_idx < std::size(LAYERS); ++layer_idx) {
        const Layer layer  = LAYERS[layer_idx];
        const bool  colour = layer == COLOUR_LAYER;

        // Load the painting of this layer from every source that has some (once for all targets).
        std::vector<std::unique_ptr<TriangleSelector>> &selectors = m->selectors[layer_idx];
        const auto t_load = std::chrono::steady_clock::now();
        if (! m->loaded[layer_idx]) {
            selectors.resize(sources.size());
            for (size_t i = 0; i < sources.size(); ++i) {
                const FacetsAnnotation &fa = sources[i].volume->*layer;
                if (! fa.empty()) {
                    selectors[i] = std::make_unique<TriangleSelector>(sources[i].volume->mesh());
                    selectors[i]->deserialize(fa.get_data());
                }
            }
            m->loaded[layer_idx] = true;
        }
        stats.load_ms += ms_since(t_load);
        bool anything = false;
        for (size_t i = 0; i < sources.size(); ++i)
            if (selectors[i] || (colour && ! sources[i].cutter && sources[i].filament != base_filament))
                anything = true;
        if (! anything) {
            (target.*layer).reset();
            continue;
        }

        const auto       t_resample = std::chrono::steady_clock::now();
        TriangleSelector selector(target.mesh());
        selector.resample([&](const Vec3f &pt) {
            TriangleSelector::SampledState out;
            const Hit hit = closest(pt);
            if (hit.source < 0 || sources[hit.source].cutter)
                return out;
            if (selectors[hit.source]) {
                out.state = selectors[hit.source]->leaf_state_at(hit.facet, hit.local, &out.leaf_area);
                out.leaf_area *= prepared[hit.source].area_to_world * world_to_target_area;
            }
            if (colour && out.state == EnforcerBlockerType::NONE && sources[hit.source].filament != base_filament)
                out.state = EnforcerBlockerType(sources[hit.source].filament);
            return out;
        }, EDGE_LIMIT, facet_map ? &copied : nullptr);
        stats.resample_ms += ms_since(t_resample);

        const auto t_store = std::chrono::steady_clock::now();
        if (facet_map) {
            // Splice: copied facets take the source's bits, the others the resampled ones.
            const FacetsAnnotation &from = sources.front().volume->*layer;
            const TriangleSelector::TriangleSplittingData resampled = selector.serialize();
            const auto src_bits = from.empty() ? std::unordered_map<int, std::pair<int, int>>{} : bits_by_facet(from.get_data());
            const auto new_bits = bits_by_facet(resampled);
            TriangleSelector::TriangleSplittingData out;
            for (int t = 0; t < int(facet_map->size()); ++t) {
                const int  src_facet = (*facet_map)[t];
                if (src_facet == FACET_UNPAINTED)
                    continue;
                const auto &bits     = src_facet >= 0 ? src_bits : new_bits;
                auto        it       = bits.find(src_facet >= 0 ? src_facet : t);
                if (it == bits.end())
                    continue;
                const std::vector<bool> &stream = src_facet >= 0 ? from.get_data().bitstream : resampled.bitstream;
                out.triangles_to_split.emplace_back(t, int(out.bitstream.size()));
                out.bitstream.insert(out.bitstream.end(), stream.begin() + it->second.first, stream.begin() + it->second.second);
            }
            if (out.triangles_to_split.empty()) {
                (target.*layer).reset();
            } else {
                TriangleSelector spliced(target.mesh());
                spliced.deserialize(out);
                (target.*layer).set(spliced);
            }
        } else
            (target.*layer).set(selector);
        stats.store_ms += ms_since(t_store);
        ++stats.layers;
    }
    stats.samples      = samples.combine(std::plus<size_t>());
    stats.searches     = searches.combine(std::plus<size_t>());
    stats.tie_searches = tie_searches.combine(std::plus<size_t>());
    return stats;
}


namespace {
// The three corners of a facet, bit for bit.
struct FacetKey
{
    std::array<float, 9> v;
    bool operator==(const FacetKey &o) const { return std::memcmp(v.data(), o.v.data(), sizeof(float) * 9) == 0; }
};
struct FacetKeyHash
{
    size_t operator()(const FacetKey &k) const
    {
        size_t h = 1469598103934665603ull;
        for (float f : k.v) {
            uint32_t u;
            std::memcpy(&u, &f, sizeof(u));
            h = (h ^ u) * 1099511628211ull;
        }
        return h;
    }
};
FacetKey facet_key(const indexed_triangle_set &its, size_t facet)
{
    FacetKey k;
    for (int c = 0; c < 3; ++c)
        for (int d = 0; d < 3; ++d)
            k.v[c * 3 + d] = its.vertices[its.indices[facet][c]](d);
    return k;
}
} // namespace

struct FacetMatcher::Impl
{
    std::unordered_map<FacetKey, int, FacetKeyHash> by_key;
};

FacetMatcher::FacetMatcher(const indexed_triangle_set &source) : m(new Impl)
{
    m->by_key.reserve(source.indices.size());
    for (size_t i = 0; i < source.indices.size(); ++i)
        m->by_key.emplace(facet_key(source, i), int(i));
}

FacetMatcher::~FacetMatcher() = default;

std::vector<int> FacetMatcher::match(const indexed_triangle_set &piece) const
{
    std::vector<int> map(piece.indices.size(), -1);
    for (size_t i = 0; i < piece.indices.size(); ++i)
        if (auto it = m->by_key.find(facet_key(piece, i)); it != m->by_key.end())
            map[i] = it->second;
    return map;
}

bool copy_painting(const ModelVolume &source, ModelVolume &target, const std::vector<int> &facet_map)
{
    if (facet_map.size() != target.mesh().its.indices.size() ||
        std::find(facet_map.begin(), facet_map.end(), -1) != facet_map.end())
        return false;

    for (const Layer layer : LAYERS) {
        const FacetsAnnotation &from = source.*layer;
        FacetsAnnotation       &to   = target.*layer;
        if (from.empty()) {
            to.reset();
            continue;
        }
        // Where the bits of each source facet are: from its start to the start of the next one.
        const TriangleSelector::TriangleSplittingData &data = from.get_data();
        const std::unordered_map<int, std::pair<int, int>> bits = bits_by_facet(data); // source facet -> [begin, end)

        TriangleSelector::TriangleSplittingData out;
        for (size_t t = 0; t < facet_map.size(); ++t) {
            auto it = bits.find(facet_map[t]);
            if (it == bits.end())
                continue;
            out.triangles_to_split.emplace_back(int(t), int(out.bitstream.size()));
            out.bitstream.insert(out.bitstream.end(), data.bitstream.begin() + it->second.first, data.bitstream.begin() + it->second.second);
        }
        if (out.triangles_to_split.empty()) {
            to.reset();
            continue;
        }
        TriangleSelector selector(target.mesh());
        selector.deserialize(out);
        to.set(selector);
    }
    return true;
}

void set_mesh_keep_painting(ModelVolume &volume, TriangleMesh &&mesh)
{
    if (! has_painting(volume)) {
        volume.set_mesh(std::move(mesh));
        return;
    }
    // Keep the old mesh and its painting in a copy while the volume takes the new one.
    Model        tmp;
    ModelObject *tmp_object = tmp.add_object();
    ModelVolume *old        = tmp_object->add_volume(volume);
    Source src;
    src.volume   = old;
    src.to_world = volume.get_matrix();
    src.filament = effective_filament(volume);

    volume.set_mesh(std::move(mesh));

    const std::vector<int> facet_map = FacetMatcher(old->mesh().its).match(volume.mesh().its);
    Transfer(std::vector<Source>{ src }).to(volume, volume.get_matrix(), src.filament, &facet_map);
}

} // namespace PaintTransfer
} // namespace Slic3r
