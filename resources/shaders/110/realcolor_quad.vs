#version 110

// NEOTKO_REALCOLOR_TAG: legacy/compat-profile counterpart of 140/realcolor_quad.vs — fullscreen
// NDC quad, shared by the accumulate and present passes (both screen-space, no camera
// transform needed). Draw as GL_TRIANGLE_STRIP, 4 verts.
attribute vec2 v_position;
varying vec2 uv;

void main()
{
    uv = v_position * 0.5 + 0.5;
    gl_Position = vec4(v_position, 0.0, 1.0);
}
