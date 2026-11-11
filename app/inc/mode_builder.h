/*
************************************************************************************************************************
*           Pedalboard builder: the pedalboard graph on the panel.
*
*           The Duo X has no modes the way the Dwarf has: the builder sits on top of the system menu,
*           entered from CURRENT PEDALBOARD > EDIT PEDALBOARD, and gives the menu back when it is left.
*           The menu tools stay on underneath the whole time, so nothing the host sends meanwhile
*           draws a control over the graph.
*
*           Left display: the graph. Right display: the box under the cursor.
*           ENC1 walks the boxes, BTN1 goes back to the menu.
************************************************************************************************************************
*/

#ifndef MODE_BUILDER_H
#define MODE_BUILDER_H


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <stdint.h>


/*
************************************************************************************************************************
*           DO NOT CHANGE THESE DEFINES
************************************************************************************************************************
*/

// the three buttons between the footswitches, as BM_button_pressed() numbers them
#define BM_BUTTON_BACK      0
#define BM_BUTTON_ADD       1
#define BM_BUTTON_DEL       2


/*
************************************************************************************************************************
*           FUNCTION PROTOTYPES
************************************************************************************************************************
*/

void BM_init(void);

// Whether the builder owns the panel. Read from every task, so it is only a flag.
uint8_t BM_is_active(void);

// From the menu: fetches the graph, asks the host to watch the board and draws both displays.
void BM_enter(void);

// Back to the menu. Tells the host to stop watching, so it must not be called from a protocol
// callback; BM_drop() is for those.
void BM_exit(void);

// Leaves without a word to the host, for the paths that come from a protocol callback, where
// a command sent and waited on would deadlock. Only clears the state: the caller is about to
// redraw the panel itself, or asks BM_tick() to put the menu back.
void BM_drop(uint8_t restore_menu);

void BM_up(uint8_t encoder);
void BM_down(uint8_t encoder);
void BM_encoder_click(uint8_t encoder);
void BM_button_pressed(uint8_t button);

// The host says the board changed. Only a flag: the refetch happens in BM_tick().
void BM_plugin_map_stale(void);

// From the displays task: refetches a stale graph and finishes a BM_drop().
void BM_tick(void);

void BM_print_screen(void);


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
