/*
 * cadstop.cpp -- blocks Ctrl+Alt+Del: once two of {Ctrl, Alt, Delete} are
 * believed held down, the initial press of the third one is swallowed
 * instead of passed through.
 */

#include "interceptor.h"
#include "utils.h"

#include <iostream>

namespace
{
    const unsigned short ESC_CODE  = 0x01;
    const unsigned short CTRL_CODE = 0x1D;
    const unsigned short ALT_CODE  = 0x38;
    const unsigned short DEL_CODE  = 0x53;
    const unsigned short DEL_STATE_MASK = (unsigned short)(INTERCEPTOR_KEY_DOWN | INTERCEPTOR_KEY_E0);
    const unsigned short DEL_UP_STATE   = (unsigned short)(INTERCEPTOR_KEY_UP | INTERCEPTOR_KEY_E0);

    /* Believed-held state for each of the three tracked modifiers, carried
       across the whole run. */
    bool ctrl_is_down = false;
    bool alt_is_down  = false;
    bool del_is_down  = false;

    bool matches_ctrl_down(const InterceptorKeyStroke &s) { return s.code == CTRL_CODE && s.state == INTERCEPTOR_KEY_DOWN; }
    bool matches_ctrl_up(const InterceptorKeyStroke &s)   { return s.code == CTRL_CODE && s.state == INTERCEPTOR_KEY_UP; }
    bool matches_alt_down(const InterceptorKeyStroke &s)  { return s.code == ALT_CODE && s.state == INTERCEPTOR_KEY_DOWN; }
    bool matches_alt_up(const InterceptorKeyStroke &s)    { return s.code == ALT_CODE && s.state == INTERCEPTOR_KEY_UP; }
    bool matches_del_down(const InterceptorKeyStroke &s)  { return s.code == DEL_CODE && s.state == DEL_STATE_MASK; }
    bool matches_del_up(const InterceptorKeyStroke &s)    { return s.code == DEL_CODE && s.state == DEL_UP_STATE; }

    /* Decides whether `stroke` should be produced (reinjected). Also
       updates ctrl_is_down/alt_is_down/del_is_down as a side effect. */
    bool shall_produce_keystroke(const InterceptorKeyStroke &stroke)
    {
        int held_count = (ctrl_is_down ? 1 : 0) + (alt_is_down ? 1 : 0) + (del_is_down ? 1 : 0);

        if (held_count < 2)
        {
            /* Fewer than two modifiers held: just track state, let
               everything through unconditionally. */
            if (matches_ctrl_down(stroke))      ctrl_is_down = true;
            else if (matches_ctrl_up(stroke))   ctrl_is_down = false;
            else if (matches_alt_down(stroke))  alt_is_down = true;
            else if (matches_alt_up(stroke))    alt_is_down = false;
            else if (matches_del_down(stroke))  del_is_down = true;
            else if (matches_del_up(stroke))    del_is_down = false;

            return true;
        }

        /* Two or three modifiers already held: block the first appearance
           of whichever tracked modifier isn't yet believed down. */
        if ((matches_ctrl_down(stroke) || matches_ctrl_up(stroke)) && !ctrl_is_down)
            return false;
        if ((matches_alt_down(stroke) || matches_alt_up(stroke)) && !alt_is_down)
            return false;
        if ((matches_del_down(stroke) || matches_del_up(stroke)) && !del_is_down)
            return false;

        /* Not blocked: let a genuine release of an already-held modifier
           clear its flag, then pass the stroke through either way. */
        if (matches_ctrl_up(stroke))      ctrl_is_down = false;
        else if (matches_alt_up(stroke))  alt_is_down = false;
        else if (matches_del_up(stroke))  del_is_down = false;

        return true;
    }
}

int main()
{
    InterceptorContext context;

    raise_process_priority();

    context = interceptor_create_context();
    if (context == 0)
        return 1;

    interceptor_set_filter(context, interceptor_is_keyboard, INTERCEPTOR_FILTER_KEY_ALL);

    for (;;)
    {
        InterceptorDevice device = interceptor_wait(context);
        InterceptorKeyStroke stroke;

        if (interceptor_receive(context, device, (InterceptorStroke *)&stroke, 1) <= 0)
            break;

        if (!shall_produce_keystroke(stroke))
        {
            std::cout << "ctrl-alt-del pressed" << std::endl;
            continue;
        }

        interceptor_send(context, device, (InterceptorStroke *)&stroke, 1);

        if (stroke.code == ESC_CODE)
            break;
    }

    interceptor_destroy_context(context);
    return 0;
}
