#ifndef PICKLE_INPUT_H
#define PICKLE_INPUT_H

#include <stdbool.h>
#include <linux/joystick.h>

// Joystick button constants
#define JS_BUTTON_A        0
#define JS_BUTTON_B        1
#define JS_BUTTON_X        2
#define JS_BUTTON_Y        3
#define JS_BUTTON_L1       4
#define JS_BUTTON_R1       5
#define JS_BUTTON_SELECT   6
#define JS_BUTTON_START    7
#define JS_BUTTON_L3       8  // Left stick press
#define JS_BUTTON_R3       9  // Right stick press
#define JS_BUTTON_HOME    10  // Home/Guide button (not always present)

// Gamepad layout types
typedef enum {
    GP_LAYOUT_AUTO,
    GP_LAYOUT_XBOX,     // A=bottom, B=right, X=left, Y=top
    GP_LAYOUT_NINTENDO  // A=right, B=bottom, X=top, Y=left
} gamepad_layout_t;

// Input initialization and cleanup
bool init_joystick(void);
void cleanup_joystick(void);

// Event handling
bool handle_joystick_event(struct js_event *event);
bool handle_keyboard_input(char key);

// Joystick status and properties
bool is_joystick_enabled(void);
const char* get_joystick_name(void);
gamepad_layout_t get_gamepad_layout(void);

// Setup gamepad button configurations
void setup_label_mapping(void);
void configure_special_buttons(void);

#endif // PICKLE_INPUT_H