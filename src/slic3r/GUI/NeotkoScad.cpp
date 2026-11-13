// see NeotkoScad.hpp
#include "NeotkoScad.hpp"

#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
// Windows: wx defines pid_t as a macro, which breaks boost::process (same fix as MediaPlayCtrl.cpp).
#undef pid_t
#include <boost/process.hpp>
#ifdef _WIN32
#include <boost/nowide/convert.hpp>
#include <boost/process/windows.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <regex>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace Slic3r {
namespace GUI {
namespace NeoScad {

namespace fs = boost::filesystem;

static const char* CONFIG_KEY = "openscad_path";

// -------------------------------------------------------------------------------------------------
// Small text helpers (no locale: wx may set a comma decimal separator)
// -------------------------------------------------------------------------------------------------

static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool parse_number(const std::string& in, double& out)
{
    const std::string s = trim(in);
    static const std::regex re(R"(^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$)");
    if (!std::regex_match(s, re)) return false;
    // manual parse of mantissa/exponent, locale free
    size_t i = 0; bool neg = false;
    if (s[i] == '-' || s[i] == '+') neg = s[i++] == '-';
    double v = 0., scale = 1.; bool dot = false;
    for (; i < s.size() && s[i] != 'e' && s[i] != 'E'; ++i) {
        if (s[i] == '.') { dot = true; continue; }
        if (dot) { scale /= 10.; v += (s[i] - '0') * scale; } else v = v * 10. + (s[i] - '0');
    }
    if (i < s.size()) { int e = std::atoi(s.c_str() + i + 1); v *= std::pow(10., e); }
    out = neg ? -v : v;
    return true;
}

static std::string num_to_str(double x)
{
    const bool neg = x < 0;
    long long  q   = (long long) std::llround(std::abs(x) * 1000000.);
    std::string s  = std::to_string(q / 1000000);
    long long  fr  = q % 1000000;
    if (fr != 0) {
        std::string f = std::to_string(fr);
        f.insert(0, 6 - f.size(), '0');
        while (!f.empty() && f.back() == '0') f.pop_back();
        s += "." + f;
    }
    return (neg && q != 0 ? "-" : "") + s;
}

static bool is_quoted(const std::string& s) { return s.size() >= 2 && s.front() == '"' && s.back() == '"'; }

static std::string unquote(const std::string& s)
{
    if (!is_quoted(s)) return s;
    std::string out;
    for (size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] == '\\' && i + 2 < s.size()) { out += s[++i]; continue; }
        out += s[i];
    }
    return out;
}

static std::string quote(const std::string& s)
{
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

static std::string pct_encode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '_' || c == '.' || c == '-')
            out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

static std::string pct_decode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else
            out += s[i];
    }
    return out;
}

// -------------------------------------------------------------------------------------------------
// Customizer parser
// -------------------------------------------------------------------------------------------------

