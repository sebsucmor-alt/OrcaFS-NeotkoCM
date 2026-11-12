// NEOTKO_PRIMGEN_TAG_START — 5-nov-2026, Primitive Generator phase 3: OpenSCAD scripts.
// Reads the Customizer variables of a .scad file and runs the user's OpenSCAD in the background.
#ifndef slic3r_NeotkoScad_hpp_
#define slic3r_NeotkoScad_hpp_

#include "libslic3r/TriangleMesh.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace NeoScad {

// One Customizer variable: `name = value; // [annotation]`, with the comment line above as its text.
struct Param
{
    enum Type { Number, Bool, Text, Choice };
    std::string name;
    std::string description; // comment line(s) right above
    std::string group;       // from /* [Group] */
    Type        type = Number;
    double      num  = 0.;   // Number, Bool (0/1), Choice (index)
    std::string text;        // Text
    double      mn = 0., mx = 1., step = 0.;
    bool        has_range = false;
    bool        integer   = false;
    // Choice: value as written in the script (number or quoted string) + its label
    std::vector<std::pair<std::string, std::string>> choices;
};

std::vector<Param> parse(const std::string& source);

// Multi-part scripts: the Customizer dropdown that draws one piece at a time (`part = "all"; //
// [all, base, text]`, the usual convention for multi-colour .scad). Index into params, or -1.
int part_param(const std::vector<Param>& params);
// Its pieces (value as written, label), without the "all / both / everything" entry.
std::vector<std::pair<std::string, std::string>> part_choices(const Param& p);

// One generated piece: its mesh in the script's coordinates and the colour it was given with
// color() ("#rrggbb", empty if none).
struct Part
{
    std::string          label;
    indexed_triangle_set its;
    std::string          color;
};

// -D value for one parameter, as OpenSCAD reads it
std::string value_literal(const Param& p);

// "scad:<file>|name=value|..." recipe stored on the ModelVolume (values percent-encoded)
std::string encode(const std::string& file_name, const std::vector<Param>& params);
bool        decode(const std::string& recipe, std::string& file_name, std::map<std::string, std::string>& values);
// Puts decoded values back into parsed params (by name)
void        apply(std::vector<Param>& params, const std::map<std::string, std::string>& values);
inline bool is_recipe(const std::string& recipe) { return recipe.rfind("scad:", 0) == 0; }

// The OpenSCAD binary: the path saved in the app config, else the usual install places. Empty if none.
std::string find_openscad();
void        set_openscad_path(const std::string& path);

// Where the scripts live: the user's folder (created on demand) and the examples shipped with the app.
std::string user_dir();
std::string examples_dir();
struct Script { std::string path, name; bool example = false; bool from_project = false; };
std::vector<Script> list_scripts();
std::string         find_script(const std::string& file_name); // user first, then examples

// Scripts that came inside a 3mf live in user_dir()/projects, named <stem>-<hash>.scad so two
// different scripts with the same name never overwrite each other. Returns the path.
std::string projects_dir();
std::string materialize(const std::string& file_name, const std::string& source);
// A script from someone else runs only after "Trust and run". Trust is remembered by content.
bool        is_trusted(const std::string& source);
void        trust(const std::string& source);
std::string read_file(const std::string& path);

// One OpenSCAD run in a worker thread. A newer start() makes the older result stale.
class Runner
{
public:
    enum class State { Idle, Running, Done, Failed };
    Runner();
    ~Runner();
    void  start(const std::string& binary, const std::string& script, const std::vector<Param>& params);
    State state() const;
    int   generation() const { return m_generation.load(); }
    // Takes the result of the last finished run (Done), once: one Part, or one per piece.
    bool  take(std::vector<Part>& out, int& generation);
    std::string error() const;

    struct Shared;

private:
    // Shared with detached worker threads, so closing the app never waits for OpenSCAD.
    std::shared_ptr<Shared> m;
    std::atomic<State>      m_state{State::Idle};
    std::atomic<int>        m_generation{0};
};

} // namespace NeoScad
} // namespace GUI
} // namespace Slic3r

#endif
// NEOTKO_PRIMGEN_TAG_END
