#include "stats_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <GLES3/gl3.h>
#include <math.h>

// Global stats overlay instance
stats_overlay_t g_stats_overlay;

// Simple text rendering constants
#define CHAR_WIDTH 6
#define CHAR_HEIGHT 12
#define LINE_SPACING 2
#define OVERLAY_PADDING 6

// Initialize stats overlay
void stats_overlay_init(stats_overlay_t *stats) {
    memset(stats, 0, sizeof(stats_overlay_t));
    gettimeofday(&stats->last_fps_update, NULL);
    gettimeofday(&stats->last_cpu_update, NULL);
    stats->x_pos = OVERLAY_PADDING;
    stats->y_pos = OVERLAY_PADDING;
}

// Update FPS counter
static void update_fps(stats_overlay_t *stats) {
    stats->frame_count++;
    
    struct timeval now;
    gettimeofday(&now, NULL);
    
    float elapsed = (float)(now.tv_sec - stats->last_fps_update.tv_sec) + 
                   (float)(now.tv_usec - stats->last_fps_update.tv_usec) / 1000000.0f;
    
    if (elapsed >= 1.0f) {
        stats->current_fps = (float)stats->frame_count / elapsed;
        stats->frame_count = 0;
        stats->last_fps_update = now;
    }
}

// Read CPU usage from /proc/stat
static void update_cpu_usage(stats_overlay_t *stats) {
    struct timeval now;
    gettimeofday(&now, NULL);
    
    float elapsed = (float)(now.tv_sec - stats->last_cpu_update.tv_sec) + 
                   (float)(now.tv_usec - stats->last_cpu_update.tv_usec) / 1000000.0f;
    
    // Update CPU usage every 0.5 seconds to reduce overhead
    if (elapsed < 0.5f) return;
    
    FILE *stat_file = fopen("/proc/stat", "r");
    if (!stat_file) return;
    
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    if (fscanf(stat_file, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) == 8) {
        
        unsigned long long total_time = user + nice + system + idle + iowait + irq + softirq + steal;
        unsigned long long idle_time = idle + iowait;
        
        if (stats->last_total_time > 0) {
            unsigned long long total_diff = total_time - stats->last_total_time;
            unsigned long long idle_diff = idle_time - stats->last_idle_time;
            
            if (total_diff > 0) {
                stats->cpu_usage = 100.0f * (1.0f - (float)idle_diff / (float)total_diff);
            }
        }
        
        stats->last_total_time = total_time;
        stats->last_idle_time = idle_time;
        stats->last_cpu_update = now;
    }
    
    fclose(stat_file);
}

// Read memory usage from /proc/meminfo
static void update_memory_usage(stats_overlay_t *stats) {
    FILE *meminfo = fopen("/proc/meminfo", "r");
    if (!meminfo) return;
    
    unsigned long total_kb = 0, free_kb = 0, buffers_kb = 0, cached_kb = 0;
    char line[256];
    
    while (fgets(line, sizeof(line), meminfo)) {
        if (sscanf(line, "MemTotal: %lu kB", &total_kb) == 1) continue;
        if (sscanf(line, "MemFree: %lu kB", &free_kb) == 1) continue;
        if (sscanf(line, "Buffers: %lu kB", &buffers_kb) == 1) continue;
        if (sscanf(line, "Cached: %lu kB", &cached_kb) == 1) continue;
    }
    
    fclose(meminfo);
    
    if (total_kb > 0) {
        unsigned long used_kb = total_kb - free_kb - buffers_kb - cached_kb;
        stats->memory_usage_mb = (float)used_kb / 1024.0f;
    }
}

// Called at start of render frame
void stats_overlay_render_frame_start(stats_overlay_t *stats) {
    gettimeofday(&stats->last_render_start, NULL);
}