std::vector<Param> parse(const std::string& source)
{
    std::vector<Param> out;
    std::istringstream in(source);
    std::string        line, group, desc;
    bool               in_block = false, hidden = false;
    int                depth = 0;
    static const std::regex re_group(R"(^\s*/\*\s*\[\s*([^\]]+?)\s*\]\s*\*/\s*$)");
    static const std::regex re_assign(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*;\s*(//\s*(.*))?$)");
    static const std::regex re_stop(R"(^\s*(module|function)\b)");

    while (std::getline(in, line)) {
        std::smatch m;
        if (!in_block && depth == 0 && std::regex_match(line, m, re_group)) {
            group  = trim(m[1]);
            hidden = group == "Hidden" || group == "hidden";
            desc.clear();
            continue;
        }
        if (depth == 0 && !in_block && std::regex_search(line, re_stop))
            break; // Customizer stops at the first module / function
        const std::string t = trim(line);
        if (in_block) {
            if (t.find("*/") != std::string::npos) in_block = false;
            continue;
        }
        if (t.rfind("/*", 0) == 0 && t.find("*/") == std::string::npos) { in_block = true; continue; }
        if (t.empty()) { desc.clear(); continue; }
        if (depth == 0 && t.rfind("//", 0) == 0) {
            if (!desc.empty()) desc += "\n";
            desc += trim(t.substr(2));
            continue;
        }
        if (depth == 0 && !hidden && std::regex_match(line, m, re_assign)) {
            Param p;
            p.name        = m[1];
            p.group       = group;
            p.description = desc;
            const std::string val  = trim(m[2]);
            const std::string note = trim(m[4]);
            double num;
            bool   ok = true;
            if (val == "true" || val == "false") {
                p.type = Param::Bool;
                p.num  = val == "true" ? 1. : 0.;
            } else if (parse_number(val, num)) {
                p.type    = Param::Number;
                p.num     = num;
                p.integer = val.find('.') == std::string::npos && val.find('e') == std::string::npos;
            } else if (is_quoted(val)) {
                p.type = Param::Text;
                p.text = unquote(val);
            } else
                ok = false; // vectors and expressions: not offered
            if (ok && note.size() >= 2 && note.front() == '[' && note.back() == ']' && p.type != Param::Bool) {
                const std::string inner = note.substr(1, note.size() - 2);
                if (inner.find(',') != std::string::npos) {
                    // dropdown: "a, b, c" or "10:Small, 20:Big"
                    std::stringstream ss(inner);
                    std::string       item;
                    while (std::getline(ss, item, ',')) {
                        item = trim(item);
                        const size_t colon = item.find(':');
                        std::string  v = colon == std::string::npos ? item : trim(item.substr(0, colon));
                        std::string  l = colon == std::string::npos ? item : trim(item.substr(colon + 1));
                        if (p.type == Param::Text && !is_quoted(v)) v = quote(v);
                        p.choices.emplace_back(v, unquote(l));
                    }
                    const std::string cur = p.type == Param::Text ? quote(p.text) : val;
                    int sel = 0;
                    for (int i = 0; i < (int) p.choices.size(); ++i) {
                        double a, b;
                        if (p.choices[i].first == cur || (parse_number(p.choices[i].first, a) && parse_number(cur, b) && a == b))
                            sel = i;
                    }
                    p.type = Param::Choice;
                    p.num  = sel;
                } else if (p.type == Param::Number) {
                    std::vector<double> parts;
                    std::stringstream   ss(inner);
                    std::string         item;
                    while (std::getline(ss, item, ':')) {
                        double x;
                        if (parse_number(item, x)) parts.push_back(x);
                    }
                    if (parts.size() == 1) { p.mn = 0.; p.mx = parts[0]; p.has_range = true; }
                    if (parts.size() == 2) { p.mn = parts[0]; p.mx = parts[1]; p.has_range = true; }
                    if (parts.size() == 3) {
                        p.mn = parts[0]; p.step = parts[1]; p.mx = parts[2]; p.has_range = true;
                        p.integer = std::abs(p.step - std::round(p.step)) < 1e-9 && p.integer;
                    }
                }
            }
            if (ok && p.type == Param::Number && !p.has_range) {
                const double a = std::max(10., 2. * std::abs(p.num));
                p.mn = p.num < 0 ? -a : 0.;
                p.mx = a;
            }
            if (ok)
                out.push_back(std::move(p));
            desc.clear();
        }
        // brace depth, ignoring strings and line comments
        bool in_str = false;
        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (in_str) { if (c == '\\') ++i; else if (c == '"') in_str = false; continue; }
            if (c == '"') in_str = true;
            else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') break;
            else if (c == '{') ++depth;
            else if (c == '}') depth = std::max(0, depth - 1);
        }
    }
    return out;
}

std::string value_literal(const Param& p)
{
    switch (p.type) {
    case Param::Bool: return p.num != 0. ? "true" : "false";
    case Param::Text: return quote(p.text);
    case Param::Choice: {
        const int i = std::clamp(int(std::lround(p.num)), 0, std::max(0, int(p.choices.size()) - 1));
        return p.choices.empty() ? "0" : p.choices[i].first;
    }
    default: return num_to_str(p.num);
    }
}

std::string encode(const std::string& file_name, const std::vector<Param>& params)
{
    std::string out = "scad:" + pct_encode(file_name);
    for (const Param& p : params)
        out += "|" + p.name + "=" + pct_encode(value_literal(p));
    return out;
}

bool decode(const std::string& recipe, std::string& file_name, std::map<std::string, std::string>& values)
{
    values.clear();
    if (!is_recipe(recipe)) return false;
    std::stringstream ss(recipe.substr(5));
    std::string       item;
    bool              first = true;
    while (std::getline(ss, item, '|')) {
        if (first) { file_name = pct_decode(item); first = false; continue; }
        const size_t eq = item.find('=');
        if (eq != std::string::npos)
            values[item.substr(0, eq)] = pct_decode(item.substr(eq + 1));
    }
    return !file_name.empty();
}

void apply(std::vector<Param>& params, const std::map<std::string, std::string>& values)
{
    for (Param& p : params) {
        auto it = values.find(p.name);
        if (it == values.end()) continue;
        const std::string& v = it->second;
        double x;
        switch (p.type) {
        case Param::Bool: p.num = v == "true" ? 1. : 0.; break;
        case Param::Text: p.text = unquote(v); break;
        case Param::Choice:
            for (int i = 0; i < (int) p.choices.size(); ++i) {
                double a, b;
                if (p.choices[i].first == v || (parse_number(p.choices[i].first, a) && parse_number(v, b) && a == b))
                    p.num = i;
            }
            break;
        default:
            if (parse_number(v, x)) p.num = x;
        }
    }
}

// -------------------------------------------------------------------------------------------------
// Places
// -------------------------------------------------------------------------------------------------

std::string find_openscad()
{
    if (AppConfig* cfg = wxGetApp().app_config) {
        const std::string saved = cfg->get(CONFIG_KEY);
        boost::system::error_code ec;
        if (!saved.empty() && fs::exists(saved, ec))
            return saved;
    }
    std::vector<std::string> cand;
    // 1) the copy shipped inside the app (resources/openscad/...)
    {
        const fs::path b = fs::path(resources_dir()) / "openscad";
#if defined(_WIN32)
        cand.push_back((b / "openscad.exe").string());
#elif defined(__APPLE__)
        cand.push_back((b / "OpenSCAD.app" / "Contents" / "MacOS" / "OpenSCAD").string());
#else
        cand.push_back((b / "openscad").string());
        cand.push_back((b / "OpenSCAD.AppImage").string());
#endif
    }
    // 2) an OpenSCAD the user installed
#if defined(_WIN32)
    for (const char* env : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"})
        if (const char* pf = std::getenv(env)) {
            cand.push_back(std::string(pf) + "\\OpenSCAD\\openscad.exe");
            cand.push_back(std::string(pf) + "\\OpenSCAD (Nightly)\\openscad.exe");
        }
#elif defined(__APPLE__)
    cand.insert(cand.end(), {"/Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD", "/opt/homebrew/bin/openscad", "/usr/local/bin/openscad",
                             "/opt/local/bin/openscad" /* MacPorts */});
    boost::system::error_code ec;
    for (fs::directory_iterator it("/Applications", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (n.rfind("OpenSCAD", 0) == 0 && it->path().extension() == ".app")
            cand.push_back((it->path() / "Contents" / "MacOS" / "OpenSCAD").string());
    }
#else
    cand = {"/usr/bin/openscad", "/usr/local/bin/openscad", "/snap/bin/openscad"};
#endif
    for (const std::string& c : cand) {
        boost::system::error_code ec;
        if (fs::exists(c, ec))
            return c;
    }
    return {};
}

void set_openscad_path(const std::string& path)
{
    if (AppConfig* cfg = wxGetApp().app_config) {
        cfg->set(CONFIG_KEY, path);
        cfg->save();
    }
}

std::string user_dir()
{
    const fs::path p = fs::path(data_dir()) / "primitives" / "scad";
    boost::system::error_code ec;
    fs::create_directories(p, ec);
    return p.string();
}

std::string examples_dir() { return (fs::path(resources_dir()) / "primitives" / "scad").string(); }

std::vector<Script> list_scripts()
{
    std::vector<Script> out;
    auto scan = [&out](const std::string& dir, bool example) {
        boost::system::error_code ec;
        std::vector<Script>       found;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".scad")
                found.push_back({it->path().string(), it->path().stem().string(), example});
        std::sort(found.begin(), found.end(), [](const Script& a, const Script& b) { return a.name < b.name; });
        out.insert(out.end(), found.begin(), found.end());
    };
    scan(user_dir(), false);
    const size_t first_project = out.size();
    scan(projects_dir(), false);
    for (size_t i = first_project; i < out.size(); ++i)
        out[i].from_project = true;
    scan(examples_dir(), true);
    return out;
}

std::string find_script(const std::string& file_name)
{
    for (const std::string& dir : {user_dir(), projects_dir(), examples_dir()}) {
        const fs::path p = fs::path(dir) / file_name;
        boost::system::error_code ec;
        if (fs::exists(p, ec))
            return p.string();
    }
    return {};
}

static std::string fnv_hex(const std::string& s)
{
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", h);
    return buf;
}

std::string read_file(const std::string& path)
{
    boost::nowide::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string projects_dir()
{
    const fs::path p = fs::path(user_dir()) / "projects";
    boost::system::error_code ec;
    fs::create_directories(p, ec);
    return p.string();
}

std::string materialize(const std::string& file_name, const std::string& source)
{
    const fs::path name(file_name);
    const fs::path p = fs::path(projects_dir()) / (name.stem().string() + "-" + fnv_hex(source).substr(0, 8) + ".scad");
    boost::system::error_code ec;
    if (!fs::exists(p, ec) || read_file(p.string()) != source) {
        boost::nowide::ofstream f(p.string(), std::ios::binary | std::ios::trunc);
        f << source;
    }
    return p.string();
}

static const char* TRUST_KEY = "openscad_trusted_scripts";

bool is_trusted(const std::string& source)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr) return false;
    return (";" + cfg->get(TRUST_KEY) + ";").find(";" + fnv_hex(source) + ";") != std::string::npos;
}

void trust(const std::string& source)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr || is_trusted(source)) return;
    std::string list = cfg->get(TRUST_KEY);
    cfg->set(TRUST_KEY, list.empty() ? fnv_hex(source) : list + ";" + fnv_hex(source));
    cfg->save();
}

