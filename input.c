#include "input.h"
#include "utils.h"
#include "keystone.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h> // For strcasecmp

// Define logging macros if not already defined
#ifndef LOG_INPUT
#define LOG_INPUT(fmt, ...) fprintf(stderr, "[INPUT] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef LOG_ERROR
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef LOG_WARN
#define LOG_WARN(fmt, ...) fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef LOG_INFO
#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef RETURN_ERROR
#define RETURN_ERROR(msg) do { LOG_ERROR("%s", msg); return false; } while(0)
#endif

#ifndef RETURN_ERROR_ERRNO
#define RETURN_ERROR_ERRNO(msg) do { LOG_ERROR("%s: %s", msg, strerror(errno)); return false; } while(0)
#endif

// Global state
static int g_joystick_fd = -1;        // File descriptor for joystick
static bool g_joystick_enabled = false; // Whether joystick support is enabled
static char g_joystick_name[128];     // Name of the connected joystick
static struct timeval g_last_js_event_time = {0}; // For debouncing joystick events
static gamepad_layout_t g_gamepad_layout = GP_LAYOUT_AUTO;

// Corner selection
static int g_selected_corner = -1;

// Button mappings
static bool g_use_label_mapping = false;
static bool g_x_cycle_enabled = true;
static int g_btn_code_X = 2;
static int g_btn_code_A = 0;
static int g_btn_code_B = 1;
static int g_btn_code_Y = 3;
static int g_corner_for_X = 0;
static int g_corner_for_A = 1;
static int g_corner_for_B = 2;
static int g_corner_for_Y = 3;
static int g_cycle_button_code = 2;  // Default to X
static int g_help_button_code = 1;   // Default to B

// Quit combo tracking
static bool g_js_start_down = false;
static bool g_js_select_down = false;
static struct timeval g_js_start_time = {0};
static struct timeval g_js_select_time = {0};

// Configuration helper functions
static void parse_btn_code_env(void) {
    const char *env = getenv("PICKLE_BTN_CODES");
    if (env && *env) {
        int x, a, b, y;
        if (sscanf(env, "%d,%d,%d,%d", &x, &a, &b, &y) == 4) {
            g_btn_code_X = x; g_btn_code_A = a; g_btn_code_B = b; g_btn_code_Y = y;
            g_use_label_mapping = true;
        }
    }
}

static void parse_corner_map_env(void) {
    const char *env = getenv("PICKLE_CORNER_MAP");
    if (env && *env) {
        int x, a, b, y;
        if (sscanf(env, "%d,%d,%d,%d", &x, &a, &b, &y) == 4) {
            g_corner_for_X = x; g_corner_for_A = a; g_corner_for_B = b; g_corner_for_Y = y;
            g_use_label_mapping = true;
        }
    }
}

static int label_to_code_default(const char *label) {
    if (!label) return -1;
    if (!strcasecmp(label, "X")) return JS_BUTTON_X;
    if (!strcasecmp(label, "A")) return JS_BUTTON_A;
    if (!strcasecmp(label, "B")) return JS_BUTTON_B;
    if (!strcasecmp(label, "Y")) return JS_BUTTON_Y;
    return -1;
}

/**
 * Configure button mappings based on environment variables
 */
void setup_label_mapping(void) {
    parse_btn_code_env();
    parse_corner_map_env();
    
    const char *use = getenv("PICKLE_USE_LABEL_MAPPING");
    if (use && *use && atoi(use) != 0) g_use_label_mapping = true;
    
    const char *xc = getenv("PICKLE_X_CYCLE");
    if (xc && *xc) g_x_cycle_enabled = (atoi(xc) != 0);
    
    if (g_use_label_mapping) {
        LOG_INFO("Using explicit ABXY mapping: codes X=%d A=%d B=%d Y=%d; corners X=%d A=%d B=%d Y=%d",
                 g_btn_code_X, g_btn_code_A, g_btn_code_B, g_btn_code_Y,
                 g_corner_for_X, g_corner_for_A, g_corner_for_B, g_corner_for_Y);
    }
    
    LOG_INFO("X button cycling: %s (PICKLE_X_CYCLE=%s)", 
             g_x_cycle_enabled ? "enabled" : "disabled", 
             xc && *xc ? xc : "(default)");
}

/**
 * Configure special button assignments based on gamepad layout
 */
