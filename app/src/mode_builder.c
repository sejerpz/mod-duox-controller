/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "config.h"
#include "utils.h"
#include "naveg.h"
#include "screen.h"
#include "ui_comm.h"
#include "mod-protocol.h"
#include "mode_builder.h"
#include "plugin_map.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

// The plugin_map viewport on the left display: inside the outlines, between the title bar and
// the button labels. Mirrors PLUGIN_MAP_HMI_VIEW_* in mod-ui's settings.py, which picks the
// window for this size; the Dwarf builder screen has the same one, so mod-ui needs no Duo X case.
#define PLUGIN_MAP_VIEW_X 2
#define PLUGIN_MAP_VIEW_Y 9
#define PLUGIN_MAP_VIEW_W 124
#define PLUGIN_MAP_VIEW_H 43

// The list is drawn by widget_listbox(), which keeps the cursor in the middle of the rows on
// screen, so the rows below it are needed as much as the ones above: a new window is asked
// for before fewer than this many are left either way.
#define PLUGIN_LIST_ROWS_AROUND 4

// The graph's selection follows the list cursor once the encoder has been still this long, in
// ms, when the box is outside the window of the graph in hand: one fetch per step would make
// the list crawl.
#define PLUGIN_LIST_SYNC_DELAY 250

// The parameters of the box under the cursor are fetched once the cursor has been still this
// long, in ms: a page is a command and up to BM_PARAM_ROWS control_adds, too much for every
// step of the encoder.
#define PARAMS_LOAD_DELAY 250

// Steps of a continuous parameter. mod-ui takes the count from the hardware descriptor for
// /hmi/knob<hw_id + 1>, and the Duo X has two of them: the rows below the second would turn
// in a handful of steps. 201 is what the descriptor gives the two that exist.
#define PARAM_VALUE_STEPS 201

// how many options mod-ui sends of an enumeration at once, around the current one
#define PARAM_OPTIONS_WINDOW 5

// how long the control_adds of a page are waited for after the answer, in ms
#define PARAMS_RECEIVE_TIMEOUT 2000


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

/*
 * The pedalboard graph, as mod-ui described it. Not a bitmap: the server sends a display
 * list of boxes, port stubs and cables in scene coordinates and plugin_map.c draws it with
 * the GLCD primitives, because a pannable bitmap would be ~65kB on a part with 96kB of
 * SRAM. The same records are the hit map: the rect of a box is both what gets drawn and
 * what gets selected.
 */
static plugin_map_t g_plugin_map;
static uint8_t g_plugin_map_loaded = 0;

/*
 * The list view: every plugin on the board by name, which mod-ui sends a window at a time.
 * The left display shows it in place of the graph when the board is too big to find a box
 * in the picture. Not taken from the graph's window, whose boxes are the ones around the
 * selection -- their alphabetical neighbours are anywhere on the board. The hardware boxes
 * have nothing to open, bypass or delete, so mod-ui leaves them out.
 */
static plugin_list_t g_plugin_list;
/* the left display shows the list rather than the graph; kept across visits */
static uint8_t g_list_view;
/* where the box a list request was centred on sits in the whole list, or BM_NONE */
static int16_t g_list_position = BM_NONE;

/* the list cursor moved to a box outside the graph's window; BM_tick() fetches it */
static uint8_t g_list_sync_pending;
static portTickType g_list_sync_stamp;

/*
 * The parameters on the right display, of the plugin with the wire id g_params_id: a page of
 * them at a time, as controls the host sends with control_add. g_params_id is BM_NONE when
 * there are none in hand, and then the right display shows the box itself.
 */
static plugin_params_t g_params;
static int16_t g_params_id = BM_NONE;
static char g_params_uid[12];
static char g_params_title[PLUGIN_MAP_TITLE_SIZE];

/*
 * How many control_adds of the page asked for are still to come. They follow the answer
 * rather than precede it: mod-ui writes the answer to a request straight away and queues its
 * own commands until the request has been handled. So they are taken as ours until the page
 * is full, or PARAMS_RECEIVE_TIMEOUT after the request should some never arrive.
 */
static volatile uint8_t g_receiving_controls;
static portTickType g_receiving_stamp;

/* the cursor moved onto another plugin; BM_tick() fetches its parameters */
static uint8_t g_params_pending;
static portTickType g_params_stamp;

/* the host changed a value on the page; BM_tick() redraws it */
static volatile uint8_t g_params_dirty;

static volatile uint8_t g_active;

/*
 * The builder runs from two tasks: the actuators task for the encoders and buttons, and the
 * displays task for BM_tick(). Both fetch the graph and draw it, so they take turns. Never
 * taken from a protocol callback: the protocol task is the one that delivers the response a
 * holder may be waiting for.
 */
static SemaphoreHandle_t module_mutex = NULL;

/*
 * Set from the protocol callback when the host says the board has changed. Acted on in
 * BM_tick() rather than there: fetching the graph is a command, and a callback of a
 * received one cannot send commands.
 */
