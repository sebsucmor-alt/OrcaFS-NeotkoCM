// NEOTKO_NEOARACHNE_TAG fase0
#include "NeoArachneDebug.hpp"
#include "NeoArachneRuntime.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace NeoArachne {

void log_dispatch(const Config& /*cfg*/, int layer_id, int region_id, const char* branch)
{
    BOOST_LOG_TRIVIAL(debug) << "[neoarachne] L" << layer_id
                             << " region=" << region_id
                             << " branch=" << (branch ? branch : "?");
}

Runtime& Runtime::mut()
{
    static Runtime g;
    return g;
}

const Runtime& Runtime::get() { return mut(); }

}} // namespace Slic3r::NeoArachne