// -------------------------------------------------------------------------------------------------
// Runner
// -------------------------------------------------------------------------------------------------

// -------------------------------------------------------------------------------------------------
// Multi-part scripts
// -------------------------------------------------------------------------------------------------

int part_param(const std::vector<Param>& params)
{
    for (int i = 0; i < (int) params.size(); ++i) {
        const Param& p = params[i];
        if (p.type != Param::Choice || p.choices.size() < 2) continue;
        std::string n = p.name;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        if (n == "part" || n == "parts" || n == "part_to_render" || n == "render_part" || n == "show_part")
            return i;
    }
    return -1;
}

static bool is_all_choice(std::string v)
{
    v = unquote(v);
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    return v == "all" || v == "both" || v == "everything" || v == "assembly" || v == "assembled" || v == "*";
}

std::vector<std::pair<std::string, std::string>> part_choices(const Param& p)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& c : p.choices)
        if (!is_all_choice(c.first) && !is_all_choice(c.second))
            out.push_back(c);
    return out;
}

// -------------------------------------------------------------------------------------------------
// The 3mf OpenSCAD writes: one object, a colour per triangle (basematerials). Small own reader,
// only what OpenSCAD writes: vertices, triangles, the material of each triangle.
// -------------------------------------------------------------------------------------------------

