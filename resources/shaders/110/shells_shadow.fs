#version 110

// NEOTKO_REALCOLOR_TAG s166 (item 4.3): legacy/compat-profile counterpart of
// 140/shells_shadow.fs — see that file for the full rationale.

uniform vec4 u_shadow_color;

void main()
{
    gl_FragColor = u_shadow_color;
}
