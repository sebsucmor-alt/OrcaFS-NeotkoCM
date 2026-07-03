#version 110

// NEOTKO_REALCOLOR_TAG s166 (item 4): legacy/compat-profile counterpart of
// 140/shells_gbuffer.fs — see that file for the full rationale. Single output, gl_FragColor
// (no MRT needed, only one attachment).

varying vec3 v_view_normal;
varying float v_eye_z;

void main()
{
    gl_FragColor = vec4(normalize(v_view_normal) * 0.5 + 0.5, v_eye_z);
}
