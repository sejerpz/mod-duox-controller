/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <stdlib.h>
#include <string.h>

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

// The graph follows the list cursor once the encoder has been still this long, in ms, when
// the box is outside the window of the graph in hand: one fetch per step would make the list
// crawl, and the graph is not where the eye is while the list is turning.
#define PLUGIN_LIST_SYNC_DELAY 250


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
 * The list on the right display: every plugin on the board by name, which mod-ui sends a
 * window at a time. Not taken from the graph's window, whose boxes are the ones around the
 * selection -- their alphabetical neighbours are anywhere on the board. The hardware boxes
 * have nothing to open, bypass or delete, so mod-ui leaves them out.
 */
static plugin_list_t g_plugin_list;
/* where the box a list request was centred on sits in the whole list, or BM_NONE */
static int16_t g_list_position = BM_NONE;

/* the list cursor moved to a box outside the graph's window; BM_tick() fetches it */
static uint8_t g_list_sync_pending;
static portTickType g_list_sync_stamp;

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
 * The list cursor follows the graph. A plugin among the rows held costs nothing; one outside
 * them fetches the window around it. A hardware box is not in the list, and the cursor stays.
 */
static void list_follow_graph(void)
{
    const plugin_map_node_t *node = plugin_map_selected(&g_plugin_map);
    uint8_t i;

    if (!node || node->kind != BM_PLUGIN) return;

    for (i = 0; i < g_plugin_list.count; i++)
    {
        if (g_plugin_list.rows[i].id == node->id)
        {
            g_plugin_list.hover = g_plugin_list.first + i;
            list_follow();
            return;
        }
    }

    list_reload(node->id);
}

/*
 * The graph follows the list cursor. With <fetch> off only within the window of the graph
 * in hand, and the move is left to BM_tick() when the box is outside it.
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

static void print_screen(void)
{
    // a BM_drop() may have given the panel away while a request was waited on
    if (!g_active) return;

    screen_plugin_map(&g_plugin_map, g_plugin_map_loaded);
    screen_plugin_list(&g_plugin_list, g_plugin_map_loaded ? plugin_map_selected(&g_plugin_map) : NULL);
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
    request_plugin_map(BM_NONE, 1);

    {
        const plugin_map_node_t *node = plugin_map_selected(&g_plugin_map);
        plugin_list_clear(&g_plugin_list);
        list_reload(node ? node->id : BM_NONE);
    }

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
        // ENC1 walks the graph and the list follows, ENC2 walks the list and the graph follows
        if (encoder == 0)
        {
            g_list_sync_pending = 0;
            plugin_map_move(PLUGIN_MAP_PREV);
            list_follow_graph();
        }
        else
        {
            list_move(-1);
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
        // ENC1 walks the graph and the list follows, ENC2 walks the list and the graph follows
        if (encoder == 0)
        {
            g_list_sync_pending = 0;
            plugin_map_move(PLUGIN_MAP_NEXT);
            list_follow_graph();
        }
        else
        {
            list_move(1);
        }

        print_screen();
    }

    xSemaphoreGive(module_mutex);
}

void BM_encoder_click(uint8_t encoder)
{
    // opens the box under the cursor once there is a parameter page to open
    (void) encoder;
}

void BM_button_pressed(uint8_t button)
{
    if (button == BM_BUTTON_BACK)
        BM_exit();
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

        g_plugin_map_stale = 0;
        g_list_sync_pending = 0;

        request_plugin_map(node ? node->id : BM_NONE, 0);

        // the graph's selection again, which may have fallen back on another box
        node = plugin_map_selected(&g_plugin_map);
        list_reload(node ? node->id : BM_NONE);
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