void configure_special_buttons(void) {
    // Defaults based on layout or explicit label mapping
    if (g_use_label_mapping) {
        g_cycle_button_code = g_btn_code_X;
        g_help_button_code = g_btn_code_B;
    } else {
        if (g_gamepad_layout == GP_LAYOUT_NINTENDO) {
            // Typical Nintendo-style mapping: B=0, A=1, Y=2, X=3
            g_cycle_button_code = 3; // physical X
            g_help_button_code = 0;  // physical B
        } else {
            // Xbox-style default mapping
            g_cycle_button_code = JS_BUTTON_X;
            g_help_button_code = JS_BUTTON_B;
        }
    }

    // Env overrides: numeric or label
    const char *cb = getenv("PICKLE_CYCLE_BUTTON");
    if (cb && *cb) {
        char *end=NULL; 
        long v = strtol(cb, &end, 10);
        if (end && *end=='\0') {
            g_cycle_button_code = (int)v;
        } else {
            int code = g_use_label_mapping ?
                (!strcasecmp(cb,"X")?g_btn_code_X:!strcasecmp(cb,"A")?g_btn_code_A:!strcasecmp(cb,"B")?g_btn_code_B:!strcasecmp(cb,"Y")?g_btn_code_Y:-1)
                : label_to_code_default(cb);
            if (code >= 0) g_cycle_button_code = code;
        }
    }
}

/**
 * Initialize joystick/gamepad support
 * Attempts to open the first joystick device and set up event handling
 * 
 * @return true if a joystick was found and initialized
 */
bool init_joystick(void) {
    // Try to open joystick device
    const char *device = "/dev/input/js0";
    g_joystick_fd = open(device, O_RDONLY | O_NONBLOCK);
    
    if (g_joystick_fd < 0) {
        LOG_WARN("Could not open joystick at %s: %s", device, strerror(errno));
        return false;
    }
    
    // Get joystick name
    if (ioctl(g_joystick_fd, JSIOCGNAME(sizeof(g_joystick_name)), g_joystick_name) < 0) {
        strcpy(g_joystick_name, "Unknown Controller");
    }
    
    LOG_INFO("Joystick initialized: %s", g_joystick_name);
    g_joystick_enabled = true;
    
    // Initialize the first corner as selected
    g_selected_corner = 0;

    // Determine gamepad layout
    const char *layout_env = getenv("PICKLE_GAMEPAD_LAYOUT");
    if (layout_env && *layout_env) {
        if (!strcasecmp(layout_env, "xbox")) g_gamepad_layout = GP_LAYOUT_XBOX;
        else if (!strcasecmp(layout_env, "nintendo")) g_gamepad_layout = GP_LAYOUT_NINTENDO;
        else g_gamepad_layout = GP_LAYOUT_AUTO;
    } else {
        // Heuristic: prefer Nintendo layout for 8BitDo Zero or Nintendo devices
        if (strstr(g_joystick_name, "Nintendo") || strstr(g_joystick_name, "Zero")) {
            g_gamepad_layout = GP_LAYOUT_NINTENDO;
        } else {
            g_gamepad_layout = GP_LAYOUT_XBOX;
        }
    }
    LOG_INFO("Gamepad layout: %s", (g_gamepad_layout==GP_LAYOUT_NINTENDO?"nintendo":(g_gamepad_layout==GP_LAYOUT_XBOX?"xbox":"auto")));
    
    // Apply optional explicit ABXY mapping from environment (takes precedence for ABXY selection)
    setup_label_mapping();
    // Configure which buttons perform cycle and help based on layout/env
    configure_special_buttons();
    
    return true;
}

/**
 * Clean up joystick resources
 */
void cleanup_joystick(void) {
    if (g_joystick_fd >= 0) {
        close(g_joystick_fd);
        g_joystick_fd = -1;
    }
    g_joystick_enabled = false;
}

/**
 * Process a joystick event for keystone control
 * Maps controller buttons to keystone adjustment actions
 * 
 * @param event The joystick event to process
 * @return true if the event was handled and resulted in a keystone adjustment
 */
