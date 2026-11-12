#version 110

// NEOTKO_REALCOLOR_TAG s166 (item 4): legacy/compat-profile counterpart of
// 140/shells_gbuffer.fs — see that file for the full rationale. Single output, gl_FragColor
// (no MRT needed, only one attachment).

varying vec3 v_view_normal;
varying float v_eye_z;
varying vec3 v_view_pos;
uniform bool u_derived_normal;   // NEOTKO_SHAREDVERTS_TAG 4-nov

void main()
{
    vec3 nk_n = normalize(v_view_normal);
    if (u_derived_normal) {
        nk_n = normalize(cross(dFdx(v_view_pos), dFdy(v_view_pos)));
        if (dot(nk_n, -v_view_pos) < 0.0) nk_n = -nk_n;
    }
    gl_FragColor = vec4(nk_n * 0.5 + 0.5, v_eye_z);
}
