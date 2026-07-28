#version 110

// NEOTKO_SHADOW_TAG s229 (Fase 2): legacy/compat-profile counterpart of 140/shadow_depth.fs.
// Writes nothing — the shadow-map FBO has no color attachment; only the implicit depth matters.
// Note there is deliberately no gl_FragColor assignment: with glDrawBuffer(GL_NONE) there is
// nowhere for a color to go.

void main()
{
}
