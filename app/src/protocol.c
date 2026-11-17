
/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <string.h>
#include <stdlib.h>

#include "protocol.h"
#include "utils.h"
#include "naveg.h"
#include "ledz.h"
#include "hardware.h"
#include "glcd.h"
#include "utils.h"
#include "screen.h"
#include "system.h"
#include "cli.h"
#include "calibration.h"
#include "mod-protocol.h"
#include "mode_builder.h"
#include "widget_area.h"

/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

#define NOT_FOUND           (-1)
#define MANY_ARGUMENTS      (-2)
#define FEW_ARGUMENTS       (-3)
#define INVALID_ARGUMENT    (-4)


/*
************************************************************************************************************************
*           LOCAL CONSTANTS
************************************************************************************************************************
*/

const char *g_error_messages[] = {
    RESP_ERR_COMMAND_NOT_FOUND,
    RESP_ERR_MANY_ARGUMENTS,
    RESP_ERR_FEW_ARGUMENTS,
    RESP_ERR_INVALID_ARGUMENT
};


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

typedef struct CMD_T {
    char* command;
    char** list;
    uint32_t count;
    void (*callback)(uint8_t serial_id, proto_t *proto);
} cmd_t;


/*
************************************************************************************************************************
*           LOCAL MACROS
************************************************************************************************************************
*/

#define MAP(x, Omin, Omax, Nmin, Nmax)      ( x - Omin ) * (Nmax -  Nmin)  / (Omax - Omin) + Nmin;
#define UNUSED_PARAM(var)   do { (void)(var); } while (0)

/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static unsigned int g_command_count = 0;
static cmd_t g_commands[COMMAND_COUNT_DUOX];

static int8_t *WIDGET_LED_COLORS[]  = {
#ifdef WIDGET_LED0_COLOR
    (int8_t []) WIDGET_LED0_COLOR,
#endif
#ifdef WIDGET_LED1_COLOR
    (int8_t []) WIDGET_LED1_COLOR,
#endif
#ifdef WIDGET_LED2_COLOR
    (int8_t []) WIDGET_LED2_COLOR,
#endif
#ifdef WIDGET_LED3_COLOR
    (int8_t []) WIDGET_LED3_COLOR,
#endif
#ifdef WIDGET_LED4_COLOR
    (int8_t []) WIDGET_LED4_COLOR,
#endif
#ifdef WIDGET_LED5_COLOR
    (int8_t []) WIDGET_LED5_COLOR,
#endif
#ifdef WIDGET_LED6_COLOR
    (int8_t []) WIDGET_LED6_COLOR,
#endif
#ifdef WIDGET_LED7_COLOR
    (int8_t []) WIDGET_LED7_COLOR,
#endif
};

/*
************************************************************************************************************************
*           LOCAL FUNCTION PROTOTYPES
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           LOCAL CONFIGURATION ERRORS
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS
************************************************************************************************************************
*/