static bool attr(const char* b, const char* e, const char* name, std::string& out)
{
    const std::string key = std::string(" ") + name + "=\"";
    const char*       f   = std::search(b, e, key.begin(), key.end());
    if (f == e) return false;
    f += key.size();
    const char* q = std::find(f, e, '"');
    if (q == e) return false;
    out.assign(f, q);
    return true;
}

static bool read_openscad_3mf(const std::string& path, indexed_triangle_set& its, std::string& color)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, path)) return false;
    size_t size = 0;
    void*  data = mz_zip_reader_extract_file_to_heap(&zip, "3D/3dmodel.model", &size, 0);
    close_zip_reader(&zip);
    if (data == nullptr) return false;
    const std::string xml((const char*) data, size);
    mz_free(data);

    std::vector<std::string>   base_colors; // basematerials, in order
    std::map<std::string, int> uses;        // material index -> triangles
    std::string                obj_pindex = "0";
    const char* b = xml.data();
    const char* e = b + xml.size();
    size_t      vbase = 0;
    for (const char* t = b; (t = std::find(t, e, '<')) != e; ++t) {
        const char* close = std::find(t, e, '>');
        if (close == e) break;
        std::string v1, v2, v3, x, y, z, p1;
        if (std::strncmp(t, "<object ", 8) == 0) {
            vbase = its.vertices.size();
            attr(t, close, "pindex", obj_pindex);
        } else if (std::strncmp(t, "<vertex ", 8) == 0 && attr(t, close, "x", x) && attr(t, close, "y", y) && attr(t, close, "z", z)) {
            double a, bb, c;
            if (parse_number(x, a) && parse_number(y, bb) && parse_number(z, c))
                its.vertices.emplace_back(float(a), float(bb), float(c));
        } else if (std::strncmp(t, "<triangle ", 10) == 0 && attr(t, close, "v1", v1) && attr(t, close, "v2", v2) && attr(t, close, "v3", v3)) {
            its.indices.emplace_back(int(vbase + std::atoi(v1.c_str())), int(vbase + std::atoi(v2.c_str())), int(vbase + std::atoi(v3.c_str())));
            if (!attr(t, close, "p1", p1)) p1 = obj_pindex;
            uses[p1]++;
        } else if (std::strncmp(t, "<base ", 6) == 0) {
            std::string dc;
            attr(t, close, "displaycolor", dc);
            base_colors.push_back(dc);
        }
        t = close;
    }
    // the colour most of the triangles carry; index 0 is OpenSCAD's "Default" (no color() given)
    int best = -1, best_n = 0;
    for (const auto& u : uses) {
        const int idx = std::atoi(u.first.c_str());
        if (idx > 0 && u.second > best_n) { best = idx; best_n = u.second; }
    }
    color.clear();
    if (best >= 0 && best < (int) base_colors.size() && base_colors[best].size() >= 7)
        color = base_colors[best].substr(0, 7);
    std::transform(color.begin(), color.end(), color.begin(), ::tolower);
    return !its.indices.empty();
}

