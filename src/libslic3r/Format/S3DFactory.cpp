// NEOTKO_S3DFACTORY_TAG_START
#include "S3DFactory.hpp"

#include "../Model.hpp"
#include "../TriangleMesh.hpp"
#include "../Exception.hpp"

#include <miniz.h>

#include <boost/log/trivial.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/algorithm/string.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r {

// ── Debug output ────────────────────────────────────────────────────────────
// Set env ORCA_DEBUG_S3DFACTORY=1 to write verbose log to /tmp/s3dfactory_debug.txt
namespace {

struct S3DDebug {
    std::ofstream file;
    bool          active = false;

    S3DDebug() {
        const char* env = std::getenv("ORCA_DEBUG_S3DFACTORY");
        active = (env && std::string(env) == "1");
        if (active)
            file.open("/tmp/s3dfactory_debug.txt", std::ios::app);
    }

    template<typename T>
    S3DDebug& operator<<(const T& v) {
        if (active) file << v;
        return *this;
    }
    S3DDebug& operator<<(std::ostream& (*f)(std::ostream&)) {
        if (active) f(file);
        return *this;
    }
};

// ── Binary helpers ───────────────────────────────────────────────────────────

static uint32_t read_be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
}

static std::string utf16be_to_utf8(const uint8_t* p, size_t byte_len)
{
    std::string out;
    out.reserve(byte_len / 2);
    for (size_t i = 0; i + 1 < byte_len; i += 2) {
        uint16_t cp = (uint16_t(p[i]) << 8) | p[i + 1];
        if      (cp < 0x80)  { out += char(cp); }
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6));
                                out += char(0x80 | (cp & 0x3F)); }
        else                 { out += char(0xE0 | (cp >> 12));
                                out += char(0x80 | ((cp >> 6) & 0x3F));
                                out += char(0x80 | (cp & 0x3F)); }
    }
    return out;
}

static std::vector<uint8_t> zlib_decompress(const uint8_t* src, size_t src_len, size_t hint)
{
    std::vector<uint8_t> out;
    mz_ulong dest_len = static_cast<mz_ulong>(hint > 0 ? hint : src_len * 4);
    out.resize(dest_len);

    int ret = mz_uncompress(out.data(), &dest_len, src, static_cast<mz_ulong>(src_len));
    if (ret == MZ_BUF_ERROR) {
        dest_len *= 2;
        out.resize(dest_len);
        ret = mz_uncompress(out.data(), &dest_len, src, static_cast<mz_ulong>(src_len));
    }
    if (ret != MZ_OK)
        throw Slic3r::RuntimeError("S3DFactory: zlib decompress failed (code " + std::to_string(ret) + ")");

    out.resize(dest_len);
    return out;
}

// ── Lightweight XML tag extractor ────────────────────────────────────────────

static std::string xml_tag(const std::string& block, const std::string& tag)
{
    const std::string open  = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    auto s = block.find(open);
    if (s == std::string::npos) return {};
    s += open.size();
    auto e = block.find(close, s);
    if (e == std::string::npos) return {};
    return block.substr(s, e - s);
}

// ── Group number extractor ────────────────────────────────────────────────────
// "Group 1" → 1, "Group 2" → 2.  Falls back to 1.

static int group_number(const std::string& group_name)
{
    if (group_name.empty()) return 1;
    for (int i = static_cast<int>(group_name.size()) - 1; i >= 0; --i) {
        if (std::isdigit(static_cast<unsigned char>(group_name[i]))) {
            int j = i;
            while (j > 0 && std::isdigit(static_cast<unsigned char>(group_name[j - 1]))) --j;
            try { return std::stoi(group_name.substr(j, i - j + 1)); } catch (...) {}
        }
    }
    return 1;
}

// ── Binary STL parser ────────────────────────────────────────────────────────
// Builds an indexed_triangle_set from in-memory binary STL bytes.
// Vertex dedup via unordered_map on bit-exact float keys.

