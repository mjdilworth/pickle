#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE
#include "video_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <linux/input.h>

static bool process_keystone_movement(app_context_t *app, double delta_time, double target_frame_time) {
    if (!app || !app->keystone || !app->input) {
        return false;
    }

    bool left = input_is_key_pressed(app->input, KEY_LEFT);
    bool right = input_is_key_pressed(app->input, KEY_RIGHT);
    bool up = input_is_key_pressed(app->input, KEY_UP);
    bool down = input_is_key_pressed(app->input, KEY_DOWN);

    if (app->input->use_stdin_fallback) {
        left = left || input_is_key_just_pressed(app->input, KEY_LEFT);
        right = right || input_is_key_just_pressed(app->input, KEY_RIGHT);
        up = up || input_is_key_just_pressed(app->input, KEY_UP);
        down = down || input_is_key_just_pressed(app->input, KEY_DOWN);
    }

    float move_x = 0.0f;
    float move_y = 0.0f;

    if (left != right) {
        move_x = left ? -1.0f : 1.0f;
    }
    if (up != down) {
        move_y = up ? 1.0f : -1.0f;
    }

    if (app->keystone->selected_corner < 0 || (move_x == 0.0f && move_y == 0.0f)) {
        return false;
    }

    float speed_scale = 1.0f;
    if (target_frame_time > 0.0 && delta_time > 0.0) {
        speed_scale = (float)(delta_time / target_frame_time);
        if (speed_scale < 0.25f) speed_scale = 0.25f;
        if (speed_scale > 3.0f) speed_scale = 3.0f;
    }

    keystone_move_corner(app->keystone, move_x * speed_scale, move_y * speed_scale);
    app->needs_redraw = true;
    return true;
}

int app_init(app_context_t *app, const char *video_file, bool loop_playback) {
    printf("app_init: Starting initialization...\n");
    fflush(stdout);
    
    memset(app, 0, sizeof(*app));
    app->video_file = video_file;
    app->running = true;
    app->loop_playback = loop_playback;

    // Allocate contexts
    app->drm = calloc(1, sizeof(display_ctx_t));
    app->gl = calloc(1, sizeof(gl_context_t));
    app->video = calloc(1, sizeof(video_context_t));
    app->keystone = calloc(1, sizeof(keystone_context_t));
    app->input = calloc(1, sizeof(input_context_t));

    if (!app->drm || !app->gl || !app->video || !app->keystone || !app->input) {
        fprintf(stderr, "Failed to allocate contexts\n");
        app_cleanup(app);
        return -1;
    }

    printf("app_init: Initializing DRM display...\n");
    fflush(stdout);
    
    // Initialize DRM display
    if (drm_init(app->drm) != 0) {
        fprintf(stderr, "Failed to initialize DRM display\n");
        app_cleanup(app);
        return -1;
    }

    // Initialize OpenGL context
    if (gl_init(app->gl, app->drm) != 0) {
        fprintf(stderr, "Failed to initialize OpenGL context\n");
        app_cleanup(app);
        return -1;
    }

    // Initialize video decoder
    if (video_init(app->video, video_file) != 0) {
        fprintf(stderr, "Failed to initialize video decoder\n");
        app_cleanup(app);
        return -1;
    }
    
    // Set loop playback if requested
    video_set_loop(app->video, loop_playback);

    // Initialize keystone correction
    if (keystone_init(app->keystone) != 0) {
        fprintf(stderr, "Failed to initialize keystone correction\n");
        app_cleanup(app);
        return -1;
    }
    
    // Load saved keystone settings if available
    if (keystone_load_settings(app->keystone) == 0) {
        printf("Loaded saved keystone settings from pickle_keystone.conf\n");
    } else {
        printf("No saved keystone settings found, using defaults\n");
    }

    // Initialize input handler
    if (input_init(app->input) != 0) {
        fprintf(stderr, "Failed to initialize input handler\n");
        app_cleanup(app);
        return -1;
    }

    gl_setup_buffers(app->gl);

    // Application initialization complete
    return 0;
}