// Called at end of render frame
void stats_overlay_render_frame_end(stats_overlay_t *stats) {
    gettimeofday(&stats->last_render_end, NULL);
    
    float render_time = (float)(stats->last_render_end.tv_sec - stats->last_render_start.tv_sec) * 1000.0f +
                       (float)(stats->last_render_end.tv_usec - stats->last_render_start.tv_usec) / 1000.0f;
    
    // Simple moving average for render time
    if (stats->avg_render_time_ms == 0.0f) {
        stats->avg_render_time_ms = render_time;
    } else {
        stats->avg_render_time_ms = stats->avg_render_time_ms * 0.9f + render_time * 0.1f;
    }
    
    // Estimate GPU usage based on render time vs frame time
    float target_frame_time = 1000.0f / 60.0f; // Assuming 60 FPS target
    stats->gpu_usage = fminf(100.0f, (stats->avg_render_time_ms / target_frame_time) * 100.0f);
}

// Update all stats
void stats_overlay_update(stats_overlay_t *stats) {
    update_fps(stats);
    update_cpu_usage(stats);
    update_memory_usage(stats);
}

// Simple shader for text rendering
static GLuint g_text_shader_program = 0;
static GLint g_text_u_color_loc = -1;
static GLint g_text_a_position_loc = -1;

// Simple text shader setup
static bool init_text_shader() {
    if (g_text_shader_program != 0) return true;
    
    const char *vertex_shader_source = 
        "#version 300 es\n"
        "precision highp float;\n"
        "in vec2 a_position;\n"
        "void main() {\n"
        "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
        "}\n";
    
    const char *fragment_shader_source = 
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform vec4 u_color;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    fragColor = u_color;\n"
        "}\n";
    
    // Compile vertex shader
    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
    glCompileShader(vertex_shader);
    
    // Check compilation
    GLint success;
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetShaderInfoLog(vertex_shader, 512, NULL, info_log);
        fprintf(stderr, "Text vertex shader compilation failed: %s\n", info_log);
        return false;
    }
    
    // Compile fragment shader
    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &fragment_shader_source, NULL);
    glCompileShader(fragment_shader);
    
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetShaderInfoLog(fragment_shader, 512, NULL, info_log);
        fprintf(stderr, "Text fragment shader compilation failed: %s\n", info_log);
        return false;
    }
    
    // Create program
    g_text_shader_program = glCreateProgram();
    glAttachShader(g_text_shader_program, vertex_shader);
    glAttachShader(g_text_shader_program, fragment_shader);
    glLinkProgram(g_text_shader_program);
    
    glGetProgramiv(g_text_shader_program, GL_LINK_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetProgramInfoLog(g_text_shader_program, 512, NULL, info_log);
        fprintf(stderr, "Text shader program linking failed: %s\n", info_log);
        return false;
    }
    
    // Get uniform and attribute locations
    g_text_u_color_loc = glGetUniformLocation(g_text_shader_program, "u_color");
    g_text_a_position_loc = glGetAttribLocation(g_text_shader_program, "a_position");
    
    // Clean up
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    
    return true;
}

// Simple 5x7 bitmap font data for basic characters
static const unsigned char font_5x7[][7] = {
    // Space (32)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // ! (33)  
    {0x04, 0x04, 0x04, 0x04, 0x00, 0x04, 0x00},
    // " (34)
    {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00},
    // # (35)
    {0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x00, 0x00},
    // $ (36)
    {0x0E, 0x14, 0x0E, 0x05, 0x1E, 0x04, 0x00},
    // % (37)  
    {0x18, 0x19, 0x02, 0x04, 0x13, 0x03, 0x00},
    // & (38)
    {0x08, 0x14, 0x08, 0x15, 0x12, 0x0D, 0x00},
    // ' (39)
    {0x04, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00},
    // ( (40)
    {0x02, 0x04, 0x04, 0x04, 0x04, 0x02, 0x00},
    // ) (41)  
    {0x08, 0x04, 0x04, 0x04, 0x04, 0x08, 0x00},
    // * (42)
    {0x04, 0x15, 0x0E, 0x15, 0x04, 0x00, 0x00},
    // + (43)
    {0x00, 0x04, 0x0E, 0x04, 0x00, 0x00, 0x00},
    // , (44)
    {0x00, 0x00, 0x00, 0x00, 0x04, 0x08, 0x00},
    // - (45)
    {0x00, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x00},
    // . (46)
    {0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00},
    // / (47)
    {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00},
    // 0 (48)
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x0E, 0x00},
    // 1 (49)
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x0E, 0x00},
    // 2 (50)
    {0x0E, 0x11, 0x02, 0x04, 0x08, 0x1F, 0x00},
    // 3 (51)
    {0x1F, 0x02, 0x06, 0x01, 0x11, 0x0E, 0x00},
    // 4 (52)
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x00},
    // 5 (53)
    {0x1F, 0x10, 0x1E, 0x01, 0x11, 0x0E, 0x00},
    // 6 (54)
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x0E, 0x00},
    // 7 (55)
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x00},
    // 8 (56)
    {0x0E, 0x11, 0x0E, 0x11, 0x11, 0x0E, 0x00},
    // 9 (57)
    {0x0E, 0x11, 0x0F, 0x01, 0x02, 0x0C, 0x00},
    // : (58)
    {0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00}
};

