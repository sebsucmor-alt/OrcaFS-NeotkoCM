// NEOTKO_S3DFACTORY_TAG_START
//
// Simplify3D .factory file importer for OrcaSlicer / FullSpectrum fork.
//
// Supports Simplify3D .factory format versions 4.x and 5.x.
//
// Binary container format (no file extension magic — identified by the .factory extension):
//   Sequence of entries, each:
//     [4 bytes BE] name length in bytes (UTF-16 BE)
//     [N bytes]    entry name in UTF-16 BE
//     [4 bytes BE] compressed data length (zlib, NOT gzip)
//     [4 bytes BE] uncompressed size hint (first 4 bytes of compressed block)
//     [M-4 bytes]  zlib-compressed payload
//
// v5 layout (has "contents.xml"):
//   contents.xml  — XML listing <model> entries with <path>, <modelName>, <groupName>
//   Models/ModelN.stl — binary STL; vertices are in absolute world space
//
// v4 layout (no "contents.xml"):
//   Models/ModelN.stl  — binary STL; vertices in world space
//   Models/ModelN.info — CSV: key,value pairs (filename, groupName, translate…)
//
// Objects are named "[Ln] modelname" where n = group number extracted from groupName.
//
// Author: Neotko / FullSpectrum fork
// NEOTKO_S3DFACTORY_TAG_END

#pragma once
#include <string>

namespace Slic3r {

class Model;

// Load a Simplify3D .factory file into model.
// Returns true if at least one object was successfully imported.
// Throws Slic3r::RuntimeError on critical failures.
// Set env ORCA_DEBUG_S3DFACTORY=1 for verbose output to /tmp/s3dfactory_debug.txt
bool load_s3d_factory(const std::string& path, Model& model);

} // namespace Slic3r
