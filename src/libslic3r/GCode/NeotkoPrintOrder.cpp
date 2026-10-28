// NEOTKO_PRINTORDER_TAG — see NeotkoPrintOrder.hpp.
#include "NeotkoPrintOrder.hpp"

#include "../Print.hpp"
#include "../Model.hpp"
#include "../ShortestPath.hpp"
#include "../NeoDebug.hpp"
#include "../Layer.hpp"
#include "../ClipperUtils.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace Slic3r {
namespace NeotkoPrintOrder {

// Index of the instance's ModelObject in the print's Model. The GUI writes the ranks in that
// same order every time it builds the config (Plater::priv::neotko_full_config), so the index is
// always fresh; it is never stored anywhere.
static int model_object_index(const Print& print, const PrintInstance& instance)
{
    if (instance.model_instance == nullptr)
        return -1;
    const ModelObject* mo = instance.model_instance->get_object();
    const auto&        objs = print.model().objects;
    for (size_t i = 0; i < objs.size(); ++i)
        if (objs[i] == mo)
            return int(i);
    return -1;
}

int rank_of(const Print& print, const PrintInstance& instance)
{
    const auto& ranks = print.config().neotko_print_order_ranks.values;
    const int   idx   = model_object_index(print, instance);
    if (idx < 0 || idx >= int(ranks.size()))
        return 0;
    return std::max(0, ranks[idx]);
}

bool active(const Print& print)
{
    if (!print.config().neotko_print_order_manual.value)
        return false;
    for (const PrintObject* po : print.objects())
        for (const PrintInstance& inst : po->instances())
            if (rank_of(print, inst) > 0)
                return true;
    return false;
}

void log_header(const Print& print)
{
    if (!NeoDebug::enabled(NeoDebug::PRINTORDER))
        return;
    std::string ranks;
    for (int r : print.config().neotko_print_order_ranks.values)
        ranks += std::to_string(r) + " ";
    std::string names;
    for (const ModelObject* mo : print.model().objects)
        names += "\"" + (mo ? mo->name : std::string("?")) + "\" ";
    NeoDebug::write(NeoDebug::PRINTORDER, "[PO] ===== G-code export. manual(LibreMode)=" +
                                              std::string(print.config().neotko_print_order_manual.value ? "1" : "0") +
                                              " active=" + std::string(active(print) ? "1" : "0") +
                                              " | ranks mirror: [ " + ranks + "] | model objects in that order: " + names);
}

std::vector<const PrintInstance*> order(const std::vector<const PrintInstance*>& chain,
                                        const std::vector<int>&                 ranks)
{
    std::vector<size_t> idx(chain.size());
    std::iota(idx.begin(), idx.end(), size_t(0));
    auto key = [&ranks](size_t i) {
        const int r = i < ranks.size() ? ranks[i] : 0;
        return r > 0 ? r : 0x7fffffff; // Free goes after every number
    };
    // stable: ties and Free keep the nearest-chain order. Never compares pointers.
    std::stable_sort(idx.begin(), idx.end(), [&key](size_t a, size_t b) { return key(a) < key(b); });
    std::vector<const PrintInstance*> out;
    out.reserve(chain.size());
    for (size_t i : idx)
        out.push_back(chain[i]);
    return out;
}

// Part numbers of one object, from the flattened mirror (objects in Model::objects order).
static std::vector<int> part_ranks_of(const Print& print, const ModelObject* mo)
{
    std::vector<int> out;
    if (mo == nullptr)
        return out;
    const auto& flat = print.config().neotko_print_order_part_ranks.values;
    size_t      off  = 0;
    for (const ModelObject* o : print.model().objects) {
        if (o == mo) {
            for (size_t v = 0; v < mo->volumes.size(); ++v)
                out.push_back(off + v < flat.size() ? std::max(0, flat[off + v]) : 0);
            return out;
        }
        off += o ? o->volumes.size() : 0;
    }
    return out;
}

std::vector<size_t> island_order(const Print& print, const PrintObject& owner, const Layer* layer, size_t n_islands)
{
    std::vector<size_t> order(n_islands);
    std::iota(order.begin(), order.end(), size_t(0));
    if (layer == nullptr || n_islands < 2 || !print.config().neotko_print_order_manual.value)
        return order;

    const std::vector<int> ranks = part_ranks_of(print, owner.model_object());
    if (std::none_of(ranks.begin(), ranks.end(), [](int r) { return r > 0; }))
        return order;

    const PrintObject* geo = layer->object(); // may be the shared source object
    if (geo == nullptr || geo->layers().empty())
        return order;
    const int li = int(layer->id()) - int(geo->layers().front()->id());
    const std::vector<VolumeSlices>& by_vol = geo->firstLayerObjSlice();
    const ModelObject*               gmo    = geo->model_object();
    if (li < 0 || gmo == nullptr)
        return order;

    // volume position (same in owner and in a shared source: identical objects) of each VolumeSlices
    std::vector<int> vol_pos(by_vol.size(), -1);
    for (size_t k = 0; k < by_vol.size(); ++k)
        for (size_t v = 0; v < gmo->volumes.size(); ++v)
            if (gmo->volumes[v]->id() == by_vol[k].volume_id) { vol_pos[k] = int(v); break; }

    const ExPolygons& isl = layer->lslices;
    std::vector<int>  key(n_islands, 0x7fffffff);
    std::string       dbg;
    for (size_t i = 0; i < isl.size() && i < n_islands; ++i) {
        // 16-oct: parts that touch in this layer are fused into ONE island. It prints in the turn of
        // the EARLIEST numbered part that really is in it (>= 5 % of the island), so a part does
        // not get split between its own loose bits and a later part's turn (seen: base02's rim
        // fused with txt2ico's stripes went after txt1up while base02's slivers went first).
        const BoundingBox ib     = get_extents(isl[i]);
        const double      i_area = std::max(1., std::abs(isl[i].area()));
        int               r      = 0;
        std::string       who;
        std::string       shares; // every part with any overlap, for the log
        int               n_in   = 0;
        for (size_t k = 0; k < by_vol.size(); ++k) {
            if (vol_pos[k] < 0 || li >= int(by_vol[k].slices.size()) || by_vol[k].slices[li].empty())
                continue;
            if (!ib.overlap(get_extents(by_vol[k].slices[li])))
                continue;
            const double a = area(intersection_ex(ExPolygons{ isl[i] }, by_vol[k].slices[li]));
            if (NeoDebug::enabled(NeoDebug::PRINTORDER) && a > 0.)
                shares += " " + gmo->volumes[vol_pos[k]]->name + "=" + std::to_string(int(std::lround(100. * a / i_area))) + "%";
            if (a < 0.05 * i_area)
                continue;
            const int pr = vol_pos[k] < int(ranks.size()) ? ranks[vol_pos[k]] : 0;
            if (pr > 0 && (r == 0 || pr < r))
                r = pr;
            ++n_in;
            if (NeoDebug::enabled(NeoDebug::PRINTORDER))
                who += (who.empty() ? "" : "+") + ("\"" + gmo->volumes[vol_pos[k]]->name + "\"") +
                       (pr > 0 ? "#" + std::to_string(pr) : std::string("F"));
        }
        if (r > 0)
            key[i] = r;
        if (NeoDebug::enabled(NeoDebug::PRINTORDER)) {
            // one line per island: where it is, how big, holes, who is in it
            const BoundingBoxf bb(unscaled(ib.min), unscaled(ib.max));
            NeoDebug::write(NeoDebug::PRINTORDER,
                "[PO]     island i" + std::to_string(i) + " z=" + std::to_string(layer->print_z) + " slice_layer=" + std::to_string(li) +
                " x " + std::to_string(int(bb.min.x())) + "-" + std::to_string(int(bb.max.x())) +
                " y " + std::to_string(int(bb.min.y())) + "-" + std::to_string(int(bb.max.y())) +
                " area=" + std::to_string(int(unscaled(unscaled(i_area)))) + "mm2 holes=" + std::to_string(isl[i].holes.size()) +
                " |" + shares + "  => " + (r > 0 ? "#" + std::to_string(r) : std::string("Free")));
        }
        if (NeoDebug::enabled(NeoDebug::PRINTORDER))
            dbg += " i" + std::to_string(i) + "=" + (who.empty() ? std::string("?") : who) +
                   (n_in > 1 ? "(fused->" + (r > 0 ? "#" + std::to_string(r) : std::string("F")) + ")" : std::string(""));
    }
    std::stable_sort(order.begin(), order.end(), [&key](size_t a, size_t b) { return key[a] < key[b]; });

    if (NeoDebug::enabled(NeoDebug::PRINTORDER)) {
        std::string seq;
        for (size_t i : order)
            seq += std::to_string(i) + " ";
        NeoDebug::write(NeoDebug::PRINTORDER, "[PO]   parts z=" + std::to_string(layer->print_z) + " \"" +
                                                  (owner.model_object() ? owner.model_object()->name : std::string("?")) +
                                                  "\" :" + dbg + "  -> islands " + seq);
    }
    return order;
}

bool object_has_part_ranks(const Print& print, const PrintObject* po)
{
    if (po == nullptr || !print.config().neotko_print_order_manual.value)
        return false;
    const std::vector<int> r = part_ranks_of(print, po->model_object());
    return std::any_of(r.begin(), r.end(), [](int v) { return v > 0; });
}

int island_for_point(const Layer& layer, const std::vector<size_t>& test_order, const Point& p)
{
    for (size_t i : test_order)
        if (i < layer.lslices.size() && i < layer.lslices_bboxes.size() && layer.lslices_bboxes[i].contains(p) &&
            layer.lslices[i].contains(p))
            return int(i);
    return -1;
}

std::string label(const Print& print, const PrintInstance& instance)
{
    const ModelObject* mo = instance.model_instance ? instance.model_instance->get_object() : nullptr;
    const int          r  = rank_of(print, instance);
    const Vec2d        p  = unscale(instance.shift_without_plate_offset()); // plate mm, without the multi-plate offset
    std::ostringstream os;
    os << "\"" << (mo ? mo->name : std::string("?")) << "\" " << (r > 0 ? "#" + std::to_string(r) : std::string("Free"))
       << " (" << int(std::lround(p.x())) << "," << int(std::lround(p.y())) << ")";
    return os.str();
}

std::string describe(const Print& print, const std::vector<const PrintInstance*>& seq)
{
    std::string out;
    for (const PrintInstance* inst : seq) {
        if (inst == nullptr)
            continue;
        if (!out.empty())
            out += "  >  ";
        out += label(print, *inst);
    }
    return out;
}

std::vector<const PrintInstance*> build(const Print& print)
{
    std::vector<const PrintInstance*> chain = chain_print_object_instances(print);
    std::vector<int>                  ranks;
    ranks.reserve(chain.size());
    for (const PrintInstance* inst : chain)
        ranks.push_back(inst ? rank_of(print, *inst) : 0);
    std::vector<const PrintInstance*> out = order(chain, ranks);

    if (NeoDebug::enabled(NeoDebug::PRINTORDER)) {
        NeoDebug::write(NeoDebug::PRINTORDER, "[PO] manual order ON. ranks mirror size=" +
                                                  std::to_string(print.config().neotko_print_order_ranks.values.size()) +
                                                  " model objects=" + std::to_string(print.model().objects.size()));
        NeoDebug::write(NeoDebug::PRINTORDER, "[PO] Orca would do : " + describe(print, chain));
        NeoDebug::write(NeoDebug::PRINTORDER, "[PO] your order    : " + describe(print, out));
    }
    return out;
}

} // namespace NeotkoPrintOrder
} // namespace Slic3r