static volatile uint8_t g_plugin_map_stale;

/*
 * Whether the host is watching the board for us.
 *
 * Kept because the builder must not send anything on the paths that leave it from inside a
 * protocol callback -- a pedalboard load, the host coming back up, the web UI taking over.
 * protocol_parse() writes the reply only after the callback returns, so a command sent from
 * one and waited on is a deadlock. On those paths the watch is dropped without a word: the
 * host has either stopped it itself, or never knew about it.
 */
static uint8_t g_watching;

// a BM_drop() that left the menu to be redrawn, by BM_tick()
static volatile uint8_t g_restore_menu;

/*
 * A BM_drop() while the host was watching. On a load or a host restart the watch is gone
 * already, but the web UI taking the panel over stops nothing on the server, so BM_tick()
 * says it once it may send again. Harmless when the host had already stopped.
 */
static volatile uint8_t g_unwatch;


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS
************************************************************************************************************************
*/

static void parse_plugin_map(void *data, menu_item_t *item)
{
    (void) item;
    char **list = data;
    uint32_t i;

    g_plugin_map_loaded = 0;

    //error, dont parse when mod-ui gives error
    if (!list || !list[0] || !list[1] || atoi(list[1]) == -1)
        return;

    if (!list[2]) return;

    /*
     * The display list is one message with spaces inside it, and protocol.c has already
     * split the whole thing into tokens. strarr_split() writes the separators over in
     * place, so the tokens are still contiguous in the rx buffer: putting the spaces back
     * rebuilds the original text without a second 3.5kB buffer, which this device does not
     * have to spare. Labels are sanitised server side, so no token can contain a space or
     * a quotation mark that would have made the split lossy.
     */
    for (i = 2; list[i + 1] != NULL; i++)
        list[i][strlen(list[i])] = ' ';

    g_plugin_map_loaded = plugin_map_parse(&g_plugin_map, list[2]);

    if (g_plugin_map_loaded)
    {
        /* mod-ui centred the window on the node we asked for, so carry the selection over */
        int8_t focus = plugin_map_index_of(&g_plugin_map, g_plugin_map.focus_id);

        if (focus == BM_NONE)
        {
            g_plugin_map.selected = BM_NONE;
            if (g_plugin_map.n_nodes > 0)
                plugin_map_select(&g_plugin_map, 0);
        }
        else
        {
            plugin_map_select(&g_plugin_map, focus);
        }
    }
}

/*
 * ask for the window of the pedalboard graph around <focus_id>
 */
static void request_plugin_map(int16_t focus_id, uint8_t initial)
{
    uint8_t i;
    char buffer[32];
    memset(buffer, 0, sizeof buffer);

    // sets the response callback
    ui_comm_webgui_set_response_cb(parse_plugin_map, NULL);
    //clear the buffer
    ui_comm_webgui_clear_tx_buffer();

    // send command plugin_map
    // response: "r 1 M 392 124 7 amc 3 12 40 ; N 3 p 12 20 30 12 0 0 0 REVERB ; ..."
    i = copy_command((char *)buffer, CMD_BUILDER_PLUGIN_MAP);

    uint8_t bitmask = 0;
    if (initial)
        bitmask |= FLAG_PAGINATION_INITIAL_REQ;

    // insert the flags on buffer
    i += int_to_str(bitmask, &buffer[i], sizeof(buffer) - i, 0);

    // inserts one space
    buffer[i++] = ' ';

    // insert the node to centre on; ignored by mod-ui on an initial request
    i += int_to_str(focus_id, &buffer[i], sizeof(buffer) - i, 0);

    buffer[i++] = 0;

    // sends the data to GUI
    ui_comm_webgui_send(buffer, i);

    // waits the display list be received
    ui_comm_webgui_wait_response();
}

/*
 * The host watches the board only while we are looking at it: one command on the way in,
 * one on the way out, and no timer running on the server the rest of the time.
 */
static void request_change_notify(uint8_t on)
{
    uint8_t i;
    char buffer[24];
    memset(buffer, 0, sizeof buffer);

    ui_comm_webgui_set_response_cb(NULL, NULL);
    ui_comm_webgui_clear_tx_buffer();

    i = copy_command((char *)buffer, CMD_BUILDER_PLUGIN_NOTIFY);
    i += int_to_str(on, &buffer[i], sizeof(buffer) - i, 0);
    buffer[i++] = 0;

    ui_comm_webgui_send(buffer, i);
    ui_comm_webgui_wait_response();
}

static void plugin_map_move(uint8_t direction)
{
    int16_t target;
    int8_t index;

    if (!g_plugin_map_loaded || g_plugin_map.selected == BM_NONE) return;

    target = plugin_map_step(&g_plugin_map, g_plugin_map.selected, direction);

    // the two ends of the board; the walk does not wrap
    if (target == BM_NONE) return;

    index = plugin_map_index_of(&g_plugin_map, target);

    if (index == BM_NONE)
    {
        /*
         * The next box is outside the window mod-ui sent, which is how a board too big
         * for one message is crossed: ask for the window centred on it and carry on. The
         * walk order is built over the whole board, so this always names a real box.
         */
        request_plugin_map(target, 0);

        if (!g_plugin_map_loaded) return;

        index = plugin_map_index_of(&g_plugin_map, target);
        if (index == BM_NONE) return;
    }

    plugin_map_select(&g_plugin_map, index);
}

