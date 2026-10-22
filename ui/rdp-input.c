/*
 * QEMU RDP server (libvds frontend): keyboard and mouse
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/input.h"
#include "rdp.h"
#include "trace.h"

/* XT scancodes of the lock keys */
#define RDP_SC_CAPS_LOCK   0x3a
#define RDP_SC_NUM_LOCK    0x45
#define RDP_SC_SCROLL_LOCK 0x46

/* Wheel notches injected for one client event at most. */
#define RDP_WHEEL_MAX_CLICKS 16

static struct {
    int ledstate;
    int last_x;
    int last_y;
    bool have_last;
} rdp_input;

static void rdp_led_event(void *opaque, int ledstate)
{
    rdp_input.ledstate = ledstate;
}

void rdp_input_init(void)
{
    qemu_add_led_event_handler(rdp_led_event, NULL);
}

void rdp_input_key(uint8_t scancode, bool extended, bool down)
{
    /* QEMU numeric keycodes: E0-prefixed keys have bit 7 set. */
    int qnum = (scancode & 0x7f) | (extended ? 0x80 : 0);

    qemu_input_event_send_key_number(NULL, qnum, down);
}

static void rdp_toggle(int scancode)
{
    qemu_input_event_send_key_number(NULL, scancode, true);
    qemu_input_event_send_key_number(NULL, scancode, false);
}

void rdp_input_lock_sync(bool scroll, bool num, bool caps)
{
    int leds = rdp_input.ledstate;

    trace_rdp_input_lock_sync(scroll, num, caps, leds);
    if (caps != !!(leds & QEMU_CAPS_LOCK_LED)) {
        rdp_toggle(RDP_SC_CAPS_LOCK);
    }
    if (num != !!(leds & QEMU_NUM_LOCK_LED)) {
        rdp_toggle(RDP_SC_NUM_LOCK);
    }
    if (scroll != !!(leds & QEMU_SCROLL_LOCK_LED)) {
        rdp_toggle(RDP_SC_SCROLL_LOCK);
    }
}

void rdp_input_pointer_abs(uint32_t head, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height)
{
    QemuConsole *con = rdp_display_console(head);

    if (!con) {
        return;
    }
    if (qemu_input_is_absolute(con)) {
        qemu_input_queue_abs(con, INPUT_AXIS_X, x, 0, width);
        qemu_input_queue_abs(con, INPUT_AXIS_Y, y, 0, height);
    } else if (rdp_input.have_last) {
        /* Relative-only guest pointer: send deltas. */
        qemu_input_queue_rel(con, INPUT_AXIS_X, (int)x - rdp_input.last_x);
        qemu_input_queue_rel(con, INPUT_AXIS_Y, (int)y - rdp_input.last_y);
    }
    rdp_input.last_x = x;
    rdp_input.last_y = y;
    rdp_input.have_last = true;
    qemu_input_event_sync();
}

void rdp_input_pointer_rel(int32_t dx, int32_t dy)
{
    qemu_input_queue_rel(NULL, INPUT_AXIS_X, dx);
    qemu_input_queue_rel(NULL, INPUT_AXIS_Y, dy);
    qemu_input_event_sync();
}

void rdp_input_button(uint32_t button, bool down)
{
    static const InputButton map[] = {
        [VDS_BUTTON_LEFT] = INPUT_BUTTON_LEFT,
        [VDS_BUTTON_RIGHT] = INPUT_BUTTON_RIGHT,
        [VDS_BUTTON_MIDDLE] = INPUT_BUTTON_MIDDLE,
        [VDS_BUTTON_SIDE] = INPUT_BUTTON_SIDE,
        [VDS_BUTTON_EXTRA] = INPUT_BUTTON_EXTRA,
    };

    if (button >= ARRAY_SIZE(map)) {
        return;
    }
    qemu_input_queue_btn(NULL, map[button], down);
    qemu_input_event_sync();
}

static void rdp_wheel_clicks(InputButton btn, int count)
{
    while (count-- > 0) {
        qemu_input_queue_btn(NULL, btn, true);
        qemu_input_event_sync();
        qemu_input_queue_btn(NULL, btn, false);
        qemu_input_event_sync();
    }
}

void rdp_input_wheel(int32_t dx, int32_t dy)
{
    trace_rdp_input_wheel(dx, dy);
    /* A bogus client value must not stall the main loop clicking. */
    dx = MAX(MIN(dx, RDP_WHEEL_MAX_CLICKS), -RDP_WHEEL_MAX_CLICKS);
    dy = MAX(MIN(dy, RDP_WHEEL_MAX_CLICKS), -RDP_WHEEL_MAX_CLICKS);
    rdp_wheel_clicks(dy > 0 ? INPUT_BUTTON_WHEEL_UP : INPUT_BUTTON_WHEEL_DOWN,
                     abs(dy));
    rdp_wheel_clicks(dx > 0 ? INPUT_BUTTON_WHEEL_RIGHT
                            : INPUT_BUTTON_WHEEL_LEFT, abs(dx));
}