// Get font data for a character (returns index into font_5x7 array)
static int get_char_index(char c) {
    if (c >= 32 && c <= 58) {
        return c - 32;
    }
    // Handle additional characters
    switch (c) {
        case 'A': case 'a': return 33; // Use pattern similar to A
        case 'B': case 'b': return 34;
        case 'C': case 'c': return 35; 
        case 'D': case 'd': return 36;
        case 'E': case 'e': return 37;
        case 'F': case 'f': return 38;
        case 'G': case 'g': return 39;
        case 'P': case 'p': return 40;
        case 'R': case 'r': return 41;
        case 'S': case 's': return 42;
        case 'U': case 'u': return 43;
        case 'M': case 'm': return 44;
        default: return 0; // space
    }
}

// Add more font patterns for letters we need
static const unsigned char font_letters[][7] = {
    // A (33)
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x00},
    // B (34)  
    {0x1E, 0x11, 0x1E, 0x11, 0x11, 0x1E, 0x00},
    // C (35)
    {0x0E, 0x11, 0x10, 0x10, 0x11, 0x0E, 0x00},
    // D (36)
    {0x1C, 0x12, 0x11, 0x11, 0x12, 0x1C, 0x00},
    // E (37)
    {0x1F, 0x10, 0x1E, 0x10, 0x10, 0x1F, 0x00},
    // F (38)
    {0x1F, 0x10, 0x1E, 0x10, 0x10, 0x10, 0x00},
    // G (39)
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x0F, 0x00},
    // P (40)
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x00},
    // R (41)
    {0x1E, 0x11, 0x11, 0x1E, 0x12, 0x11, 0x00},
    // S (42)  
    {0x0F, 0x10, 0x0E, 0x01, 0x01, 0x1E, 0x00},
    // U (43)
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00},
    // M (44)
    {0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x00}
};

static void render_char(char c, int x, int y, int screen_width, int screen_height) {
    if (!init_text_shader()) return;
    
    // Get character pattern
    int char_idx = get_char_index(c);
    const unsigned char* pattern;
    
    if (char_idx >= 33) {
        // Use letter patterns
        pattern = font_letters[char_idx - 33];
    } else if (char_idx >= 0) {
        // Use number/symbol patterns  
        pattern = font_5x7[char_idx];
    } else {
        return; // Unknown character
    }
    
    // Enable blending for text overlay
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_text_shader_program);
    
    // Set bright green color
    glUniform4f(g_text_u_color_loc, 0.0f, 1.0f, 0.0f, 1.0f);
    
    // Render each pixel of the character bitmap
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (pattern[row] & (1 << (4 - col))) {
                // Calculate pixel position
                int px = x + col;
                int py = y + row;
                
                // Convert to normalized coordinates  
                float x1 = (2.0f * (float)px) / (float)screen_width - 1.0f;
                float y1 = 1.0f - (2.0f * (float)py) / (float)screen_height;
                float x2 = (2.0f * (float)(px + 1)) / (float)screen_width - 1.0f;
                float y2 = 1.0f - (2.0f * (float)(py + 1)) / (float)screen_height;
                
                float vertices[] = {
                    x1, y1, x2, y1, x2, y2, x1, y2
                };
                GLuint indices[] = {0, 1, 2, 2, 3, 0};
                
                GLuint vbo, ebo;
                glGenBuffers(1, &vbo);  
                glGenBuffers(1, &ebo);
                
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
                
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
                
                glVertexAttribPointer((GLuint)g_text_a_position_loc, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
                glEnableVertexAttribArray((GLuint)g_text_a_position_loc);
                
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
                
                glDisableVertexAttribArray((GLuint)g_text_a_position_loc);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                glDeleteBuffers(1, &vbo);
                glDeleteBuffers(1, &ebo);
            }
        }
    }
    
    glDisable(GL_BLEND);
    glUseProgram(0);
}