static void parse_plugin_list(void *data, menu_item_t *item)
{
    (void) item;
    char **list = data;
    uint32_t i, count;

    g_plugin_list.count = 0;
    g_plugin_list.loaded = 0;
    g_list_position = BM_NONE;

    if (!list || !list[0] || !list[1] || atoi(list[1]) == -1) return;
    if (!list[2] || !list[3] || !list[4] || !list[5]) return;

    g_plugin_list.total = (uint16_t) atoi(list[2]);
    g_plugin_list.first = (uint16_t) atoi(list[3]);
    g_list_position = (int16_t) atoi(list[4]);
    count = (uint32_t) atoi(list[5]);

    for (i = 0; i < count && g_plugin_list.count < PLUGIN_LIST_MAX_ROWS; i++)
    {
        char **entry = &list[6 + i * 3];
        plugin_list_row_t *row = &g_plugin_list.rows[g_plugin_list.count];

        if (!entry[0] || !entry[1] || !entry[2]) break;

        row->id = (int16_t) atoi(entry[0]);
        row->kind = (uint8_t) entry[1][0];
        strncpy(row->title, entry[2], PLUGIN_MAP_TITLE_SIZE - 1);
        row->title[PLUGIN_MAP_TITLE_SIZE - 1] = 0;
        plugin_map_unescape(row->title);

        g_plugin_list.count++;
    }

    g_plugin_list.loaded = 1;
}

/*
 * ask for the rows of the list from <first>, or centred on <focus_id> when <first> is
 * negative
 */
static void request_plugin_list(int16_t first, int16_t focus_id)
{
    uint8_t i;
    char buffer[32];
    memset(buffer, 0, sizeof buffer);

    ui_comm_webgui_set_response_cb(parse_plugin_list, NULL);
    ui_comm_webgui_clear_tx_buffer();

    // response: "r 1 40 0 3 48 7 p AUTOPAN 9 p BIG_MUFF ..."
    i = copy_command((char *)buffer, CMD_BUILDER_PLUGIN_LIST);
    i += int_to_str(first, &buffer[i], sizeof(buffer) - i, 0);
    buffer[i++] = ' ';
    i += int_to_str(focus_id, &buffer[i], sizeof(buffer) - i, 0);
    buffer[i++] = 0;

    ui_comm_webgui_send(buffer, i);
    ui_comm_webgui_wait_response();
}

/*
 * Slides the window when the cursor has got too close to either end of the rows held. The
 * board can change between two windows, so the cursor is pulled back inside a list that got
 * shorter meanwhile.
 */
static void list_follow(void)
{
    plugin_list_t *list = &g_plugin_list;
    uint16_t last = list->first + list->count;

    if (!list->loaded || list->total == 0) return;

    if (!plugin_list_hovered(list) ||
        (list->first > 0 && list->hover < (int16_t) (list->first + PLUGIN_LIST_ROWS_AROUND)) ||
        (last < list->total && list->hover >= (int16_t) (last - PLUGIN_LIST_ROWS_AROUND)))
    {
        request_plugin_list((int16_t) plugin_list_window_for(list), BM_NONE);
    }

    if (list->total > 0 && list->hover >= (int16_t) list->total)
        list->hover = list->total - 1;
}

/*
 * Fetches the list with the cursor on <focus_id>. A box that is not in the list -- a
 * hardware one, or the one just deleted -- leaves the cursor where it was.
 */
static void list_reload(int16_t focus_id)
{
    int16_t hover = g_plugin_list.hover;

    request_plugin_list(-1, focus_id);
    if (!g_plugin_list.loaded) return;

    if (g_list_position >= 0)
    {
        g_plugin_list.hover = g_list_position;
        return;
    }

    if (hover >= (int16_t) g_plugin_list.total) hover = g_plugin_list.total - 1;
    if (hover < 0) hover = 0;
    g_plugin_list.hover = hover;

    list_follow();
}

/*
 * The graph's selection follows the list cursor: the right display shows the box the graph
 * has selected, and the graph is what the list gives way to. With <fetch> off only within the
 * window of the graph in hand, and the move is left to BM_tick() when the box is outside it.
 */
static void list_sync(uint8_t fetch)
{
    const plugin_list_row_t *row = plugin_list_hovered(&g_plugin_list);
    const plugin_map_node_t *node = plugin_map_selected(&g_plugin_map);
    int8_t index;

    g_list_sync_pending = 0;

    if (!row || (node && node->id == row->id)) return;

    index = plugin_map_index_of(&g_plugin_map, row->id);

    if (index == BM_NONE)
    {
        if (!fetch)
        {
            g_list_sync_pending = 1;
            g_list_sync_stamp = xTaskGetTickCount();
            return;
        }

        request_plugin_map(row->id, 0);
        if (!g_plugin_map_loaded) return;

        index = plugin_map_index_of(&g_plugin_map, row->id);
        if (index == BM_NONE) return;
    }

    plugin_map_select(&g_plugin_map, index);
}

