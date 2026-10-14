// Carry the painting of a mesh over to another one: the result of a boolean, the pieces of a split.
#ifndef slic3r_PaintTransfer_hpp_
#define slic3r_PaintTransfer_hpp_

#include "Point.hpp"
#include "TriangleMesh.hpp"

#include <memory>
#include <vector>

namespace Slic3r {

class ModelObject;
class ModelVolume;

namespace PaintTransfer {

// One piece the painting comes from: a model part of one of the inputs of the boolean.
struct Source
{
    const ModelVolume *volume   = nullptr;
    Transform3d        to_world = Transform3d::Identity(); // volume mesh coords -> world
    // Filament the unpainted skin of this piece prints with (1-based). The unpainted skin of the
    // result is coloured with it, so the result keeps the colour each input had.
    int filament = 1;
    // The tool of a Difference: what is left of its skin in the result is the wall of the cut,
    // which stays unpainted (prints with the result's own filament).
    bool cutter = false;
};

struct Stats
{
    size_t layers  = 0; // paint layers written on the target
    size_t samples = 0; // points looked up on the sources
    size_t searches = 0; // of those, the ones that needed a search over a whole source mesh
    size_t tie_searches = 0; // searches over earlier sources for the tie check
    // Where the time goes (ms): preparing the sources, loading their painting, resampling, storing.
    long   prepare_ms = 0, load_ms = 0, resample_ms = 0, store_ms = 0;
};

// Whether the volume has any painting this module carries (support, seam, colour, fuzzy skin).
bool has_painting(const ModelVolume &volume);
// Whether transfer() from these sources has anything to do: some painting, or inputs with
// different filaments (whose unpainted skin then gets painted). Used to decide whether to tell
// the user to wait.
bool worth_transferring(const std::vector<Source> &sources);

// Filament a model part prints with: its own, else its object's, else the first one.
int effective_filament(const ModelVolume &volume);

// Every model part of `object`, placed with its first instance.
void append_object_sources(const ModelObject &object, bool cutter, std::vector<Source> &out);

// The sources prepared once (search trees, painting loaded) for several targets, as when one
// mesh is split into many pieces.
class Transfer
{
public:
    explicit Transfer(std::vector<Source> sources);
    ~Transfer();
    // See transfer() below.
    Stats to(ModelVolume &target, const Transform3d &target_to_world, int base_filament);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Repaints `target` (support, seam, colour and fuzzy skin painting) from `sources`.
// `target_to_world` maps the target mesh into the same world as the sources. Where the skins of
// two sources coincide, the one listed first wins. `base_filament` is the filament the target
// prints with: skin that would be coloured with it is left unpainted.
Stats transfer(const std::vector<Source> &sources, ModelVolume &target, const Transform3d &target_to_world, int base_filament);

// Split: a piece cut out of a mesh keeps the very same triangles (same vertices, same corner
// order), so its painting is copied triangle by triangle, exactly and without any search.
// Finds the facets of the pieces in the source. Built once per source, used for every piece.
class FacetMatcher
{
public:
    explicit FacetMatcher(const indexed_triangle_set &source);
    ~FacetMatcher();
    // For each facet of `piece`, the facet of the source it is, or -1 when there is none.
    std::vector<int> match(const indexed_triangle_set &piece) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
// Copies the painting of `source` onto `target` through `facet_map` (from FacetMatcher::match() on
// their meshes). Returns false, without touching `target`, when some facet has no match.
bool copy_painting(const ModelVolume &source, ModelVolume &target, const std::vector<int> &facet_map);

} // namespace PaintTransfer
} // namespace Slic3r

#endif // slic3r_PaintTransfer_hpp_