bool handle_joystick_event(struct js_event *event) {
    // Debounce to prevent too many events
    struct timeval now;
    gettimeofday(&now, NULL);
    long time_diff_ms = (now.tv_sec - g_last_js_event_time.tv_sec) * 1000 + 
                       (now.tv_usec - g_last_js_event_time.tv_usec) / 1000;
    
    // Require 100ms between events for buttons, 250ms for analog sticks
    int min_ms = (event->type == JS_EVENT_BUTTON) ? 100 : 250;
    if (time_diff_ms < min_ms) {
        return false;
    }
    
    // Track timestamp for debouncing
    g_last_js_event_time = now;
    
    // Skip initial state events sent when joystick is first opened
    if (event->type & JS_EVENT_INIT) {
        return false;
    }
    
    // Handle button events
    if (event->type == JS_EVENT_BUTTON) {
        // Track Start/Select state for quit combo
        if (event->number == JS_BUTTON_START) {
            if (event->value == 1) { g_js_start_down = true; gettimeofday(&g_js_start_time, NULL); }
            else if (event->value == 0) { g_js_start_down = false; }
        } else if (event->number == JS_BUTTON_SELECT) {
            if (event->value == 1) { g_js_select_down = true; gettimeofday(&g_js_select_time, NULL); }
            else if (event->value == 0) { g_js_select_down = false; }
        }

        // If keystone enabled and cycle button is pressed, optionally cycle corners TL->TR->BR->BL
        if (event->value == 1 && is_keystone_enabled() && g_x_cycle_enabled && event->number == g_cycle_button_code) {
            int order[4] = {0,1,3,2}; // TL,TR,BR,BL -> next
            int cur = *get_keystone_active_corner_ptr();
            if (cur < 0) cur = g_selected_corner >= 0 ? g_selected_corner : 0;
            int idx = 0;
            for (idx = 0; idx < 4; idx++) {
                if (order[idx] == cur) break;
            }
            g_selected_corner = order[(idx + 1) % 4];
            *get_keystone_active_corner_ptr() = g_selected_corner;
            LOG_INFO("Keystone corner: %d", g_selected_corner);
            return true;
        }
        
        // Handle ABXY buttons for corner selection if not using cycling
        if (event->value == 1 && is_keystone_enabled() && !g_x_cycle_enabled) {
            // Map ABXY buttons to corners directly based on label mapping
            if (g_use_label_mapping) {
                if (event->number == g_btn_code_X) { g_selected_corner = g_corner_for_X; *get_keystone_active_corner_ptr() = g_selected_corner; return true; }
                if (event->number == g_btn_code_A) { g_selected_corner = g_corner_for_A; *get_keystone_active_corner_ptr() = g_selected_corner; return true; }
                if (event->number == g_btn_code_B) { g_selected_corner = g_corner_for_B; *get_keystone_active_corner_ptr() = g_selected_corner; return true; }
                if (event->number == g_btn_code_Y) { g_selected_corner = g_corner_for_Y; *get_keystone_active_corner_ptr() = g_selected_corner; return true; }
            } else {
                // Xbox-style default mapping (X=TL, A=TR, B=BR, Y=BL)
                if (event->number == JS_BUTTON_X) { g_selected_corner = 0; *get_keystone_active_corner_ptr() = g_selected_corner; return true; }
                if (event->number == JS_BUTTON_A) { g_selected_corner = 1; *get_keystone_active_corner_ptr() = g_selected_corner; return true; }
                if (event->number == JS_BUTTON_B) { g_selected_corner = 3; *get_keystone_active_corner_ptr() = g_selected_corner; return true; }
                if (event->number == JS_BUTTON_Y) { g_selected_corner = 2; *get_keystone_active_corner_ptr() = g_selected_corner; return true; }
            }
        }
        
        // Toggle help display when help button is pressed
        if (event->value == 1 && event->number == g_help_button_code) {
            keystone_toggle_border();
            return true;
        }
        
        // Toggle keystone correction with L1/R1 simultaneously
        if (event->value == 1 && (event->number == JS_BUTTON_L1 || event->number == JS_BUTTON_R1)) {
            static struct timeval last_shoulder = {0};
            struct timeval shoulder_now;
            gettimeofday(&shoulder_now, NULL);
            long ms = (shoulder_now.tv_sec - last_shoulder.tv_sec) * 1000 + 
                     (shoulder_now.tv_usec - last_shoulder.tv_usec) / 1000;
            last_shoulder = shoulder_now;
            
            if (ms < 500) { // Both shoulders pressed within 500ms
                keystone_toggle_enabled();
                return true;
            }
        }
    }
    
    // Handle analog stick movements for corner adjustments
    if (event->type == JS_EVENT_AXIS && is_keystone_enabled() && g_selected_corner >= 0) {
        // Axis 0 = Left stick X, Axis 1 = Left stick Y
        // Values are -32767 to +32767
        
        // Only process significant movements (deadzone)
        if (abs(event->value) < 8000) {
            return false;
        }
        
        // Calculate adjustment amount (smaller for fine control)
        float adjust = (float)event->value / 32767.0f * 0.01f;
        
        if (event->number == 0) { // X-axis
            keystone_adjust_corner(g_selected_corner, adjust, 0);
            return true;
        } else if (event->number == 1) { // Y-axis
            keystone_adjust_corner(g_selected_corner, 0, adjust);
            return true;
        }
    }
    
    return false;
}

/**
 * Handle keyboard input for keystone control
 * 
 * @param key The key pressed
 * @return true if the key was handled for keystone control
 */
bool handle_keyboard_input(char key) {
    return keystone_handle_key(key);
}

/**
 * Check if joystick is enabled
 * 
 * @return true if joystick is enabled, false otherwise
 */
bool is_joystick_enabled(void) {
    return g_joystick_enabled;
}

/**
 * Get the name of the connected joystick
 * 
 * @return Joystick name or "Unknown" if not connected
 */
const char* get_joystick_name(void) {
    return g_joystick_enabled ? g_joystick_name : "Not connected";
}

/**
 * Get the current gamepad layout
 * 
 * @return Current gamepad layout
 */
gamepad_layout_t get_gamepad_layout(void) {
    return g_gamepad_layout;
}