// NEOTKO_LOCALZ_TAG s355 — "Subdivide Mix Layer" (FS Local-Z dithering) for the NeoTower planner.
//
// The stock (Classic) tower learns the Local-Z toolchanges of every layer inside
// Print::_make_wipe_tower (collect_local_z_wipe_tower_toolchanges + plan_local_z_toolchange).
// The NeoTower branch returns before that, so its plan never contained them and the G-code
// export aborted ("Wipe tower generation failed, possibly due to empty first layer").
//
// This is the same computation, exposed so NeoTower::collect_all_events can ask for it layer by
// layer. Defined in Print.cpp, next to the collector it wraps (single owner of the sequence: the
// very function Classic uses, so plan == emission holds for both towers).
#ifndef slic3r_NeoTowerLocalZ_hpp_
#define slic3r_NeoTowerLocalZ_hpp_

#include <memory>
#include <utility>
#include <vector>

namespace Slic3r {

class Print;

class NeotkoLocalZPlanner
{
public:
    explicit NeotkoLocalZPlanner(const Print& print);
    ~NeotkoLocalZPlanner();
    // False when "Subdivide Mix Layer" is off or no object carries a Local-Z plan: at() is then
    // free and always empty.
    bool enabled() const;
    // Local-Z toolchanges (old, new) of the layer at print_z, in emission order, given the tool
    // the writer holds when the layer starts. Call with ascending print_z.
    std::vector<std::pair<unsigned int, unsigned int>> at(double print_z, int start_extruder);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace Slic3r

#endif
