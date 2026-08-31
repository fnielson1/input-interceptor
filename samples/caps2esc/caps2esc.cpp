/*
 * caps2esc.cpp -- a Windows port of the *behavioral idea* from caps2esc
 * (https://github.com/alexandre/caps2esc) by Alexandre: Caps Lock acts as
 * Escape when tapped alone, and as a real held Ctrl the moment it is
 * combined with another key. Credit for the remapping concept belongs to
 * that upstream project; the C++ below implementing it against this
 * repository's own interceptor API is original.
 *
 * This program runs windowless (GUI subsystem, no console): it has no
 * output and reads nothing from a console, so there is no window to close
 * and no console to Ctrl+C in. To stop it, end it from Task Manager.
 */

#include <windows.h>

#include "interceptor.h"
#include "utils.h"

#include <vector>

namespace
{
    const unsigned short ESC_CODE      = 0x01;
    const unsigned short CTRL_CODE     = 0x1D;
    const unsigned short CAPSLOCK_CODE = 0x3A;

    /* Persist across the whole run. */
    bool capslock_is_down = false;
    bool esc_give_up = false;

    InterceptorKeyStroke make_stroke(unsigned short code, unsigned short state)
    {
        InterceptorKeyStroke stroke;
        stroke.code = code;
        stroke.state = state;
        stroke.information = 0;
        return stroke;
    }

    bool matches(const InterceptorKeyStroke &s, unsigned short code, unsigned short state)
    {
        return s.code == code && s.state == state;
    }

    /* Maps one incoming stroke to zero or more outgoing strokes to
       reinject, per the caps2esc state machine, updating
       capslock_is_down/esc_give_up as a side effect. */
    std::vector<InterceptorKeyStroke> transform(const InterceptorKeyStroke &incoming)
    {
        std::vector<InterceptorKeyStroke> out;

        if (capslock_is_down)
        {
            if (matches(incoming, CAPSLOCK_CODE, INTERCEPTOR_KEY_DOWN) || incoming.code == CTRL_CODE)
            {
                /* OS auto-repeat of Caps Lock, or the real Ctrl key -- fully
                   swallowed so it never leaks through on its own while this
                   remap is active. */
                return out;
            }

            if (matches(incoming, CAPSLOCK_CODE, INTERCEPTOR_KEY_UP))
            {
                if (esc_give_up)
                {
                    out.push_back(make_stroke(CTRL_CODE, INTERCEPTOR_KEY_UP));
                    esc_give_up = false;
                }
                else
                {
                    /* Tapped and released without ever combining with
                       another key: synthesize an Escape tap. */
                    out.push_back(make_stroke(ESC_CODE, INTERCEPTOR_KEY_DOWN));
                    out.push_back(make_stroke(ESC_CODE, INTERCEPTOR_KEY_UP));
                }

                capslock_is_down = false;
                return out;
            }

            /* Some other key event while Caps Lock is held (and it wasn't
               Caps-Lock-repeat or Ctrl). */
            if (!esc_give_up && (incoming.state & INTERCEPTOR_KEY_UP) == 0)
            {
                /* First non-Ctrl, non-repeat key pressed this hold episode:
                   commit to "held Ctrl" and emit the real Ctrl-down once. */
                esc_give_up = true;
                out.push_back(make_stroke(CTRL_CODE, INTERCEPTOR_KEY_DOWN));
            }

            if (matches(incoming, ESC_CODE, INTERCEPTOR_KEY_DOWN))
                out.push_back(make_stroke(CAPSLOCK_CODE, INTERCEPTOR_KEY_DOWN));
            else if (matches(incoming, ESC_CODE, INTERCEPTOR_KEY_UP))
                out.push_back(make_stroke(CAPSLOCK_CODE, INTERCEPTOR_KEY_UP));
            else
                out.push_back(incoming);

            return out;
        }

        /* capslock_is_down == false */

        if (matches(incoming, CAPSLOCK_CODE, INTERCEPTOR_KEY_DOWN))
        {
            /* Swallow the real press; the block above now governs what
               holding Caps Lock produces. */
            capslock_is_down = true;
            return out;
        }

        if (matches(incoming, ESC_CODE, INTERCEPTOR_KEY_DOWN))
        {
            out.push_back(make_stroke(CAPSLOCK_CODE, INTERCEPTOR_KEY_DOWN));
            return out;
        }

        if (matches(incoming, ESC_CODE, INTERCEPTOR_KEY_UP))
        {
            out.push_back(make_stroke(CAPSLOCK_CODE, INTERCEPTOR_KEY_UP));
            return out;
        }

        out.push_back(incoming);
        return out;
    }
}

int WINAPI WinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int /*nCmdShow*/)
{
    void *single_instance;
    InterceptorContext context;

    /* Fixed GUID-shaped identity for the single-instance guard; reused from
       this sample's established convention so a rebuilt binary still
       recognizes an already-running prior instance. */
    single_instance = try_open_single_program("407631B6-78D3-4EFC-A868-40BBB7204CF1");
    if (single_instance == NULL)
        return 0;

    raise_process_priority();

    context = interceptor_create_context();
    if (context == 0)
    {
        close_single_program(single_instance);
        return 0;
    }

    interceptor_set_filter(context, interceptor_is_keyboard,
        INTERCEPTOR_FILTER_KEY_DOWN | INTERCEPTOR_FILTER_KEY_UP);

    for (;;)
    {
        InterceptorDevice device = interceptor_wait(context);
        InterceptorKeyStroke stroke;

        /* No Escape-based exit anywhere in this sample: the loop only ends
           if interceptor_receive stops returning a positive count, which in
           practice means the context was torn down from outside. */
        if (interceptor_receive(context, device, (InterceptorStroke *)&stroke, 1) <= 0)
            break;

        std::vector<InterceptorKeyStroke> outgoing = transform(stroke);
        if (!outgoing.empty())
        {
            std::vector<InterceptorStroke> batch(outgoing.size());
            for (size_t i = 0; i < outgoing.size(); i++)
                *(InterceptorKeyStroke *)&batch[i] = outgoing[i];

            interceptor_send(context, device, &batch[0], (unsigned int)batch.size());
        }
    }

    interceptor_destroy_context(context);
    close_single_program(single_instance);
    return 0;
}