static void list_move(int8_t step)
{
    if (!plugin_list_step(&g_plugin_list, step)) return;

    list_follow();
    list_sync(0);
}

/*
 * Parameters of the plugin under the cursor
 */

// First option mod-ui sends of an enumeration whose current one is <index> of <count>.
// Mirrors the window of hmi.py control_add() and host.py _hmi_send_next_control_page_real().
static int16_t options_window_start(int16_t index, int16_t count)
{
    if (count <= PARAM_OPTIONS_WINDOW || index <= 2) return 0;
    if (index + 2 >= count) return count - PARAM_OPTIONS_WINDOW;
    return index - 2;
}

static uint8_t is_enumeration(const control_t *control)
{
    return (control->properties & (FLAG_CONTROL_ENUMERATION | FLAG_CONTROL_SCALE_POINTS | FLAG_CONTROL_REVERSE)) != 0;
}

/*
 * The step of a control from its value, as the encoders of control mode reckon it. For an
 * enumeration, <options> is how many it has in all -- the two messages that carry one count
 * them differently -- and the step is the place of the current one among those held, while
 * `steps` becomes the last index of the whole list.
 */
static void control_prepare(control_t *control, int16_t options)
{
    if (is_enumeration(control))
    {
        int16_t step = control->scale_point_index -
                       options_window_start(control->scale_point_index, options);

        if (step < 0) step = 0;
        if (control->scale_points_count && step >= control->scale_points_count)
            step = control->scale_points_count - 1;

        control->step = step;
        control->steps = (options > 0) ? options - 1 : 0;
    }
    else if (control->properties & (FLAG_CONTROL_BYPASS | FLAG_CONTROL_TOGGLED | FLAG_CONTROL_TRIGGER))
    {
        control->steps = 1;
        control->step = (control->value != 0);
    }
    else if (control->properties & FLAG_CONTROL_INTEGER)
    {
        control->steps = (control->maximum - control->minimum) + 1;
        control->step = control->value - control->minimum;
    }
    else
    {
        control->steps = PARAM_VALUE_STEPS;

        if (control->properties & FLAG_CONTROL_LOGARITHMIC)
        {
            if (control->minimum == 0) control->minimum = FLT_MIN;
            if (control->maximum == 0) control->maximum = FLT_MIN;
            if (control->value == 0) control->value = FLT_MIN;

            control->step = (control->steps - 1) * log(control->value / control->minimum) /
                            log(control->maximum / control->minimum);
        }
        else
        {
            control->step = (control->steps - 1) * (control->value - control->minimum) /
                            (control->maximum - control->minimum);
        }
    }

    if (control->step < 0) control->step = 0;
    if (control->step > control->steps) control->step = control->steps;
}

// the value of a control from its step; the same calculation as control mode
static void step_to_value(control_t *control)
{
    float p_step;

    if (is_enumeration(control))
    {
        if (control->scale_points_count)
            control->value = control->scale_points[control->step]->value;
        return;
    }

    if (control->properties & (FLAG_CONTROL_BYPASS | FLAG_CONTROL_TOGGLED | FLAG_CONTROL_TRIGGER))
        return;

    if (control->properties & FLAG_CONTROL_INTEGER)
    {
        control->value = control->minimum + control->step;
    }
    else
    {
        p_step = ((float) control->step) / ((float) (control->steps - 1));

        if (control->properties & FLAG_CONTROL_LOGARITHMIC)
            control->value = control->minimum * pow(control->maximum / control->minimum, p_step);
        else
            control->value = (p_step * (control->maximum - control->minimum)) + control->minimum;
    }

    if (control->value > control->maximum) control->value = control->maximum;
    if (control->value < control->minimum) control->value = control->minimum;
}

static void params_free_page(void)
{
    uint8_t i;

    for (i = 0; i < BM_PARAM_ROWS; i++)
    {
        if (g_params.controls[i]) data_free_control(g_params.controls[i]);
        g_params.controls[i] = NULL;
    }
}

static void params_clear(void)
{
    // what is still on its way belongs to a page nobody is looking at
    g_receiving_controls = 0;
    params_free_page();
    g_params.total = 0;
    g_params.first = 0;
    g_params.hover = 0;
    g_params.editing = 0;
    g_params_id = BM_NONE;
    g_params_pending = 0;
}

static void parse_controls_count(void *data, menu_item_t *item)
{
    (void) item;
    char **list = data;

    g_params.total = 0;

    if (!list || !list[0] || !list[1] || atoi(list[1]) == -1 || !list[2]) return;

    g_params.total = (uint8_t) atoi(list[2]);
}