struct Vec3fHash {
    size_t operator()(const std::tuple<uint32_t, uint32_t, uint32_t>& k) const {
        size_t h = std::get<0>(k);
        h ^= std::get<1>(k) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::get<2>(k) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

using VKey = std::tuple<uint32_t, uint32_t, uint32_t>;
using VMap = std::unordered_map<VKey, int, Vec3fHash>;

static bool parse_stl(const std::vector<uint8_t>& stl, indexed_triangle_set& its)
{
    if (stl.size() < 84) return false;
    uint32_t tri_count = 0;
    std::memcpy(&tri_count, stl.data() + 80, 4);
    if (stl.size() < 84 + static_cast<size_t>(tri_count) * 50) return false;

    its.vertices.reserve(tri_count * 3 / 2);
    its.indices.reserve(tri_count);
    VMap vmap;
    vmap.reserve(tri_count * 2);

    auto get_vertex = [&](float x, float y, float z) -> int {
        uint32_t bx, by, bz;
        std::memcpy(&bx, &x, 4);
        std::memcpy(&by, &y, 4);
        std::memcpy(&bz, &z, 4);
        VKey k{bx, by, bz};
        auto it = vmap.find(k);
        if (it != vmap.end()) return it->second;
        int idx = static_cast<int>(its.vertices.size());
        its.vertices.emplace_back(x, y, z);
        vmap[k] = idx;
        return idx;
    };

    for (uint32_t i = 0; i < tri_count; ++i) {
        const uint8_t* tri = stl.data() + 84 + i * 50;
        float v[3][3];
        for (int vi = 0; vi < 3; ++vi)
            std::memcpy(v[vi], tri + 12 + vi * 12, 12);
        int i0 = get_vertex(v[0][0], v[0][1], v[0][2]);
        int i1 = get_vertex(v[1][0], v[1][1], v[1][2]);
        int i2 = get_vertex(v[2][0], v[2][1], v[2][2]);
        if (i0 != i1 && i1 != i2 && i0 != i2)
            its.indices.emplace_back(i0, i1, i2);
    }
    return !its.indices.empty();
}

// ── Object builder ───────────────────────────────────────────────────────────
// STL vertices are in absolute world space — no translation needed.

static void add_object(Model& model, indexed_triangle_set&& its,
                       const std::string& name, const std::string& group_name,
                       const std::string& input_file)
{
    const int gn = group_number(group_name);
    const std::string obj_name = "[L" + std::to_string(gn) + "] " + name;

    ModelObject* obj = model.add_object();
    obj->name       = obj_name;
    obj->input_file = input_file;

    ModelVolume* vol = obj->add_volume(TriangleMesh(std::move(its)), ModelVolumeType::MODEL_PART, false);
    vol->name = name;
    obj->add_instance();
}

// ── v5 loader (contents.xml) ─────────────────────────────────────────────────

using EntryMap = std::unordered_map<std::string, std::vector<uint8_t>>;

static int load_v5(const EntryMap& entries, Model& model,
                   const std::string& input_file, S3DDebug& dbg)
{
    auto it_xml = entries.find("contents.xml");
    if (it_xml == entries.end()) { dbg << "v5: contents.xml not found\n"; return 0; }

    const std::string xml(it_xml->second.begin(), it_xml->second.end());
    const std::string otag = "<model>", ctag = "</model>";
    int loaded = 0;
    size_t pos = 0;

    while (true) {
        auto s = xml.find(otag, pos); if (s == std::string::npos) break;
        auto e = xml.find(ctag, s);  if (e == std::string::npos) break;
        const std::string blk = xml.substr(s + otag.size(), e - s - otag.size());
        pos = e + ctag.size();

        std::string path  = xml_tag(blk, "path");
        std::string name  = xml_tag(blk, "modelName");
        std::string group = xml_tag(blk, "groupName");
        if (path.empty()) continue;

        auto it = entries.find(path);
        if (it == entries.end()) { dbg << "  v5: STL not found: " << path << "\n"; continue; }

        if (name.empty())
            name = boost::filesystem::path(path).stem().string();

        indexed_triangle_set its;
        if (!parse_stl(it->second, its)) {
            dbg << "  v5: STL parse failed: " << name << "\n";
            BOOST_LOG_TRIVIAL(warning) << "S3DFactory v5: failed to parse STL for " << name;
            continue;
        }

        dbg << "  v5 [" << name << "] group=[" << group
            << "] tris=" << its.indices.size() << "\n";
        BOOST_LOG_TRIVIAL(debug) << "S3DFactory v5: loaded " << name
                                 << " group=" << group
                                 << " tris=" << its.indices.size();

        add_object(model, std::move(its), name, group, input_file);
        ++loaded;
    }
    return loaded;
}

// ── v4 loader (.info based) ──────────────────────────────────────────────────

static int load_v4(const EntryMap& entries, Model& model,
                   const std::string& input_file, S3DDebug& dbg)
{
    int loaded = 0;
    for (const auto& kv : entries) {
        const std::string& entry = kv.first;
        if (entry.size() < 5 || entry.substr(entry.size() - 5) != ".info") continue;

        const std::string stl_path = entry.substr(0, entry.size() - 5) + ".stl";
        auto it_stl = entries.find(stl_path);
        if (it_stl == entries.end()) {
            dbg << "  v4: no STL for info: " << entry << "\n";
            continue;
        }

        // Parse .info CSV (key,value per line)
        const std::string info_str(kv.second.begin(), kv.second.end());
        std::string obj_name, group_name;
        std::istringstream iss(info_str);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            auto comma = line.find(',');
            if (comma == std::string::npos) continue;
            std::string key = line.substr(0, comma);
            std::string val = line.substr(comma + 1);
            // Trim trailing CR/LF/space
            while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' '))
                val.pop_back();
            if      (key == "filename")  obj_name   = val;
            else if (key == "groupName") group_name = val;
        }
        if (obj_name.empty())
            obj_name = boost::filesystem::path(stl_path).stem().string();