static int is_wildcard(const char *str)
{
    if (str && strchr(str, '%') != NULL) return 1;
    return 0;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

// a decimal integer, with its sign, and the spaces after it; NULL when there is none
static const char *read_int(const char *p, int16_t *value)
{
    int32_t v = 0;
    uint8_t negative = 0, digits = 0;

    if (*p == '-')
    {
        negative = 1;
        p++;
    }

    for (; *p >= '0' && *p <= '9'; p++, digits++)
        v = v * 10 + (*p - '0');

    if (!digits) return NULL;

    *value = (int16_t)(negative ? -v : v);

    while (*p == ' ') p++;
    return p;
}

// sys_wbm and sys_wcl, the widget bitmaps and clears: hundreds a second, each with hundreds of hex digits.
// Read here in one pass, without what the generic path costs for every character of them: the split into
// a list, parse_quote on every argument, the search among the commands. 0 when the message is not one of
// them, or not well formed: the generic path takes it then.
static uint8_t parse_widget_command(msg_t *msg)
{
    const char *p = msg->data;
    uint8_t bitmap;

    if (msg->sender_id != SYSTEM_SERIAL) return 0;

    if (strncmp(p, "sys_wbm ", 8) == 0) bitmap = 1;
    else if (strncmp(p, "sys_wcl ", 8) == 0) bitmap = 0;
    else return 0;

    // the data length after the command
    p += 8;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;

    int16_t area_id, x, y, width, height;
    if (!(p = read_int(p, &area_id)) || !(p = read_int(p, &x)) || !(p = read_int(p, &y)) ||
        !(p = read_int(p, &width)) || !(p = read_int(p, &height)))
        return 0;

    if (bitmap && !*p) return 0;

    WA_draw(area_id, x, y, width, height, bitmap ? p : NULL);

    SEND_TO_SENDER(msg->sender_id, "r 0", 3);
    return 1;
}

void protocol_parse(msg_t *msg)
{
    uint32_t i, j;
    int32_t index = NOT_FOUND;
    proto_t proto;

    if (parse_widget_command(msg)) return;

    proto.list = strarr_split(msg->data, ' ');
    proto.list_count = strarr_length(proto.list);
    proto.response = NULL;

    // TODO: check invalid argumets (wildcards)

    if (proto.list_count == 0)
    {
        FREE(proto.list);
        return;
    }

    unsigned int match, variable_arguments = 0;

    // loop all registered commands
    for (i = 0; i < g_command_count; i++)
    {
        match = 0;
        index = NOT_FOUND;

        // checks received protocol
        for (j = 0; j < proto.list_count && j < g_commands[i].count; j++)
        {
            if (strcmp(g_commands[i].list[j], proto.list[j]) == 0)
            {
                match++;
            }
            else if (match > 0)
            {
                if (is_wildcard(g_commands[i].list[j])) match++;
                else if (strcmp(g_commands[i].list[j], "...") == 0)
                {
                    match++;
                    variable_arguments = 1;
                }
            }
        }

        if (match > 0)
        {
            // checks if the last argument is ...
            if (j < g_commands[i].count)
            {
                if (strcmp(g_commands[i].list[j], "...") == 0) variable_arguments = 1;
            }

            // few arguments
            if (proto.list_count < (g_commands[i].count - variable_arguments))
            {
                index = FEW_ARGUMENTS;
            }

            // many arguments
            else if (proto.list_count > g_commands[i].count && !variable_arguments)
            {
                index = MANY_ARGUMENTS;
            }

            // arguments match
            else if (match == proto.list_count || variable_arguments)
            {
                index = i;
            }

            break;
        }
    }

    // Protocol OK
    if (index >= 0)
    {
        if (g_commands[index].callback)
        {
           g_commands[index].callback(msg->sender_id, &proto);

            if (proto.response)
            {
                SEND_TO_SENDER(msg->sender_id, proto.response, proto.response_size);

                // The free is not more necessary because response is not more allocated dynamically
                // uncomment bellow line if this change in the future
                // FREE(proto.response);
            }
        }
    }
    // Protocol error
    else
    {
        SEND_TO_SENDER(msg->sender_id, g_error_messages[-index-1], strlen(g_error_messages[-index-1]));
    }

    FREE(proto.list);
}


void protocol_add_command(const char *command, void (*callback)(uint8_t serial_id, proto_t *proto))
{
    if (g_command_count >= COMMAND_COUNT_DUOX) while (1);

    char *cmd = str_duplicate(command);
    g_commands[g_command_count].command = cmd;
    g_commands[g_command_count].list = strarr_split(cmd, ' ');
    g_commands[g_command_count].count = strarr_length(g_commands[g_command_count].list);
    g_commands[g_command_count].callback = callback;
    g_command_count++;
}


void protocol_response(const char *response, proto_t *proto)
{
    static char response_buffer[32];

    proto->response = response_buffer;

    proto->response_size = strlen(response);
    if (proto->response_size >= sizeof(response_buffer))
        proto->response_size = sizeof(response_buffer) - 1;

    strncpy(response_buffer, response, sizeof(response_buffer)-1);
    response_buffer[proto->response_size] = 0;
}

void protocol_send_response(const char *response, const uint8_t value ,proto_t *proto)
{
    char buffer[20];
    uint8_t i = 0;
    memset(buffer, 0, sizeof buffer);

    i = copy_command(buffer, response); 

    // insert the value on buffer
    i += int_to_str(value, &buffer[i], sizeof(buffer) - i, 0);

    protocol_response(&buffer[0], proto);
}

void protocol_remove_commands(void)
{
    unsigned int i;

    for (i = 0; i < g_command_count; i++)
    {
        FREE(g_commands[i].command);
        FREE(g_commands[i].list);
    }
}

//initialize all protocol commands
void protocol_init(void)
{
    // protocol definitions
    protocol_add_command(CMD_PING, cb_ping);
    protocol_add_command(CMD_SAY, cb_say);
    protocol_add_command(CMD_LED, cb_led);
    protocol_add_command(CMD_GLCD_TEXT, cb_glcd_text);
    protocol_add_command(CMD_GLCD_DIALOG, cb_glcd_dialog);
    protocol_add_command(CMD_GLCD_DRAW, cb_glcd_draw);
    protocol_add_command(CMD_WIDGET_AREA_ADD, cb_widget_area_add);
    protocol_add_command(CMD_WIDGET_AREA_REMOVE, cb_widget_area_remove);
    protocol_add_command(CMD_DISP_BRIGHTNESS, cb_disp_brightness);
    protocol_add_command(CMD_GUI_CONNECTED, cb_gui_connection);
    protocol_add_command(CMD_GUI_DISCONNECTED, cb_gui_connection);
    protocol_add_command(CMD_CONTROL_ADD, cb_control_add);
    protocol_add_command(CMD_CONTROL_REMOVE, cb_control_rm);
    protocol_add_command(CMD_CONTROL_SET, cb_control_set);
    protocol_add_command(CMD_CONTROL_GET, cb_control_get);
    protocol_add_command(CMD_INITIAL_STATE, cb_initial_state);
    protocol_add_command(CMD_TUNER, cb_tuner);
    protocol_add_command(CMD_RESPONSE, cb_resp);
    protocol_add_command(CMD_RESTORE, cb_restore);
    protocol_add_command(CMD_DUO_BOOT, cb_boot);
    protocol_add_command(CMD_MENU_ITEM_CHANGE, cb_menu_item_changed);
    protocol_add_command(CMD_PEDALBOARD_CLEAR, cb_pedalboard_clear);
    protocol_add_command(CMD_PEDALBOARD_LOADING_BEGIN, cb_pedalboard_load_begin);
    protocol_add_command(CMD_PEDALBOARD_LOADING_END, cb_pedalboard_load_end);
    protocol_add_command(CMD_BUILDER_PLUGIN_UPDATED, cb_plugin_map_updated);
    protocol_add_command(CMD_BUILDER_CONTROL_SET, cb_builder_control_set);
    protocol_add_command(CMD_PEDALBOARD_NAME_SET, cb_pedalboard_name);
    protocol_add_command(CMD_DUOX_PAGES_AVAILABLE, cb_pages_available);
    protocol_add_command(CMD_SELFTEST_SAVE_POT_CALIBRATION, cb_save_pot_cal_val);
    protocol_add_command(CMD_SELFTEST_SKIP_CONTROL_ENABLE, cb_set_selftest_control_skip);
    protocol_add_command(CMD_SELFTEST_CHECK_CALIBRATION, cb_check_cal);
    protocol_add_command(CMD_RESET_EEPROM, cb_clear_eeprom);
    protocol_add_command(CMD_DUOX_SET_CONTRAST, cb_set_disp_contrast);
    protocol_add_command(CMD_DUOX_EXP_OVERCURRENT, cb_exp_overcurrent);
    protocol_add_command(CMD_SYS_LAUNCH_POPUP, cb_launch_popup);
    protocol_add_command(CMD_SYS_CHANGE_LED_BLINK, cb_change_assigned_led_blink);
    protocol_add_command(CMD_SYS_CHANGE_LED_BRIGHTNESS, cb_change_assigned_led_brightness);
    protocol_add_command(CMD_SYS_CHANGE_NAME, cb_change_assigment_name);
    protocol_add_command(CMD_SYS_CHANGE_UNIT, cb_change_assigment_unit);
    protocol_add_command(CMD_SYS_CHANGE_VALUE, cb_change_assigment_value);
    protocol_add_command(CMD_SYS_CHANGE_WIDGET_INDICATOR, cb_change_widget_indicator);
    protocol_add_command(CMD_SYS_WIDGET_BITMAP, cb_widget_bitmap);
    protocol_add_command(CMD_SYS_WIDGET_CLEAR, cb_widget_clear);
    protocol_add_command(CMD_PEDALBOARD_CHANGE, cb_pedalboard_change);
    protocol_add_command(CMD_SCREENSHOT, cb_screenshot);
}

volatile uint8_t g_screenshot = 0;

/*
************************************************************************************************************************
*           PROTOCOL CALLBACKS
************************************************************************************************************************
*/

void cb_ping(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    g_ui_communication_started = 1;
    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_say(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    protocol_response(proto->list[1], proto);
}

void cb_led(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    ledz_t *led = hardware_leds(atoi(proto->list[1]));

    int8_t value[3] = {atoi(proto->list[2]), atoi(proto->list[3]), atoi(proto->list[4])};
    ledz_set_color(CMD_COLOR_ID, value);

    led->led_state.color = CMD_COLOR_ID;

    ledz_off(led, WHITE);

    if (proto->list_count < 6)
        ledz_set_state(led, LED_ON, LED_UPDATE);
    else
    {
        led->led_state.time_on = atoi(proto->list[5]);
        led->led_state.time_off = atoi(proto->list[6]);
        led->led_state.amount_of_blinks = LED_BLINK_INFINIT;
        ledz_set_state(led, LED_BLINK, LED_UPDATE);
    }

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_glcd_text(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    uint8_t glcd_id, x, y;
    glcd_id = atoi(proto->list[1]);
    x = atoi(proto->list[2]);
    y = atoi(proto->list[3]);

    if (glcd_id >= GLCD_COUNT) return;

    screen_text_box(glcd_id, x, y, proto->list[4]);
    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_glcd_dialog(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    uint8_t val = naveg_dialog(proto->list[1], "selftest");
    protocol_send_response(CMD_RESPONSE, val, proto);
}

void cb_glcd_draw(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    uint8_t glcd_id, x, y;
    glcd_id = atoi(proto->list[1]);
    x = atoi(proto->list[2]);
    y = atoi(proto->list[3]);

    if (glcd_id >= GLCD_COUNT) return;

    protocol_send_response(CMD_RESPONSE, 0, proto);

    uint8_t msg_buffer[WEBGUI_COMM_RX_BUFF_SIZE];

    str_to_hex(proto->list[4], msg_buffer, sizeof(msg_buffer));
    glcd_draw_image(hardware_glcds(glcd_id), x, y, msg_buffer, GLCD_BLACK);
}

// mod-ui places the areas on both displays side by side, x 0 to 255
void cb_widget_area_add(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    const glcd_rect_t rect = {
        atoi(proto->list[4]), atoi(proto->list[5]), atoi(proto->list[6]), atoi(proto->list[7])
    };
    uint8_t displays;

    if (!WA_add(atoi(proto->list[3]), atoi(proto->list[1]), atoi(proto->list[2]), &rect, &displays))
    {
        protocol_send_response(CMD_RESPONSE, INVALID_ARGUMENT, proto);
        return;
    }

    naveg_print_control_screens(displays);

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_widget_area_remove(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    naveg_print_control_screens(WA_remove(atoi(proto->list[1])));

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

// bitmaps for an area that is not in view are dropped without complaint: the plugin may still be drawing
// the page the user just left, and it redraws everything when the area is in view again
void cb_widget_bitmap(uint8_t serial_id, proto_t *proto)
{
    if (serial_id != SYSTEM_SERIAL)
        return;

    WA_draw(atoi(proto->list[2]), atoi(proto->list[3]), atoi(proto->list[4]),
            atoi(proto->list[5]), atoi(proto->list[6]), proto->list[7]);

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_widget_clear(uint8_t serial_id, proto_t *proto)
{
    if (serial_id != SYSTEM_SERIAL)
        return;

    WA_draw(atoi(proto->list[2]), atoi(proto->list[3]), atoi(proto->list[4]),
            atoi(proto->list[5]), atoi(proto->list[6]), NULL);

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_gui_connection(uint8_t serial_id, proto_t *proto)
{    
    UNUSED_PARAM(serial_id);

    //clear the buffer so we dont send any messages
    ui_comm_webgui_clear_tx_buffer();

    if (strcmp(proto->list[0], CMD_GUI_CONNECTED) == 0)
        naveg_ui_connection(UI_CONNECTED);
    else
        naveg_ui_connection(UI_DISCONNECTED);

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_disp_brightness(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    hardware_glcd_brightness(atoi(proto->list[1]));

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_control_add(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    control_t *control = data_parse_control(proto->list);

    // a page of parameters the builder asked for comes with the hw_ids of the real actuators
    if (!BM_add_control(control))
        naveg_add_control(control, 1);

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_control_rm(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    g_ui_communication_started = 1;

    naveg_remove_control(atoi(proto->list[1]));

    uint8_t i;
    for (i = 2; i < TOTAL_ACTUATORS + 1; i++)
    {
        if ((proto->list[i]) != NULL)
        {
            naveg_remove_control(atoi(proto->list[i]));
        }
        else break;
    }

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_control_set(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    naveg_set_control(atoi(proto->list[1]), atof(proto->list[2]));
    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_control_get(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    float value;
    value = naveg_get_control_value(atoi(proto->list[1]));

    protocol_send_response(CMD_RESPONSE, value, proto);
}

void cb_initial_state(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    g_ui_communication_started = 1;
    naveg_initial_state(atoi(proto->list[1]), atoi(proto->list[2]), atoi(proto->list[3]), proto->list[4], proto->list[5], &(proto->list[6]));

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_tuner(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    screen_tuner(atof(proto->list[1]), proto->list[2], atoi(proto->list[3]));
    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_resp(uint8_t serial_id, proto_t *proto)
{
    if (serial_id == SYSTEM_SERIAL)
        sys_comm_response_cb(proto->list);
    else
        ui_comm_webgui_response_cb(proto->list);
}

void cb_restore(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    //clear all screens 
    screen_clear(DISPLAY_LEFT);
    screen_clear(DISPLAY_RIGHT);

    cli_restore(RESTORE_INIT);
    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_boot(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    g_self_test_mode = false;
    g_self_test_cancel_button = false;

    // the host has come back up and knows nothing of a builder it was showing
    BM_drop(1);

    //set the quick bypass link
    system_update_menu_value(MENU_ID_QUICK_BYPASS, atoi(proto->list[2]));

    //set the tuner mute state 
    system_update_menu_value(MENU_ID_TUNER_MUTE, atoi(proto->list[3]));

    // the profile and the master volume link are optional: only as many as the message has
    //set the current user profile
    if (proto->list_count > 4)
        system_update_menu_value(PROFILES_ID, atoi(proto->list[4]));

    //set the master volume link
    if (proto->list_count > 5)
        system_update_menu_value(MENU_ID_MASTER_VOL_PORT, atoi(proto->list[5]));

    naveg_turn_on_pagination_leds();

    g_device_booted = true;

    naveg_ui_booted();

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_menu_item_changed(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    naveg_menu_item_changed_cb(atoi(proto->list[1]), atoi(proto->list[2]));

    // optional more "id value" pairs: only as many as the message has
    uint8_t i;
    for (i = 3; i + 1u < proto->list_count && i < ((MENU_ID_TOP+1) * 2); i+=2)
    {
        if (atoi(proto->list[i]) != 0)
        {
            naveg_menu_item_changed_cb(atoi(proto->list[i]), atoi(proto->list[i+1]));
        }
        else break;
    }

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_pedalboard_clear(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    //clear controls
    uint8_t i;
    for (i = 0; i < TOTAL_ACTUATORS; i++)
    {
        naveg_remove_control(i);
    }

    //clear snapshots
    //we dont care yet about which snapshot, thats why hardcoded
    naveg_clear_snapshot(6);
    naveg_clear_snapshot(5);
    naveg_clear_snapshot(4);

    //reset the page to page 1
    naveg_reset_page();

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

/*
 * The two edges of a pedalboard load, whoever asked for it. Everything between them is a
 * board in pieces, so the builder, which is a view onto the board, is left rather than
 * redrawn around a graph that no longer holds.
 */
void cb_pedalboard_load_begin(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    protocol_send_response(CMD_RESPONSE, 0, proto);

    // the host stops watching the board on its own when a load starts, so this leaves
    // without saying anything: nothing may be sent from inside a callback
    BM_drop(1);
}

void cb_pedalboard_load_end(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    protocol_send_response(CMD_RESPONSE, 0, proto);

    // a NEW PEDALBOARD from the menu goes on into the builder, now that it is loaded
    BM_pedalboard_loaded();

    // WiFi icon of the title: read by the displays task, the cli is not used from here
    system_wifi_request_check();
}

/*
 * The board changed under the builder -- edited from the web UI, most of the time. Only a
 * flag here: fetching the graph is a command of its own, and a callback cannot send one.
 */
void cb_plugin_map_updated(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    protocol_send_response(CMD_RESPONSE, 0, proto);

    BM_plugin_map_stale();
}

// a value of the builder's parameter page changed on the host, by a preset load most of the time
void cb_builder_control_set(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    BM_set_control(atoi(proto->list[1]), atof(proto->list[2]));

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_pedalboard_name(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

	screen_top_info(&proto->list[1] , 1);

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_pedalboard_change(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    naveg_set_active_pedalboard(atoi(proto->list[1]));

    protocol_send_response(CMD_RESPONSE, 0, proto);

    naveg_set_pb_list_update();
}

// a callback cannot send commands of its own: the display task sends the 8 pages once this is answered
void cb_screenshot(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    protocol_send_response(CMD_RESPONSE, 0, proto);

    // one based on the wire, 1 the left display, 2 the right one: 0 is nothing to do
    uint8_t display = atoi(proto->list[1]);
    if (display <= GLCD_COUNT) g_screenshot = display;
}

void cb_pages_available(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

	naveg_pages_available(atoi(proto->list[1]), atoi(proto->list[2]), atoi(proto->list[3]), atoi(proto->list[4]), atoi(proto->list[5]), atoi(proto->list[6]));

    naveg_turn_on_pagination_leds();

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_save_pot_cal_val(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    ui_comm_webgui_clear_tx_buffer();

    //if the first argument == 1, we save the max value, if ==0 we save the min value
    if(atoi(proto->list[1]) == 1)
    {
        calibration_write_max(atoi(proto->list[2]));
    } 
    else 
    {
        calibration_write_min(atoi(proto->list[2]));
    }

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_check_cal(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    ui_comm_webgui_clear_tx_buffer();

    if (calibration_check_valid_pot(atoi(proto->list[1])))
    {
        protocol_send_response(CMD_RESPONSE, 1, proto);
    }
    //range not good
    else
    {
        protocol_send_response(CMD_RESPONSE, 0, proto);
    }
}

void cb_set_selftest_control_skip(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    ui_comm_webgui_clear_tx_buffer();

    g_self_test_cancel_button = true; 

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_clear_eeprom(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    ui_comm_webgui_clear_tx_buffer();

    hardware_reset_eeprom();    

    //!! THIS NEEDS AN HMI RESET TO TAKE PROPER EFFECT

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_set_disp_contrast(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(serial_id);

    ui_comm_webgui_clear_tx_buffer();

    int value_bfr = MAP(atoi(proto->list[2]), 0, 100, UC1701_PM_MIN, UC1701_PM_MAX);
    uc1701_set_custom_value(hardware_glcds(atoi(proto->list[1])), value_bfr, UC1701_RR_DEFAULT);

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_exp_overcurrent(uint8_t serial_id, proto_t *proto)
{
    UNUSED_PARAM(proto);
    UNUSED_PARAM(serial_id);

    naveg_dialog("Exp port shorted\nTo avoid harm to your device\nthe unit switched back to CV\nmode\n\nPlease check your exp pedal\nand cables", "WARNING");

    naveg_reload_display();
}

//HMI widgets
void cb_change_assigned_led_blink(uint8_t serial_id, proto_t *proto)
{
    if (serial_id != SYSTEM_SERIAL)
        return;

    uint8_t hw_id = atoi(proto->list[2]);

    int16_t argument_1 = atoi(proto->list[4]);
    int16_t argument_2 = atoi(proto->list[5]);

    ledz_t *led;
    if (naveg_get_actuator_type(hw_id) == ACT_FOOTSWITCH) {
        led = hardware_leds(hw_id - ENCODERS_COUNT);
    }
    else {
        protocol_send_response(CMD_RESPONSE, INVALID_ARGUMENT, proto);
        return;
    }

    control_t *control = naveg_get_control(hw_id);

    //error no assignment
    if (!control)
    {
        protocol_send_response(CMD_RESPONSE, INVALID_ARGUMENT, proto);
        return;
    }

    //set color
    ledz_set_color(MAX_COLOR_ID + hw_id-ENCODERS_COUNT +1, WIDGET_LED_COLORS[atoi(proto->list[3])]);

    led->led_state.color =  MAX_COLOR_ID + hw_id-ENCODERS_COUNT+1;

    control->lock_led_actions = 1;

    uint8_t led_update = 0;
    //check if we are in the menu
    if (!naveg_is_tool_mode(0) || !naveg_is_tool_mode(1))
        led_update = LED_UPDATE;

    if (argument_1 == 0) {
        ledz_set_state(led, LED_ON, led_update);
    }
    else if (argument_1 < 0)
    {
        led->sync_blink = abs(argument_1);
        led->led_state.sync_blink = led->sync_blink;
        ledz_set_state(led, LED_BLINK, led_update);
    }
    else
    {
        led->led_state.amount_of_blinks = LED_BLINK_INFINIT;
        led->led_state.time_on = argument_1;
        led->led_state.time_off = argument_2;
        led->led_state.sync_blink = 0;
        led->sync_blink = 0;
        ledz_set_state(led, LED_BLINK, led_update);
    }

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_change_assigned_led_brightness(uint8_t serial_id, proto_t *proto)
{
    if (serial_id != SYSTEM_SERIAL)
        return;

    uint8_t hw_id = atoi(proto->list[2]);

    int16_t argument = atoi(proto->list[4]);

    ledz_t *led;
    if (naveg_get_actuator_type(hw_id) == ACT_FOOTSWITCH) {
        led = hardware_leds(hw_id - ENCODERS_COUNT);
    }
    else {
        protocol_send_response(CMD_RESPONSE, INVALID_ARGUMENT, proto);
        return;
    }

    control_t *control = naveg_get_control(hw_id);

    //error no assignment
    if (!control)
    {
        protocol_send_response(CMD_RESPONSE, INVALID_ARGUMENT, proto);
        return;
    }

    //set color
    ledz_set_color(MAX_COLOR_ID + hw_id-ENCODERS_COUNT +1, WIDGET_LED_COLORS[atoi(proto->list[3])]);

    led->led_state.color =  MAX_COLOR_ID + hw_id-ENCODERS_COUNT+1;

    control->lock_led_actions = 1;

    uint8_t led_update = 1;

    if (argument <= 0)
    {
        //set brightnesses
        //TODO USE ENUM LIST
        switch (argument) {
            //full
            case 0:
                ledz_set_state(led, LED_OFF, led_update);
            break;

            //30%
            case -1:
                led->led_state.brightness = 0.3f;
                ledz_set_state(led, LED_DIMMED, led_update);
            break;

            //60%
            case -2:
                led->led_state.brightness = 0.6f;
                ledz_set_state(led, LED_DIMMED, led_update);
            break;

            //on
            case -3:
                ledz_set_state(led, LED_ON, led_update);
            break;
        }
    }
    //brightness control
    else
    {
        led->led_state.brightness = (float)(argument / 100.0f);
        ledz_set_state(led, LED_DIMMED, led_update);
    }

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_change_assigment_name(uint8_t serial_id, proto_t *proto)
{
    if (serial_id != SYSTEM_SERIAL)
        return;

    uint8_t hw_id = atoi(proto->list[2]);

    //error, no valid actuator
    if (hw_id > ENCODERS_COUNT + FOOTSWITCHES_ACTUATOR_COUNT + POTS_COUNT)
    {
        protocol_send_response(CMD_RESPONSE, INVALID_ARGUMENT, proto);
        return;
    }

    control_t *control = naveg_get_control(hw_id);

    //error no assignment
    if (!control)
    {
        protocol_send_response(CMD_RESPONSE, INVALID_ARGUMENT, proto);
        return;
    }

    FREE(control->label);
    control->label = str_duplicate(proto->list[3]);

    if (!naveg_is_tool_mode(0) || !naveg_is_tool_mode(1))
    {
        if (naveg_get_actuator_type(hw_id) == ACT_ENCODER)
            screen_encoder(hw_id, control);
        else if (naveg_get_actuator_type(hw_id) == ACT_POT)
            screen_pot(hw_id - ENCODERS_COUNT - FOOTSWITCHES_ACTUATOR_COUNT, control);
        else
            naveg_draw_foot(control);
    }

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_change_assigment_value(uint8_t serial_id, proto_t *proto)
{
    if (serial_id != SYSTEM_SERIAL)
        return;

    uint8_t hw_id = atoi(proto->list[2]);

    //error, we dont change value of foots
    if ((hw_id > ENCODERS_COUNT) && (hw_id < ENCODERS_COUNT + FOOTSWITCHES_ACTUATOR_COUNT))
    {
        protocol_send_response(CMD_RESPONSE, INVALID_ARGUMENT, proto);
        return;
    }

    control_t *control = naveg_get_control(hw_id);

    //error no assignment
    if (!control)
    {
        protocol_send_response(CMD_RESPONSE, INVALID_ARGUMENT, proto);
        return;
    }

    if (control->value_string)
        FREE(control->value_string);

    control->value_string = str_duplicate(proto->list[3]);

    if (!naveg_is_tool_mode(0) || !naveg_is_tool_mode(1))
    {
        if (naveg_get_actuator_type(hw_id) == ACT_ENCODER)
            screen_encoder(hw_id, control);
        else if (naveg_get_actuator_type(hw_id) == ACT_POT)
            screen_pot(hw_id - ENCODERS_COUNT - FOOTSWITCHES_ACTUATOR_COUNT, control);
    }

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_change_widget_indicator(uint8_t serial_id, proto_t *proto)
{
    if (serial_id != SYSTEM_SERIAL)
        return;

    uint8_t hw_id = atoi(proto->list[2]);

    //error, we dont have an indicator on foots and pots
    if (hw_id > ENCODERS_COUNT)
    {
        protocol_send_response(CMD_RESPONSE, INVALID_ARGUMENT, proto);
        return;
    }

    control_t *control = naveg_get_control(hw_id);

    //error no assignment
    if (!control)
    {
        protocol_send_response(CMD_RESPONSE, INVALID_ARGUMENT, proto);
        return;
    }

    control->screen_indicator_widget_val = atof(proto->list[3]);

    if (!naveg_is_tool_mode(0) || !naveg_is_tool_mode(1))
    {
        screen_encoder(hw_id, control);
    }

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_change_assigment_unit(uint8_t serial_id, proto_t *proto)
{
    if (serial_id != SYSTEM_SERIAL)
        return;

    uint8_t hw_id = atoi(proto->list[2]);

    //error, we dont change units of foots
    if ((hw_id > ENCODERS_COUNT) && (hw_id < ENCODERS_COUNT + FOOTSWITCHES_ACTUATOR_COUNT))
    {
        protocol_send_response(CMD_RESPONSE, INVALID_ARGUMENT, proto);
        return;
    }

    control_t *control = naveg_get_control(hw_id);

    //error no assignment
    if (!control)
    {
        protocol_send_response(CMD_RESPONSE, INVALID_ARGUMENT, proto);
        return;
    }

    FREE(control->unit);
    control->unit = str_duplicate(proto->list[3]);

    if (!naveg_is_tool_mode(0) || !naveg_is_tool_mode(1))
    {
        if (naveg_get_actuator_type(hw_id) == ACT_ENCODER)
            screen_encoder(hw_id, control);
        else if (naveg_get_actuator_type(hw_id) == ACT_POT)
            screen_pot(hw_id - ENCODERS_COUNT - FOOTSWITCHES_ACTUATOR_COUNT, control);
    }

    protocol_send_response(CMD_RESPONSE, 0, proto);
}

void cb_launch_popup(uint8_t serial_id, proto_t *proto)
{
    if (serial_id != SYSTEM_SERIAL)
        return;

    uint8_t hw_id = atoi(proto->list[2]);

    control_t *control = naveg_get_control(hw_id);

    //error no assignment
    if (!control)
    {
        protocol_send_response(CMD_RESPONSE, INVALID_ARGUMENT, proto);
        return;
    }

    uint8_t display;
    if ((hw_id == 0) || (hw_id == 2) || (hw_id == 3) || ((hw_id >= 6) && (hw_id <= 9)))
        display = DISPLAY_LEFT;
    else
        display = DISPLAY_RIGHT;

    if (!naveg_is_tool_mode(0) && !naveg_is_tool_mode(1))
    {
        if (hardware_get_overlay_counter() != 0)
            hardware_force_overlay_off(0);

        screen_widget_overlay(display, atoi(proto->list[3]), proto->list[4], proto->list[5]);

        hardware_set_overlay_timeout(TIME_WIDGET_OVERLAY, naveg_reload_display, 0);
    }

    protocol_send_response(CMD_RESPONSE, 0, proto);
}