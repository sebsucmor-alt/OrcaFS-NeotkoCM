// Primitive Generator 
#ifndef slic3r_NeotkoPrimitives_hpp_
#define slic3r_NeotkoPrimitives_hpp_

#include "TriangleMesh.hpp"

#include <map>
#include <string>
#include <vector>

namespace Slic3r {
namespace NeoPrim {

// One knob of a shape. The panel draws a slider (or chips when `choices` is not empty) from this.
struct Param
{
    std::string              key;      // short, [a-z0-9_] only: it travels in the recipe string
    std::string              label;    // name on screen
    double                   def  = 0.;
    double                   mn   = 0.;
    double                   mx   = 1.;
    std::string              unit;     // "mm", "°", "" ...
    bool                     integer = false;
    std::vector<std::string> choices;  // when not empty: value = index into choices
    int                      mark = 0; // number drawn on the sketch (0 = none)
    std::string              tip;
};

struct Shape
{
    std::string        id;    // [a-z0-9_], first field of the recipe
    std::string        name;  // on screen
    std::string        group; // "Basic", "Polyhedra", "Mechanical", "Organic"
    std::string        tip;
    std::vector<Param> params;
};

using Values = std::map<std::string, double>;

const std::vector<Shape>& shapes();
const Shape*              find_shape(const std::string& id);
Values                    defaults(const Shape& s);
void                      clamp_values(const Shape& s, Values& v);

// Mesh in mm, sitting on z = 0 and centred on X/Y. Empty on bad input.
indexed_triangle_set build(const std::string& id, const Values& v);

// Recipe: "ring;d=18;w=3;h=6". Only [a-z0-9_;=.-] so it goes into 3mf metadata as is.
std::string encode(const std::string& id, const Values& v);
bool        decode(const std::string& recipe, std::string& id, Values& v);

} // namespace NeoPrim
} // namespace Slic3r

#endif