/*
 * Asks for the parameters <start> .. <start + count - 1> of the plugin in g_params_uid. The
 * host sends them as control_adds and then answers with how many parameters there are in
 * all, so a count of zero just asks for that.
 */
static void request_controls(uint8_t start, uint8_t count)
{
    uint8_t i;
    char buffer[40];
    memset(buffer, 0, sizeof buffer);

    ui_comm_webgui_set_response_cb(parse_controls_count, NULL);
    ui_comm_webgui_clear_tx_buffer();

    i = copy_command((char *)buffer, CMD_BUILDER_CONTROL_LIST);
    strcpy(&buffer[i], g_params_uid);
    i += strlen(g_params_uid);
    buffer[i++] = ' ';
    i += int_to_str(start, &buffer[i], sizeof(buffer) - i, 0);
    buffer[i++] = ' ';
    i += int_to_str(count, &buffer[i], sizeof(buffer) - i, 0);
    buffer[i++] = 0;

    g_receiving_stamp = xTaskGetTickCount();
    g_receiving_controls = count;
    ui_comm_webgui_send(buffer, i);
    ui_comm_webgui_wait_response();
}

// the page of parameters with the cursor on it
static void params_fetch_page(void)
{
    uint8_t first = (g_params.hover / BM_PARAM_ROWS) * BM_PARAM_ROWS;
    uint8_t total = g_params.total;
    uint8_t count = (total - first) > BM_PARAM_ROWS ? BM_PARAM_ROWS : (total - first);

    params_free_page();
    g_params.first = first;

    if (count > 0) request_controls(first, count);

    // the answer counts them again, and the board may have changed meanwhile
    if (g_params.total == 0) g_params.total = total;
}

static void params_load(int16_t id, const char *title)
{
    params_clear();

    g_params_id = id;
    int_to_str(id, g_params_uid, sizeof(g_params_uid), 0);
    strncpy(g_params_title, title, sizeof(g_params_title) - 1);
    g_params_title[sizeof(g_params_title) - 1] = 0;
    g_params.title = g_params_title;

    request_controls(0, 0);

    if (g_params.total == 0)
    {
        // nothing to show, or the host does not know the plugin: the box itself instead
        g_params_id = BM_NONE;
        return;
    }

    params_fetch_page();
}

/*
 * The box under the cursor of the view on the left display: the list row, which the graph
 * may not have caught up with yet, or the graph's selection.
 */
static uint8_t cursor_box(int16_t *id, const char **title, uint8_t *kind)
{
    const plugin_list_row_t *row = g_list_view ? plugin_list_hovered(&g_plugin_list) : NULL;
    const plugin_map_node_t *node = g_plugin_map_loaded ? plugin_map_selected(&g_plugin_map) : NULL;

    if (row)
    {
        *id = row->id;
        *title = row->title;
        *kind = row->kind;
        return 1;
    }

    if (node)
    {
        *id = node->id;
        *title = node->title;
        *kind = node->kind;
        return 1;
    }

    return 0;
}

/*
 * The parameters follow the cursor of the left display: now, or with <now> off once it has
 * been still for PARAMS_LOAD_DELAY.
 */
static void params_follow(uint8_t now)
{
    int16_t id;
    const char *title;
    uint8_t kind;

    if (!cursor_box(&id, &title, &kind) || kind != BM_PLUGIN)
    {
        params_clear();
        return;
    }

    if (id == g_params_id && !g_params_pending) return;

    if (now)
    {
        params_load(id, title);
        return;
    }

    // the old page goes at once: it is not the box on screen any more
    params_clear();
    g_params_pending = 1;
    g_params_stamp = xTaskGetTickCount();
}

static void send_control_set(control_t *control)
{
    char buffer[40];
    uint8_t i;

    i = copy_command(buffer, CMD_BUILDER_CONTROL_SET);
    i += int_to_str(control->hw_id, &buffer[i], sizeof(buffer) - i, 0);
    buffer[i++] = ' ';
    i += float_to_str(control->value, &buffer[i], sizeof(buffer) - i, 3);
    buffer[i++] = 0;

    ui_comm_webgui_set_response_cb(NULL, NULL);
    ui_comm_webgui_clear_tx_buffer();
    ui_comm_webgui_send(buffer, i);
    ui_comm_webgui_wait_response();

    // the graph draws a bypassed plugin dashed; keep it in step without fetching it again
    if (control->properties & FLAG_CONTROL_BYPASS)
    {
        int8_t index = plugin_map_index_of(&g_plugin_map, g_params_id);
        if (index != BM_NONE) plugin_map_set_bypassed(&g_plugin_map, index, control->value != 0);
    }
}

