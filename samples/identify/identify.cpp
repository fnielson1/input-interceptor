/*
 * identify.cpp -- prints which device index produced each stroke. Run this
 * first to find your hardware.
 */

#include "interceptor.h"
#include "utils.h"

#include <iostream>

int main()
{
    InterceptorContext context;

    raise_process_priority();

    context = interceptor_create_context();
    if (context == 0)
    {
        std::cerr << "could not open the interceptor driver" << std::endl;
        return 1;
    }

    interceptor_set_filter(context, interceptor_is_keyboard,
        INTERCEPTOR_FILTER_KEY_DOWN | INTERCEPTOR_FILTER_KEY_UP);
    interceptor_set_filter(context, interceptor_is_mouse,
        INTERCEPTOR_FILTER_MOUSE_LEFT_BUTTON_DOWN);

    for (;;)
    {
        InterceptorDevice device = interceptor_wait(context);
        InterceptorStroke stroke;

        if (interceptor_receive(context, device, &stroke, 1) <= 0)
            break;

        if (interceptor_is_keyboard(device))
        {
            InterceptorKeyStroke *key = (InterceptorKeyStroke *)&stroke;

            std::cout << "INTERCEPTOR_KEYBOARD(" << (device - INTERCEPTOR_KEYBOARD(0)) << ")" << std::endl;

            if (key->code == 0x01) /* Escape: stop without reinjecting it */
                break;
        }
        else if (interceptor_is_mouse(device))
        {
            std::cout << "INTERCEPTOR_MOUSE(" << (device - INTERCEPTOR_MOUSE(0)) << ")" << std::endl;
        }
        else
        {
            std::cout << "UNRECOGNIZED(" << device << ")" << std::endl;
        }

        interceptor_send(context, device, &stroke, 1);
    }

    interceptor_destroy_context(context);
    return 0;
}
