/*
 * axes.cpp -- inverts the mouse Y axis for relative-motion packets.
 */

#include "interceptor.h"
#include "utils.h"

int main()
{
    InterceptorContext context;

    raise_process_priority();

    context = interceptor_create_context();
    if (context == 0)
        return 1;

    interceptor_set_filter(context, interceptor_is_keyboard,
        INTERCEPTOR_FILTER_KEY_DOWN | INTERCEPTOR_FILTER_KEY_UP);
    interceptor_set_filter(context, interceptor_is_mouse,
        INTERCEPTOR_FILTER_MOUSE_MOVE);

    for (;;)
    {
        InterceptorDevice device = interceptor_wait(context);
        InterceptorStroke stroke;

        if (interceptor_receive(context, device, &stroke, 1) <= 0)
            break;

        /* Two independent checks -- in principle both could apply to the
           same stroke, though in practice a device is exactly one kind. */

        if (interceptor_is_mouse(device))
        {
            InterceptorMouseStroke *mouse = (InterceptorMouseStroke *)&stroke;

            if ((mouse->flags & INTERCEPTOR_MOUSE_MOVE_ABSOLUTE) == 0)
                mouse->y = -mouse->y;

            interceptor_send(context, device, &stroke, 1);
        }

        if (interceptor_is_keyboard(device))
        {
            InterceptorKeyStroke *key = (InterceptorKeyStroke *)&stroke;

            interceptor_send(context, device, &stroke, 1);

            if (key->code == 0x01) /* Escape: only the keyboard path can end the loop */
                break;
        }
    }

    interceptor_destroy_context(context);
    return 0;
}
