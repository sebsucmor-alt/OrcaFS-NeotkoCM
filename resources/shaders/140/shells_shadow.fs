#version 140

// NEOTKO_REALCOLOR_TAG s166 (item 4.3): flat translucent dark color for the projected shadow —
// see shells_shadow.vs. The stencil test in render_shells() (GCodeViewer.cpp) already ensures
// each pixel is darkened by at most one overlapping shell's shadow, so a flat blended color is
// enough here — no per-pixel accumulation logic needed in the shader itself.

uniform vec4 u_shadow_color;

void main()
{
    gl_FragColor = u_shadow_color;
}
