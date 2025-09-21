#ifndef PICKLE_SHADER_H
#define PICKLE_SHADER_H

#include <GLES2/gl2.h>
#include <stdbool.h>

// Shader compilation and program linking
GLuint compile_shader(GLenum type, const char *source);
GLuint link_program(GLuint vertex_shader, GLuint fragment_shader);
void cleanup_shader_resources(GLuint program, GLuint vertex_shader, GLuint fragment_shader);

// Shader source strings
extern const char *g_vertex_shader_src;
extern const char *g_fragment_shader_src;
extern const char *g_border_vs_src;
extern const char *g_border_fs_src;

#endif // PICKLE_SHADER_H