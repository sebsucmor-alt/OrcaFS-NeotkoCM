#version 140

// NEOTKO_REALCOLOR_TAG s166 (item 4): writes (view-space normal, linear eye-space depth) for
// shells_lit.fs's AO kernel to sample. Single opaque z-tested pass (not peeled — shells are
// solid meshes, standard depth test already resolves the nearest surface per pixel, unlike
// RealColor's translucent toolpath stack, see 03_DEPTH_PEELING_RENDER.md). v_eye_z is always
// > 0 for real geometry in front of the camera, so shells_lit.fs uses out_gbuffer.a <= 0.0 as
// its "no geometry here" sentinel — see ensure_shells_ao_fbo's clear color in GCodeViewer.cpp.

in vec3 v_view_normal;
in float v_eye_z;

out vec4 out_gbuffer;

void main()
{
    out_gbuffer = vec4(normalize(v_view_normal) * 0.5 + 0.5, v_eye_z);
}