static void parse_control_page(void *data, menu_item_t *item)
{
    (void) item;
    char **list = data;
    control_t *control;

    if (!list || !list[0] || !list[1] || atoi(list[1]) == -1) return;

    // "r 1 <hw_id> <label> ...": a control_add with "1" where the command name would be
    control = data_parse_control(&list[1]);
    if (!control) return;

    if (control->hw_id >= BM_PARAM_ROWS || !is_enumeration(control))
    {
        data_free_control(control);
        return;
    }

    // this message counts the options in `steps`, the control_add one less
    control_prepare(control, control->steps);

    if (g_params.controls[control->hw_id]) data_free_control(g_params.controls[control->hw_id]);
    g_params.controls[control->hw_id] = control;
}

/*
 * The options of an enumeration around <index>, which the host sends as the control again.
 */
static void request_control_page(control_t *control, int16_t index, uint8_t up)
{
    char buffer[32];
    uint8_t i;

    ui_comm_webgui_set_response_cb(parse_control_page, NULL);
    ui_comm_webgui_clear_tx_buffer();

    i = copy_command(buffer, CMD_BUILDER_CONTROL_PAGE);
    i += int_to_str(control->hw_id, &buffer[i], sizeof(buffer) - i, 0);
    buffer[i++] = ' ';
    i += int_to_str(up ? FLAG_PAGINATION_PAGE_UP : 0, &buffer[i], sizeof(buffer) - i, 0);
    buffer[i++] = ' ';
    i += int_to_str(index, &buffer[i], sizeof(buffer) - i, 0);
    buffer[i++] = 0;

    ui_comm_webgui_send(buffer, i);
    ui_comm_webgui_wait_response();
}

// the control under the cursor of the parameters, or NULL
static control_t *params_hovered(void)
{
    if (g_params_id == BM_NONE) return NULL;
    if (g_params.hover < g_params.first || g_params.hover >= g_params.first + BM_PARAM_ROWS) return NULL;

    return g_params.controls[g_params.hover - g_params.first];
}

/*
 * One step of the value under the cursor, either way. Ends of the range stop, they do not
 * wrap. An enumeration steps through its options, asking for the next ones when the step
 * leaves those held.
 */
static void params_turn(control_t *control, int8_t dir)
{
    uint8_t hw_id = control->hw_id;

    if (is_enumeration(control))
    {
        int16_t index = control->scale_point_index + dir;
        int16_t start = control->scale_point_index - control->step;

        if (index < 0 || index > control->steps) return;

        if (index >= start && index < start + control->scale_points_count)
        {
            control->step += dir;
            control->scale_point_index = index;
        }
        else
        {
            request_control_page(control, index, dir > 0);

            // the control was replaced, around the option asked for
            control = g_params.controls[hw_id];
            if (!control || control->scale_point_index != index) return;
        }
    }
    else if (control->properties & (FLAG_CONTROL_BYPASS | FLAG_CONTROL_TOGGLED))
    {
        float value = (dir > 0) ? 1 : 0;

        // a bypass is on when the plugin is off: right is on, for the plugin
        if (control->properties & FLAG_CONTROL_BYPASS) value = 1 - value;
        if (control->value == value) return;
        control->value = value;
    }
    else if (control->properties & FLAG_CONTROL_TRIGGER)
    {
        return;
    }
    else
    {
        int16_t step = control->step + dir;

        if (step < 0 || step > control->steps - 1) return;
        control->step = step;
    }

    step_to_value(control);
    send_control_set(control);
}

// ENC2 click: flips a switch or fires a trigger, and starts or ends the edit of anything else
static void params_click(void)
{
    control_t *control = params_hovered();

    if (!control) return;

    if (control->properties & (FLAG_CONTROL_BYPASS | FLAG_CONTROL_TOGGLED))
    {
        control->value = (control->value != 0) ? 0 : 1;
        send_control_set(control);
    }
    else if (control->properties & FLAG_CONTROL_TRIGGER)
    {
        control->value = control->maximum;
        send_control_set(control);
    }
    else
    {
        g_params.editing = !g_params.editing;
    }
}

// ENC2 turn: the value while editing, the cursor otherwise
static void params_move(int8_t dir)
{
    int16_t hover;

    if (g_params_id == BM_NONE) return;

    if (g_params.editing)
    {
        control_t *control = params_hovered();
        if (control) params_turn(control, dir);
        return;
    }

    hover = g_params.hover + dir;
    if (hover < 0 || hover >= g_params.total) return;

    g_params.hover = hover;

    if (hover < g_params.first || hover >= g_params.first + BM_PARAM_ROWS)
        params_fetch_page();
}

/*
 * Graph to list or back. The list opens on the box the graph has selected; the graph comes
 * back on the box under the list cursor. A host that does not answer the list keeps the graph.
 */
static void toggle_view(void)
{
    if (!g_list_view)
    {
        const plugin_map_node_t *node = plugin_map_selected(&g_plugin_map);

        list_reload(node ? node->id : BM_NONE);
        g_list_view = g_plugin_list.loaded;
    }
    else
    {
        list_sync(1);
        g_list_view = 0;
    }

    params_follow(1);
}