// -------------------------------------------------------------------------------------------------
// Runner
// -------------------------------------------------------------------------------------------------

struct Runner::Shared
{
    std::mutex        mutex;
    std::atomic<int>  latest{0};
    std::atomic<int>  running{0}; // runs still going
    std::vector<Part> result;
    int               result_gen = -1;
    bool              fresh      = false;
    bool              failed     = false;
    std::string       error;
};

Runner::Runner() : m(std::make_shared<Shared>()) {}
Runner::~Runner() = default; // workers keep their own shared_ptr; nothing waits

Runner::State Runner::state() const
{
    std::lock_guard<std::mutex> lk(m->mutex);
    if (m->running.load() > 0) return State::Running;
    if (m->failed) return State::Failed;
    if (m->result_gen >= 0) return State::Done;
    return State::Idle;
}

std::string Runner::error() const
{
    std::lock_guard<std::mutex> lk(m->mutex);
    return m->error;
}

bool Runner::take(std::vector<Part>& out, int& generation)
{
    std::lock_guard<std::mutex> lk(m->mutex);
    if (!m->fresh) return false;
    out        = m->result;
    generation = m->result_gen;
    m->fresh   = false;
    return true;
}

static int run_openscad(const std::string& binary, const std::vector<std::string>& args, std::string& err_text)
{
    namespace bp = boost::process;
    try {
        bp::ipstream err;
#ifdef _WIN32
        // Paths are UTF-8 here; a narrow string would go through the ANSI code page and break on a user folder
        // with accents or non-Latin letters. Wide strings, and no console window flashing on every rebuild.
        std::vector<std::wstring> wargs;
        wargs.reserve(args.size());
        for (const std::string& a : args)
            wargs.push_back(boost::nowide::widen(a));
        bp::child c(bp::exe = boost::nowide::widen(binary), bp::args = wargs, bp::std_out > bp::null, bp::std_err > err,
                    bp::std_in < bp::null, bp::windows::hide);
#else
        bp::child    c(bp::exe = binary, bp::args = args, bp::std_out > bp::null, bp::std_err > err, bp::std_in < bp::null);
#endif
        std::string  line;
        while (std::getline(err, line))
            err_text += line + "\n";
        c.wait();
        return c.exit_code();
    } catch (const std::exception& e) {
        err_text += e.what();
        return -1;
    }
}

