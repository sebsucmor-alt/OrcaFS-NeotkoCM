#version 140

// NEOTKO_REALCOLOR_TAG: fullscreen NDC quad, shared by the accumulate and present passes
// (both are screen-space, no camera transform needed). Draw as GL_TRIANGLE_STRIP, 4 verts.
in vec2 v_position;
out vec2 uv;

void main()
{
    uv = v_position * 0.5 + 0.5;
    gl_Position = vec4(v_position, 0.0, 1.0);
}