        indexed_triangle_set its;
        if (!parse_stl(it_stl->second, its)) {
            dbg << "  v4: STL parse failed: " << obj_name << "\n";
            BOOST_LOG_TRIVIAL(warning) << "S3DFactory v4: failed to parse STL for " << obj_name;
            continue;
        }

        dbg << "  v4 [" << obj_name << "] group=[" << group_name
            << "] tris=" << its.indices.size() << "\n";
        BOOST_LOG_TRIVIAL(debug) << "S3DFactory v4: loaded " << obj_name
                                 << " group=" << group_name
                                 << " tris=" << its.indices.size();

        add_object(model, std::move(its), obj_name, group_name, input_file);
        ++loaded;
    }
    return loaded;
}

// ── Container parser ─────────────────────────────────────────────────────────

static EntryMap parse_container(const std::vector<uint8_t>& data, S3DDebug& dbg)
{
    EntryMap entries;
    size_t pos = 0;

    while (pos + 4 <= data.size()) {
        // Name: 4-byte BE length (in bytes) + UTF-16 BE string
        const uint32_t name_bytes = read_be32(data.data() + pos); pos += 4;
        if (name_bytes == 0 || pos + name_bytes > data.size()) break;

        std::string name = utf16be_to_utf8(data.data() + pos, name_bytes); pos += name_bytes;

        // Compressed block: 4-byte BE total length + [4-byte uncompressed hint] + zlib data
        if (pos + 4 > data.size()) break;
        const uint32_t block_len = read_be32(data.data() + pos); pos += 4;
        if (block_len < 4 || pos + block_len > data.size()) break;

        const uint32_t uncmp_hint = read_be32(data.data() + pos);
        const uint8_t* zlib_data  = data.data() + pos + 4;
        const size_t   zlib_len   = block_len - 4;
        pos += block_len;

        // Normalise: strip leading '/'
        if (!name.empty() && name[0] == '/') name = name.substr(1);

        try {
            entries[name] = zlib_decompress(zlib_data, zlib_len, uncmp_hint);
            dbg << "  entry [" << name << "] " << entries[name].size() << " bytes\n";
        } catch (const std::exception& ex) {
            dbg << "  entry [" << name << "] decompress FAILED: " << ex.what() << "\n";
            BOOST_LOG_TRIVIAL(warning) << "S3DFactory: decompress failed for " << name
                                       << ": " << ex.what();
        }
    }
    return entries;
}

} // anonymous namespace

// ── Public API ───────────────────────────────────────────────────────────────

bool load_s3d_factory(const std::string& path, Model& model)
{
    S3DDebug dbg;
    dbg << "=== S3DFactory: " << path << " ===\n";
    BOOST_LOG_TRIVIAL(info) << "S3DFactory: loading " << path;

    // Read file
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        BOOST_LOG_TRIVIAL(error) << "S3DFactory: cannot open " << path;
        return false;
    }
    std::vector<uint8_t> data(
        (std::istreambuf_iterator<char>(f)),
        (std::istreambuf_iterator<char>()));
    f.close();
    dbg << "file size=" << data.size() << "\n";

    if (data.size() < 8) {
        BOOST_LOG_TRIVIAL(error) << "S3DFactory: file too small";
        return false;
    }

    // Parse container
    EntryMap entries = parse_container(data, dbg);
    dbg << "total entries=" << entries.size() << "\n";
    if (entries.empty()) return false;

    // Detect format version
    const bool is_v5 = entries.count("contents.xml") > 0;
    dbg << "format: " << (is_v5 ? "v5 (contents.xml)" : "v4 (.info)") << "\n";
    BOOST_LOG_TRIVIAL(info) << "S3DFactory: format " << (is_v5 ? "v5" : "v4");

    const int loaded = is_v5
        ? load_v5(entries, model, path, dbg)
        : load_v4(entries, model, path, dbg);

    dbg << "objects loaded=" << loaded << "\n";
    BOOST_LOG_TRIVIAL(info) << "S3DFactory: loaded " << loaded << " object(s)";

    return loaded > 0;
}

} // namespace Slic3r
// NEOTKO_S3DFACTORY_TAG_END
