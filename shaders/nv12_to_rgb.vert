#version 310 es
precision highp float;

// Simple vertex shader for NV12 conversion
// Creates a full-screen quad for texture sampling

in vec2 a_position;
in vec2 a_tex_coord;

out vec2 v_tex_coord;

void main() {
    v_tex_coord = a_tex_coord;
    gl_Position = vec4(a_position, 0.0, 1.0);
}