static void print_screen(void)
{
    const plugin_map_node_t *node;
    const plugin_list_row_t *row;

    // a BM_drop() may have given the panel away while a request was waited on
    if (!g_active) return;

    node = g_plugin_map_loaded ? plugin_map_selected(&g_plugin_map) : NULL;
    row = g_list_view ? plugin_list_hovered(&g_plugin_list) : NULL;

    screen_plugin_map(&g_plugin_map, g_plugin_map_loaded, g_list_view ? &g_plugin_list : NULL);

    if (g_params_id != BM_NONE)
    {
        screen_plugin_params(&g_params);
        return;
    }

    /*
     * The right display is about the box under the cursor of the view on screen. In the list
     * that is the row, which the graph may not have caught up with yet; the bypass flag is
     * only in the graph, and is left out until it has.
     */
    if (row)
        screen_plugin_details(row->title, row->kind, (node && node->id == row->id) ? node->bypassed : 0);
    else if (node)
        screen_plugin_details(node->title, node->kind, node->bypassed);
    else
        screen_plugin_details(NULL, 0, 0);
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

void BM_init(void)
{
    module_mutex = xSemaphoreCreateMutex();

    plugin_map_init(&g_plugin_map);
    plugin_list_clear(&g_plugin_list);

    // the viewport sits inside the outlines and above the button labels, so the graph is
    // clipped to it rather than painted over the chrome
    plugin_map_set_view(&g_plugin_map, PLUGIN_MAP_VIEW_X, PLUGIN_MAP_VIEW_Y, PLUGIN_MAP_VIEW_W, PLUGIN_MAP_VIEW_H);
}

uint8_t BM_is_active(void)
{
    return g_active;
}

void BM_enter(void)
{
    xSemaphoreTake(module_mutex, portMAX_DELAY);

    g_active = 1;
    g_restore_menu = 0;
    g_unwatch = 0;
    g_plugin_map_stale = 0;
    g_list_sync_pending = 0;
    g_params_dirty = 0;
    params_clear();
    request_plugin_map(BM_NONE, 1);

    plugin_list_clear(&g_plugin_list);
    if (g_list_view)
    {
        const plugin_map_node_t *node = plugin_map_selected(&g_plugin_map);

        list_reload(node ? node->id : BM_NONE);
        g_list_view = g_plugin_list.loaded;
    }

    params_follow(1);

    // ... and only now, so the host takes note of the board we have just fetched
    request_change_notify(1);
    g_watching = 1;

    /*
     * The first screen looks at the middle of the board, not at the selection. mod-ui
     * centres every column on one axis and puts the hardware pairs across it, so the
     * middle of the canvas is the middle of IN1/IN2 -- centring on IN1 alone would sit it
     * in the centre of the panel and push IN2 below. No guard needed on the pair falling
     * outside: it is centred too, and shorter than the viewport.
     */
    if (g_plugin_map_loaded)
    {
        g_plugin_map.offset_x = 0;
        g_plugin_map.offset_y = (g_plugin_map.scene_height - g_plugin_map.view.height) / 2;
        plugin_map_scroll(&g_plugin_map, 0, 0);
    }

    print_screen();

    xSemaphoreGive(module_mutex);
}

void BM_exit(void)
{
    xSemaphoreTake(module_mutex, portMAX_DELAY);

    g_active = 0;
    g_plugin_map_stale = 0;
    params_clear();

    // nothing should be running on the server for a screen nobody is on
    if (g_watching)
    {
        g_watching = 0;
        request_change_notify(0);
    }

    xSemaphoreGive(module_mutex);

    naveg_builder_left();
}

void BM_drop(uint8_t restore_menu)
{
    if (!g_active) return;

    g_active = 0;
    g_unwatch = g_watching;
    g_watching = 0;
    g_plugin_map_stale = 0;
    g_restore_menu = restore_menu;
}

void BM_up(uint8_t encoder)
{
    xSemaphoreTake(module_mutex, portMAX_DELAY);

    if (g_active)
    {
        // ENC1 walks whichever view is on the left display, ENC2 the parameters
        if (encoder == 0)
        {
            if (g_list_view)
                list_move(-1);
            else
                plugin_map_move(PLUGIN_MAP_PREV);

            params_follow(0);
        }
        else
        {
            params_move(-1);
        }

        print_screen();
    }

    xSemaphoreGive(module_mutex);
}

void BM_down(uint8_t encoder)
{
    xSemaphoreTake(module_mutex, portMAX_DELAY);

    if (g_active)
    {
        // ENC1 walks whichever view is on the left display, ENC2 the parameters
        if (encoder == 0)
        {
            if (g_list_view)
                list_move(1);
            else
                plugin_map_move(PLUGIN_MAP_NEXT);

            params_follow(0);
        }
        else
        {
            params_move(1);
        }

        print_screen();
    }

    xSemaphoreGive(module_mutex);
}

void BM_encoder_click(uint8_t encoder)
{
    if (encoder != 1) return;

    xSemaphoreTake(module_mutex, portMAX_DELAY);

    if (g_active)
    {
        params_click();
        print_screen();
    }

    xSemaphoreGive(module_mutex);
}

void BM_encoder_hold(uint8_t encoder)
{
    if (encoder != 0) return;

    xSemaphoreTake(module_mutex, portMAX_DELAY);

    if (g_active)
    {
        toggle_view();
        print_screen();
    }

    xSemaphoreGive(module_mutex);
}

void BM_button_pressed(uint8_t button)
{
    if (button == BM_BUTTON_BACK)
        BM_exit();
}

uint8_t BM_add_control(control_t *control)
{
    if (!g_receiving_controls || !control || control->hw_id >= BM_PARAM_ROWS) return 0;

    if ((xTaskGetTickCount() - g_receiving_stamp) >= (PARAMS_RECEIVE_TIMEOUT / portTICK_RATE_MS))
    {
        g_receiving_controls = 0;
        return 0;
    }

    g_receiving_controls--;

    // the control_add counts the options of an enumeration one less than there are
    control_prepare(control, control->steps + 1);

    // Not under the mutex, which the actuators task may hold waiting on an answer that only
    // comes after this callback returns. The page is only freed under the mutex before a new
    // request, and these arrive after it.
    if (g_params.controls[control->hw_id]) data_free_control(g_params.controls[control->hw_id]);
    g_params.controls[control->hw_id] = control;

    // drawn by BM_tick(), from the displays task
    g_params_dirty = 1;

    return 1;
}

void BM_set_control(uint8_t hw_id, float value)
{
    control_t *control;
    uint8_t i;

    if (!g_active || hw_id >= BM_PARAM_ROWS) return;

    control = g_params.controls[hw_id];
    if (!control) return;

    control->value = value;

    if (is_enumeration(control))
    {
        // an option among those held; another one shows when the page is fetched again
        for (i = 0; i < control->scale_points_count; i++)
        {
            if (control->scale_points[i]->value == value)
            {
                control->scale_point_index += i - control->step;
                control->step = i;
                break;
            }
        }
    }
    else
    {
        control_prepare(control, 0);
    }

    g_params_dirty = 1;
}

void BM_plugin_map_stale(void)
{
    g_plugin_map_stale = 1;
}

void BM_tick(void)
{
    if (g_unwatch)
    {
        xSemaphoreTake(module_mutex, portMAX_DELAY);

        // unless the builder was entered again meanwhile and the host is watching for it
        if (g_unwatch && !g_active)
            request_change_notify(0);

        g_unwatch = 0;

        xSemaphoreGive(module_mutex);
    }

    if (g_restore_menu)
    {
        g_restore_menu = 0;

        // only when nothing has taken the panel back meanwhile
        if (!g_active)
            naveg_builder_left();

        return;
    }

    if (g_active && g_list_sync_pending &&
        (xTaskGetTickCount() - g_list_sync_stamp) >= (PLUGIN_LIST_SYNC_DELAY / portTICK_RATE_MS))
    {
        xSemaphoreTake(module_mutex, portMAX_DELAY);

        if (g_active && g_list_sync_pending)
        {
            list_sync(1);
            print_screen();
        }

        xSemaphoreGive(module_mutex);
    }

    if (g_active && (g_params_dirty || (g_params_pending &&
        (xTaskGetTickCount() - g_params_stamp) >= (PARAMS_LOAD_DELAY / portTICK_RATE_MS))))
    {
        xSemaphoreTake(module_mutex, portMAX_DELAY);

        if (g_active && g_params_pending) params_follow(1);

        g_params_dirty = 0;
        print_screen();

        xSemaphoreGive(module_mutex);
    }

    if (!g_active || !g_plugin_map_stale) return;

    xSemaphoreTake(module_mutex, portMAX_DELAY);

    /*
     * The board changed under us, edited from the web UI most of the time. Keep looking at
     * the same box where it is still there; the request falls back to the host's own choice
     * of focus when it is not.
     */
    if (g_active && g_plugin_map_stale)
    {
        const plugin_map_node_t *node = plugin_map_selected(&g_plugin_map);
        const plugin_list_row_t *row = g_list_view ? plugin_list_hovered(&g_plugin_list) : NULL;
        int16_t focus = row ? row->id : (node ? node->id : BM_NONE);

        g_plugin_map_stale = 0;
        g_list_sync_pending = 0;

        request_plugin_map(focus, 0);
        if (g_list_view) list_reload(focus);

        // the parameters too, which may have changed with the board; the cursor stays put
        {
            uint8_t hover = g_params.hover, editing = g_params.editing;

            g_params_id = BM_NONE;
            params_follow(1);

            if (g_params_id != BM_NONE && hover < g_params.total)
            {
                g_params.hover = hover;
                g_params.editing = editing;
                if (hover >= BM_PARAM_ROWS) params_fetch_page();
            }
        }

        print_screen();
    }

    xSemaphoreGive(module_mutex);
}

void BM_print_screen(void)
{
    xSemaphoreTake(module_mutex, portMAX_DELAY);

    print_screen();

    xSemaphoreGive(module_mutex);
}