void Runner::start(const std::string& binary, const std::string& script, const std::vector<Param>& params)
{
    const int gen = ++m_generation;
    m->latest     = gen;
    // One run per piece of a multi-part script, else one run.
    struct Job { std::string label; std::vector<std::string> defines; };
    std::vector<Job> jobs;
    const int part = part_param(params);
    std::vector<std::string> common;
    for (int i = 0; i < (int) params.size(); ++i) {
        if (i == part) continue;
        common.push_back("-D");
        common.push_back(params[i].name + "=" + value_literal(params[i]));
    }
    if (part >= 0) {
        for (const auto& c : part_choices(params[part])) {
            Job j{c.second, common};
            j.defines.push_back("-D");
            j.defines.push_back(params[part].name + "=" + c.first);
            jobs.push_back(std::move(j));
        }
    }
    if (jobs.empty())
        jobs.push_back({std::string(), common});

    std::shared_ptr<Shared> sh = m;
    sh->running++;
    std::thread([sh, gen, binary, script, jobs]() {
        std::vector<Part> parts;
        std::string       why, err;
        bool              manifold = true;
        for (const Job& job : jobs) {
            if (gen != sh->latest.load()) break; // superseded: stop early
            const fs::path out = fs::temp_directory_path() / fs::unique_path("neoprim-%%%%-%%%%.3mf");
            auto make_args = [&](bool with_manifold) {
                std::vector<std::string> a = {"-o", out.string()};
                if (with_manifold) a.push_back("--backend=manifold");
                a.insert(a.end(), job.defines.begin(), job.defines.end());
                a.push_back(script);
                return a;
            };
            err.clear();
            int rc = run_openscad(binary, make_args(manifold), err);
            if (rc != 0 && manifold && (err.find("backend") != std::string::npos || err.find("unrecognised") != std::string::npos ||
                                        err.find("unrecognized") != std::string::npos)) {
                manifold = false; // older OpenSCAD without the Manifold engine
                err.clear();
                rc = run_openscad(binary, make_args(false), err);
            }
            boost::system::error_code ec;
            Part p;
            p.label = job.label;
            if (rc != 0 || !fs::exists(out, ec)) {
                why = "OpenSCAD failed (" + std::to_string(rc) + ")" + (job.label.empty() ? "" : " on " + job.label) + ".";
                fs::remove(out, ec);
                break;
            }
            if (read_openscad_3mf(out.string(), p.its, p.color))
                parts.push_back(std::move(p));
            fs::remove(out, ec);
        }
        if (why.empty() && parts.empty())
            why = "OpenSCAD made an empty model.";
        std::lock_guard<std::mutex> lk(sh->mutex);
        sh->running--;
        if (gen != sh->latest.load())
            return; // a newer run was asked for
        if (why.empty()) {
            sh->result     = std::move(parts);
            sh->result_gen = gen;
            sh->fresh      = true;
            sh->failed     = false;
            sh->error.clear();
        } else {
            sh->failed       = true;
            std::string tail = err.size() > 600 ? err.substr(err.size() - 600) : err;
            sh->error        = why + (tail.empty() ? "" : "\n" + trim(tail));
        }
    }).detach();
}

} // namespace NeoScad
} // namespace GUI
} // namespace Slic3r
