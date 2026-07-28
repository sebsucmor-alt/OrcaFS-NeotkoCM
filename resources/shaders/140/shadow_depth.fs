#version 140

// NEOTKO_SHADOW_TAG s229 (Fase 2): companion of shadow_depth.vs. Writes nothing — the shadow-map
// FBO has no color attachment at all (glDrawBuffer(GL_NONE), see ensure_shadow_map_fbo in
// GCodeViewer.cpp); the only output that matters is the implicit gl_FragDepth the rasterizer
// stores. An empty main() is valid GLSL and is the standard shadow-pass fragment shader.

void main()
{
}
