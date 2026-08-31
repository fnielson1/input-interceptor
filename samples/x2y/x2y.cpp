/*
 * x2y.cpp -- physical 'x' produces 'y'. This is deliberately one-directional
 * (physical 'y' is left untouched); do not "fix" it into a true swap.
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

    for (;;)
    {
        InterceptorDevice device = interceptor_wait(context);
        InterceptorKeyStroke stroke;
        unsigned short original_code;

        if (interceptor_receive(context, device, (InterceptorStroke *)&stroke, 1) <= 0)
            break;

        original_code = stroke.code;

        if (stroke.code == 0x2D) /* x -> y */
            stroke.code = 0x15;

        interceptor_send(context, device, (InterceptorStroke *)&stroke, 1);

        if (original_code == 0x01) /* Escape, reinjected above, now exit */
            break;
    }

    interceptor_destroy_context(context);
    return 0;
}