void app_run(app_context_t *app) {
    struct timespec current_time, last_time;
    clock_gettime(CLOCK_MONOTONIC, &last_time);

    double target_frame_time = 1.0 / 60.0; // 60 FPS target
    if (app->video && app->video->fps > 0) {
        target_frame_time = 1.0 / app->video->fps;
        printf("Video FPS: %.2f, Target frame time: %.3fms\n", 
               app->video->fps, target_frame_time * 1000);
    }
    
    // IMPROVED: Simpler, more stable frame timing
    double frame_budget = target_frame_time;  // How much time we have per frame
    // 10% buffer for timing overhead
    
    // Adaptive frame timing variables
    double min_frame_time = target_frame_time;  // Minimum frame time (target FPS)
    double adaptive_frame_time = target_frame_time;  // Current adaptive frame time
    
    int consecutive_decode_failures = 0;
    
    // Frame timing diagnostics
    struct timespec decode_start, decode_end, render_start, render_end;
    double total_decode_time = 0, total_render_time = 0;
    int diagnostic_frame_count = 0;
    bool show_diagnostics = true;

    // Main loop starting
    double startup_time = (double)last_time.tv_sec + last_time.tv_nsec / 1e9;
    bool first_decode_attempted = false;
    
    while (app->running) {
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        
        // Calculate frame time
        double delta_time = (current_time.tv_sec - last_time.tv_sec) +
                           (current_time.tv_nsec - last_time.tv_nsec) / 1e9;
        
        double current_total_time = (double)current_time.tv_sec + current_time.tv_nsec / 1e9;
        double decode_time = 0.0, render_time = 0.0;
        
        // Handle input regardless of video state
        input_update(app->input);
        
        // Check for quit
        if (input_should_quit(app->input)) {
            printf("Quit requested by user\n");
            fflush(stdout);
            app->running = false;
            break;
        }

        // Handle keystone corner selection - match visual numbering
        // Key should select the corner with the matching visual number
        if (input_is_key_just_pressed(app->input, KEY_1)) {
            keystone_select_corner(app->keystone, CORNER_BOTTOM_LEFT);  // Visual "1" (bottom-left)
        } else if (input_is_key_just_pressed(app->input, KEY_2)) {
            keystone_select_corner(app->keystone, CORNER_BOTTOM_RIGHT); // Visual "2" (bottom-right)
        } else if (input_is_key_just_pressed(app->input, KEY_3)) {
            keystone_select_corner(app->keystone, CORNER_TOP_RIGHT);    // Visual "3" (top-right)
        } else if (input_is_key_just_pressed(app->input, KEY_4)) {
            keystone_select_corner(app->keystone, CORNER_TOP_LEFT);     // Visual "4" (top-left)
        }
        
        // Check for arrow key input (simply helps the corner movement by triggering input_is_key_pressed)

        // Reset keystone to defaults
        if (input_is_key_just_pressed(app->input, KEY_R)) {
            keystone_reset_corners(app->keystone);
            printf("Keystone reset to defaults\n");
        }
        
        // Save keystone settings (S key or P key for compatibility)
        if (app->input->save_keystone) {
            if (keystone_save_settings(app->keystone) == 0) {
                printf("Keystone settings saved to pickle_keystone.conf\n");
            } else {
                printf("Failed to save keystone settings\n");
            }
            app->input->save_keystone = false; // Reset flag
        }
        
        // Toggle corner visibility
        if (app->input->toggle_corners) {
            keystone_toggle_corners(app->keystone);
            app->input->toggle_corners = false; // Reset flag
        }
        
        // Toggle border visibility
        if (app->input->toggle_border) {
            keystone_toggle_border(app->keystone);
            app->input->toggle_border = false; // Reset flag
        }
        
        // Toggle help overlay visibility
        if (app->input->toggle_help) {
            keystone_toggle_help(app->keystone);
            app->input->toggle_help = false; // Reset flag
        }

        // Decode video frame if enough time has passed
        uint8_t *video_data = NULL;
        static int frame_count = 0;
        static uint8_t *last_video_data = NULL;
        
        if (delta_time >= frame_budget) {
            // Update timing first to maintain consistent frame rate
            last_time = current_time;
            
            if (frame_count == 0 && !first_decode_attempted) {
                printf("Attempting first frame decode...\n");
                fflush(stdout);
                first_decode_attempted = true;
            }
            
            // Add timeout for first decode to prevent hanging
            if (frame_count == 0 && (current_total_time - startup_time) > 5.0) {
                printf("Video decode timeout after 5 seconds, continuing without video...\n");
                fflush(stdout);
                frame_count = 1; // Skip further decode attempts
            } else {
                // Measure decode time
                clock_gettime(CLOCK_MONOTONIC, &decode_start);
                int decode_result = video_decode_frame(app->video);
                clock_gettime(CLOCK_MONOTONIC, &decode_end);
                
                decode_time = (decode_end.tv_sec - decode_start.tv_sec) +
                            (decode_end.tv_nsec - decode_start.tv_nsec) / 1e9;
                
                if (decode_result == 0) {
                    if (frame_count == 0) {
                        printf("First frame decoded successfully\n");
                        fflush(stdout);
                    }
                    video_data = video_get_rgb_data(app->video);
                    last_video_data = video_data;
                    frame_count++;
                    
                    // Update diagnostics
                    total_decode_time += decode_time;
                    diagnostic_frame_count++;
                    
                    // Show diagnostic info periodically
                    if (show_diagnostics && diagnostic_frame_count % 60 == 0) {
                        double avg_decode_time = total_decode_time / diagnostic_frame_count;
                        printf("Frame timing - Avg decode: %.1fms, Target: %.1fms\n",
                               avg_decode_time * 1000, target_frame_time * 1000);
                        fflush(stdout);
                    }
                    
                    // We now clear the arrow keys ONLY if we've successfully processed a movement
            // This allows for continued movement if a corner is not selected
            if (app->keystone->selected_corner < 0) {
                // No corner selected, clear key states to prevent movement later
                app->input->keys_pressed[KEY_UP] = false;
                app->input->keys_pressed[KEY_DOWN] = false;
                app->input->keys_pressed[KEY_LEFT] = false;
                app->input->keys_pressed[KEY_RIGHT] = false;
                // printf("No corner selected, clearing arrow key states\n");
            } else {
                // printf("Corner %d selected, preserving arrow key states\n", 
                //        app->keystone->selected_corner);
            }
                    static int slow_decode_count = 0;
                    if (decode_time > adaptive_frame_time * 2.0) { // More generous threshold
                        slow_decode_count++;
                        if (slow_decode_count == 1) {
                            printf("Slow decode detected (%.1fms), enabling frame skipping\n", decode_time * 1000);
                        }
                        
                        // Only skip 1 frame, and only every 3rd slow decode
                        if (slow_decode_count % 3 == 0) {
                            video_decode_frame(app->video); // Skip just 1 frame
                        }
                    } else {
                        slow_decode_count = 0; // Reset when decode is fast again
                    }
                    
                    // Successful decode - gradually speed up timing
                    consecutive_decode_failures = 0;
                    if (adaptive_frame_time > min_frame_time * 1.1) {
                        adaptive_frame_time = adaptive_frame_time * 0.98; // Gradually faster
                        if (adaptive_frame_time < min_frame_time * 1.1) {
                            adaptive_frame_time = min_frame_time * 1.1; // Keep slightly slower than target
                        }
                    }
                } else if (video_is_eof(app->video)) {
                    printf("Playback finished.\n");
                    app->running = false;
                    break;
                } else {
                    if (frame_count < 10) {  // Only show first few decode errors
                        printf("Video decode failed: %d\n", decode_result);
                        fflush(stdout);
                    }
                    // Use last good frame if decode fails
                    video_data = last_video_data;
                    
                    // Failed decode - slow down timing to give decoder more time
                    consecutive_decode_failures++;
                    if (consecutive_decode_failures >= 1) {
                        adaptive_frame_time = adaptive_frame_time * 1.8; // Slow down more aggressively
                        if (adaptive_frame_time > min_frame_time * 8) {
                            adaptive_frame_time = min_frame_time * 8; // Cap at 8x slower
                        }
                        consecutive_decode_failures = 0; // Reset counter
                    }
                }
            }
        } else {
            // Use the last decoded frame for rendering
            video_data = last_video_data;
        }

        // Get video dimensions
        int video_width, video_height;
        video_get_dimensions(app->video, &video_width, &video_height);
        if (video_width == 0) {
            video_width = 256;
            video_height = 256;
        }
        
        // Get YUV data for GPU rendering
        uint8_t *y_data, *u_data, *v_data;
        int y_stride, u_stride, v_stride;
        video_get_yuv_data(app->video, &y_data, &u_data, &v_data, &y_stride, &u_stride, &v_stride);
        
        static int debug_frame = 0;
        if (debug_frame < 2) {
            printf("Frame %d: YUV pointers: Y=%p U=%p V=%p\n", 
                   debug_frame, (void*)y_data, (void*)u_data, (void*)v_data);
            debug_frame++;
            fflush(stdout);
        }
        
        // Measure render time with detailed breakdown
        clock_gettime(CLOCK_MONOTONIC, &render_start);
        struct timespec gl_render_start, gl_render_end, overlay_start, overlay_end, swap_start, swap_end;
        
        // Time the main GL rendering
        clock_gettime(CLOCK_MONOTONIC, &gl_render_start);
        
        // Only render if we have valid YUV data
        if (y_data && u_data && v_data) {
            // Render with YUV data (GPU will do YUV→RGB conversion)
            gl_render_frame(app->gl, y_data, u_data, v_data, video_width, video_height, 
                          y_stride, u_stride, v_stride, app->drm, app->keystone);
        } else {
            // Render a blank frame or keep the last frame displayed
            gl_render_frame(app->gl, NULL, NULL, NULL, video_width, video_height, 
                          0, 0, 0, app->drm, app->keystone);
        }
        
        clock_gettime(CLOCK_MONOTONIC, &gl_render_end);
        
        // Time overlay rendering
        clock_gettime(CLOCK_MONOTONIC, &overlay_start);
        
        // OPTIMIZED: Only render overlays when actually needed
        bool any_overlay_visible = (app->keystone && (app->keystone->show_corners || 
                                                     app->keystone->show_border || 
                                                     app->keystone->show_help));
        if (any_overlay_visible) {
            // Only render the overlays that are actually visible
            if (app->keystone->show_corners) {
                gl_render_corners(app->gl, app->keystone);
            }
            if (app->keystone->show_border) {
                static int border_render_count = 0;
                if (border_render_count < 3) {  // Only log first 3 times
                    printf("Rendering border overlay (frame %d)\n", border_render_count + 1);
                    border_render_count++;
                }
                gl_render_border(app->gl, app->keystone);
            }
            if (app->keystone->show_help) {
                gl_render_help_overlay(app->gl, app->keystone);
            }
            
            // NOTE: State restoration is now handled by gl_render_frame()
            // The main video rendering function detects and fixes state corruption
            
            // Clear any OpenGL errors from overlay rendering
            while (glGetError() != GL_NO_ERROR) {
                // Clear error queue
            }
        }
        
        clock_gettime(CLOCK_MONOTONIC, &overlay_end);
        
        // Time buffer swapping
        clock_gettime(CLOCK_MONOTONIC, &swap_start);
        
        // Swap buffers after all rendering is complete
        gl_swap_buffers(app->gl, app->drm);
        
        clock_gettime(CLOCK_MONOTONIC, &swap_end);
        clock_gettime(CLOCK_MONOTONIC, &render_end);
        
        // Calculate detailed timing for debugging
        double gl_time = (gl_render_end.tv_sec - gl_render_start.tv_sec) + 
                        (gl_render_end.tv_nsec - gl_render_start.tv_nsec) / 1e9;
        double overlay_time = (overlay_end.tv_sec - overlay_start.tv_sec) + 
                             (overlay_end.tv_nsec - overlay_start.tv_nsec) / 1e9;
        double swap_time = (swap_end.tv_sec - swap_start.tv_sec) + 
                          (swap_end.tv_nsec - swap_start.tv_nsec) / 1e9;
        
        // Report detailed timing for long frames
        static int detailed_timing_count = 0;
        if (swap_time > 0.050 || (detailed_timing_count++ % 120 == 0)) { // Every 2 seconds or if swap > 50ms
            printf("Detailed timing - GL: %.1fms, Overlays: %.1fms, Swap: %.1fms\n",
                   gl_time * 1000, overlay_time * 1000, swap_time * 1000);
        }
        render_time = (render_end.tv_sec - render_start.tv_sec) +
                    (render_end.tv_nsec - render_start.tv_nsec) / 1e9;
        
        total_render_time += render_time;

        // Process keystone movement immediately after input update
        process_keystone_movement(app, delta_time, target_frame_time);

        // Implement variable frame rate control based on performance
        double total_frame_time = decode_time + render_time;
        double remaining_time = adaptive_frame_time - total_frame_time;
        
        if (remaining_time > 0.001) { // Only sleep if >1ms remaining
            // OPTIMIZED: Use nanosleep for better precision
            struct timespec sleep_time;
            sleep_time.tv_sec = 0;
            sleep_time.tv_nsec = (long)(remaining_time * 1000000000);
            nanosleep(&sleep_time, NULL);
            
            // Show render diagnostics periodically
            if (show_diagnostics && diagnostic_frame_count % 60 == 0 && diagnostic_frame_count > 0) {
                double avg_render_time = total_render_time / diagnostic_frame_count;
                printf("Render timing - Avg render: %.1fms, Total frame: %.1fms\n",
                       avg_render_time * 1000, total_frame_time * 1000);
                fflush(stdout);
            }
        } else {
            // We're running behind - no sleep, but don't make timing worse
            printf("Frame overrun: %.1fms (target: %.1fms)\n", 
                   total_frame_time * 1000, adaptive_frame_time * 1000);
            
            // FIXED: Only increase timing slightly if consistently overrunning
            static int overrun_count = 0;
            overrun_count++;
            if (overrun_count > 5) { // Only after 5 consecutive overruns
                adaptive_frame_time += 0.001; // Add 1ms, don't multiply
                if (adaptive_frame_time > min_frame_time * 2) {
                    adaptive_frame_time = min_frame_time * 2; // Max 33ms for 30fps
                }
                overrun_count = 0;
            }
        }
    }
}

void app_cleanup(app_context_t *app) {
    if (app->input) {
        input_cleanup(app->input);
        free(app->input);
    }
    if (app->keystone) {
        // Save keystone settings on exit
        if (keystone_save_settings(app->keystone) == 0) {
            printf("Keystone settings saved on exit\n");
        }
        keystone_cleanup(app->keystone);
        free(app->keystone);
    }
    if (app->video) {
        video_cleanup(app->video);
        free(app->video);
    }
    if (app->gl) {
        gl_cleanup(app->gl);
        free(app->gl);
    }
    if (app->drm) {
        drm_cleanup(app->drm);
        free(app->drm);
    }

    memset(app, 0, sizeof(*app));
    printf("Application cleanup complete\n");
}
