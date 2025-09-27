#version 310 es
precision highp float;

// NV12 to RGB conversion fragment shader
// Input: Two textures - Y plane (luminance) and UV plane (chrominance)
// Output: RGB color

in vec2 v_tex_coord;
out vec4 frag_color;

uniform sampler2D u_texture_y;   // Y plane texture
uniform sampler2D u_texture_uv;  // UV plane texture

void main() {
    // Sample Y component at full resolution
    float y = texture(u_texture_y, v_tex_coord).r;
    
    // Sample UV components at half resolution (NV12 is 4:2:0 subsampled)
    vec2 uv = texture(u_texture_uv, v_tex_coord).rg;
    
    // Convert from [0,1] to proper YUV ranges
    // Y: 16-235 -> 0-1, UV: 16-240 -> -0.5 to 0.5
    y = (y * 255.0 - 16.0) / 219.0;
    float u = (uv.r * 255.0 - 128.0) / 224.0;
    float v = (uv.g * 255.0 - 128.0) / 224.0;
    
    // BT.709 YUV to RGB conversion matrix (HD standard)
    vec3 rgb;
    rgb.r = y + 1.5748 * v;
    rgb.g = y - 0.1873 * u - 0.4681 * v;
    rgb.b = y + 1.8556 * u;
    
    // Clamp to valid range and output
    frag_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}