// Render text string
static void render_text(const char *text, int x, int y, int screen_width, int screen_height) {
    int char_x = x;
    
    for (int i = 0; text[i] != '\0'; i++) {
        render_char(text[i], char_x, y, screen_width, screen_height);
        char_x += CHAR_WIDTH;
    }
}

// Render a background rectangle for the stats overlay
static void render_background(int x, int y, int width, int height, int screen_width, int screen_height) {
    if (!init_text_shader()) return;
    
    // Convert screen coordinates to normalized device coordinates
    float x1 = (2.0f * (float)x) / (float)screen_width - 1.0f;
    float y1 = 1.0f - (2.0f * (float)y) / (float)screen_height;
    float x2 = (2.0f * (float)(x + width)) / (float)screen_width - 1.0f;
    float y2 = 1.0f - (2.0f * (float)(y + height)) / (float)screen_height;
    
    // Create background rectangle
    float vertices[] = {
        x1, y1,  // top-left
        x2, y1,  // top-right
        x2, y2,  // bottom-right
        x1, y2   // bottom-left
    };
    
    GLuint indices[] = {0, 1, 2, 2, 3, 0};
    
    // Enable blending
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Use shader and set semi-transparent black background
    glUseProgram(g_text_shader_program);
    glUniform4f(g_text_u_color_loc, 0.0f, 0.0f, 0.0f, 0.9f);
    
    // Create and bind buffers
    GLuint vbo, ebo;
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    
    // Set up vertex attributes
    glVertexAttribPointer((GLuint)g_text_a_position_loc, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray((GLuint)g_text_a_position_loc);
    
    // Draw background
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    // Clean up
    glDisableVertexAttribArray((GLuint)g_text_a_position_loc);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ebo);
    
    glDisable(GL_BLEND);
    glUseProgram(0);
}

// Render the stats overlay
void stats_overlay_render_text(stats_overlay_t *stats, int screen_width, int screen_height) {
    char text_lines[5][64];
    int num_lines = 5;
    
    // Prepare text lines
    snprintf(text_lines[0], sizeof(text_lines[0]), "FPS: %.1f", stats->current_fps > 0 ? stats->current_fps : 60.0f);
    snprintf(text_lines[1], sizeof(text_lines[1]), "CPU: %.1f%%", stats->cpu_usage);
    snprintf(text_lines[2], sizeof(text_lines[2]), "GPU: %.1f%%", stats->gpu_usage);
    snprintf(text_lines[3], sizeof(text_lines[3]), "RAM: %.0f MB", stats->memory_usage_mb);
    snprintf(text_lines[4], sizeof(text_lines[4]), "Render: %.2f ms", stats->avg_render_time_ms);
    
    // Calculate maximum text width for background
    int max_text_width = 0;
    for (int i = 0; i < num_lines; i++) {
        int text_width = (int)strlen(text_lines[i]) * CHAR_WIDTH;
        if (text_width > max_text_width) {
            max_text_width = text_width;
        }
    }
    
    // Calculate background dimensions
    int bg_width = max_text_width + (OVERLAY_PADDING * 2);
    int bg_height = num_lines * (CHAR_HEIGHT + LINE_SPACING) - LINE_SPACING + (OVERLAY_PADDING * 2);
    
    // Render background
    render_background(stats->x_pos - OVERLAY_PADDING, stats->y_pos - OVERLAY_PADDING, 
                     bg_width, bg_height, screen_width, screen_height);
    
    // Render text lines
    int line_y = stats->y_pos;
    for (int i = 0; i < num_lines; i++) {
        render_text(text_lines[i], stats->x_pos, line_y, screen_width, screen_height);
        line_y += CHAR_HEIGHT + LINE_SPACING;
    }
}