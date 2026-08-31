/*
 * hardwareid.cpp -- prints the hardware ID string of the device you press a
 * key on (or left-click with, for a mouse).
 */

#include "interceptor.h"
#include "utils.h"

#include <iostream>

int main()
{
    InterceptorContext context;
    wchar_t hardware_id[500];

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
        unsigned int length;

        if (interceptor_receive(context, device, &stroke, 1) <= 0)
            break;

        if (interceptor_is_keyboard(device))
        {
            InterceptorKeyStroke *key = (InterceptorKeyStroke *)&stroke;
            if (key->code == 0x01) /* Escape: stop without reinjecting it */
                break;
        }

        /* Every surviving stroke (keyboard non-Escape, or mouse) reports the
           hardware id of the device it came from, before being passed on. */
        length = interceptor_get_hardware_id(context, device, hardware_id, sizeof(hardware_id));
        if (length > 0 && length < sizeof(hardware_id))
            std::wcout << hardware_id << std::endl;

        interceptor_send(context, device, &stroke, 1);
    }

    interceptor_destroy_context(context);
    return 0;
}
