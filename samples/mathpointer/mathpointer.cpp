/*
 * mathpointer.cpp -- drives the mouse pointer along parametric curves
 * (circle, spirals, trochoids, Lissajous, rose, butterfly, ...) selected by
 * pressing a top-row digit key, using the first mouse device to move as the
 * "real" mouse this sample impersonates.
 */

#include "interceptor.h"
#include "utils.h"

#include <cmath>
#include <iostream>

namespace
{
    const double PI = 3.14159265358979323846;
    const double SCALE = 15.0;
    const unsigned short ESC_CODE = 0x01;

    struct Point2D
    {
        double x;
        double y;
    };

    /* --- The ten named parametric curves, digit 0-9. Each returns its
       point already multiplied by the shared `scale` constant and any
       curve-specific coefficient; combining with a drawing center and the
       screen's Y-flip happens in trace_curve(). --- */

    Point2D curve_circle(double t)
    {
        Point2D p;
        p.x = SCALE * 10.0 * std::cos(t);
        p.y = SCALE * 10.0 * std::sin(t);
        return p;
    }

    Point2D curve_mirabilis(double t) /* logarithmic spiral */
    {
        double r = SCALE * 0.5 * std::exp(t / (2.0 * PI));
        Point2D p;
        p.x = r * std::cos(t);
        p.y = r * std::sin(t);
        return p;
    }

    Point2D curve_epitrochoid(double t)
    {
        const double R = 6.0, r = 2.0, d = 1.0, c = R + r;
        Point2D p;
        p.x = SCALE * (c * std::cos(t) - d * std::cos(c * t / r));
        p.y = SCALE * (c * std::sin(t) - d * std::sin(c * t / r));
        return p;
    }

    Point2D curve_hypotrochoid(double t)
    {
        const double R = 5.0, r = 3.0, d = 5.0, c = R - r;
        const double k = SCALE * (10.0 / 7.0);
        Point2D p;
        p.x = k * (c * std::cos(t) + d * std::cos(c * t / r));
        p.y = k * (c * std::sin(t) - d * std::sin(c * t / r));
        return p;
    }

    Point2D curve_hypocycloid(double t)
    {
        const double R = 3.0, r = 1.0, c = R - r;
        const double k = SCALE * (10.0 / 3.0);
        Point2D p;
        p.x = k * (c * std::cos(t) + r * std::cos(c * t / r));
        p.y = k * (c * std::sin(t) - r * std::sin(c * t / r));
        return p;
    }

    Point2D curve_bean(double t)
    {
        double c = std::cos(t);
        double s = std::sin(t);
        double factor = SCALE * 10.0 * (c * c * c + s * s * s);
        Point2D p;
        p.x = factor * c;
        p.y = factor * s;
        return p;
    }

    Point2D curve_lissajous(double t)
    {
        Point2D p;
        p.x = SCALE * 10.0 * std::sin(2.0 * t);
        p.y = SCALE * 10.0 * std::sin(3.0 * t);
        return p;
    }

    Point2D curve_epicycloid(double t)
    {
        const double R = 21.0, r = 10.0, c = R + r;
        const double k = SCALE * (10.0 / 42.0);
        Point2D p;
        p.x = k * (c * std::cos(t) - r * std::cos(c * t / r));
        p.y = k * (c * std::sin(t) - r * std::sin(c * t / r));
        return p;
    }

    Point2D curve_rose(double t)
    {
        const double k_freq = 2.0 / 7.0;
        double factor = SCALE * 10.0 * std::cos(k_freq * t);
        Point2D p;
        p.x = factor * std::cos(t);
        p.y = factor * std::sin(t);
        return p;
    }

    Point2D curve_butterfly(double t)
    {
        double c = std::exp(std::cos(t)) - 2.0 * std::cos(4.0 * t) + std::pow(std::sin(t / 12.0), 5.0);
        const double k = SCALE * (10.0 / 4.0);
        Point2D p;
        p.x = k * std::sin(t) * c;
        p.y = k * std::cos(t) * c;
        return p;
    }

    struct CurveSpec
    {
        Point2D (*fn)(double);
        double t1;
        double t2;
        int steps;
    };

    const CurveSpec CURVES[10] =
    {
        { curve_circle,        0.0,       2.0 * PI, 200  }, /* 0 */
        { curve_mirabilis,    -6.0 * PI,  6.0 * PI, 200  }, /* 1 */
        { curve_epitrochoid,   0.0,       2.0 * PI, 200  }, /* 2 */
        { curve_hypotrochoid,  0.0,       6.0 * PI, 200  }, /* 3 */
        { curve_hypocycloid,   0.0,       2.0 * PI, 200  }, /* 4 */
        { curve_bean,          0.0,       PI,       200  }, /* 5 */
        { curve_lissajous,     0.0,       2.0 * PI, 200  }, /* 6 */
        { curve_epicycloid,    0.0,      20.0 * PI, 1000 }, /* 7 */
        { curve_rose,          0.0,      14.0 * PI, 500  }, /* 8 */
        { curve_butterfly,     0.0,      21.0 * PI, 2000 }, /* 9 */
    };

    /* Top-row digit scan codes, index == digit. Numpad digits are a
       different set of scan codes and are intentionally not matched here. */
    const unsigned short DIGIT_SCAN_CODES[10] = { 0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A };

    int digit_from_scan_code(unsigned short code)
    {
        for (int digit = 0; digit < 10; digit++)
        {
            if (DIGIT_SCAN_CODES[digit] == code)
                return digit;
        }
        return -1;
    }

    long to_absolute_coordinate(double value, int screen_dimension)
    {
        return (long)((0xFFFF * value) / screen_dimension);
    }

    void send_pointer_move(InterceptorContext context, InterceptorDevice device, const Point2D &position,
        unsigned short state, int screen_width, int screen_height)
    {
        InterceptorMouseStroke mouse;
        mouse.state = state;
        mouse.flags = INTERCEPTOR_MOUSE_MOVE_ABSOLUTE;
        mouse.rolling = 0;
        mouse.x = (int)to_absolute_coordinate(position.x, screen_width);
        mouse.y = (int)to_absolute_coordinate(position.y, screen_height);
        mouse.information = 0;

        interceptor_send(context, device, (InterceptorStroke *)&mouse, 1);
    }

    Point2D combine(const Point2D &center, const Point2D &curve_point)
    {
        /* Screen Y grows downward; curves are defined in a conventional
           math frame where Y grows upward, hence the subtraction. */
        Point2D p;
        p.x = center.x + curve_point.x;
        p.y = center.y - curve_point.y;
        return p;
    }

    void trace_curve(InterceptorContext context, InterceptorDevice mouse_device, Point2D (*curve)(double),
        const Point2D &center, double t1, double t2, int partitioning, unsigned long delay_unit,
        int screen_width, int screen_height)
    {
        lower_process_priority();

        send_pointer_move(context, mouse_device, center, INTERCEPTOR_MOUSE_LEFT_BUTTON_UP, screen_width, screen_height);

        Point2D current_position = combine(center, curve(t1));
        send_pointer_move(context, mouse_device, current_position, 0, screen_width, screen_height);

        unsigned short state = 0;
        int j = 0;

        for (int i = 0; i <= partitioning + 2; i++, j++)
        {
            if (j % 250 == 0)
            {
                busy_wait(delay_unit * 25);
                send_pointer_move(context, mouse_device, current_position, INTERCEPTOR_MOUSE_LEFT_BUTTON_UP, screen_width, screen_height);

                busy_wait(delay_unit * 25);
                send_pointer_move(context, mouse_device, current_position, INTERCEPTOR_MOUSE_LEFT_BUTTON_DOWN, screen_width, screen_height);

                state = 0;

                if (i > 0)
                    i -= 2; /* re-cover the last couple of steps instead of jumping ahead */
            }

            double t = t1 + i * (t2 - t1) / partitioning;
            current_position = combine(center, curve(t));
            send_pointer_move(context, mouse_device, current_position, state, screen_width, screen_height);

            busy_wait(delay_unit * 3);
        }

        busy_wait(delay_unit * 25);
        send_pointer_move(context, mouse_device, current_position, INTERCEPTOR_MOUSE_LEFT_BUTTON_DOWN, screen_width, screen_height);

        busy_wait(delay_unit * 25);
        send_pointer_move(context, mouse_device, current_position, INTERCEPTOR_MOUSE_LEFT_BUTTON_UP, screen_width, screen_height);

        busy_wait(delay_unit * 25);
        send_pointer_move(context, mouse_device, center, 0, screen_width, screen_height);

        raise_process_priority();
    }
}

int main()
{
    InterceptorContext context;
    InterceptorDevice claimed_mouse = 0; /* 0: none claimed yet (device ids start at 11) */
    Point2D position;
    int screen_width;
    int screen_height;
    unsigned long delay_unit;

    raise_process_priority();

    context = interceptor_create_context();
    if (context == 0)
        return 1;

    interceptor_set_filter(context, interceptor_is_keyboard,
        INTERCEPTOR_FILTER_KEY_DOWN | INTERCEPTOR_FILTER_KEY_UP);
    interceptor_set_filter(context, interceptor_is_mouse, INTERCEPTOR_FILTER_MOUSE_MOVE);

    screen_width = get_screen_width();
    screen_height = get_screen_height();

    std::cout <<
        "mathpointer only behaves correctly on a real machine: virtual\n"
        "machines commonly report absolute mouse positions, which this\n"
        "sample does not handle.\n"
        "Move the real mouse you want this sample to take over -- the first\n"
        "mouse device to produce a stroke becomes the one it drives.\n" << std::endl;

    position.x = screen_width / 2.0;
    position.y = screen_height / 2.0;

    delay_unit = calculate_busy_wait_millisecond();

    for (;;)
    {
        InterceptorDevice device = interceptor_wait(context);
        InterceptorStroke stroke;

        if (interceptor_receive(context, device, &stroke, 1) <= 0)
            break;

        if (interceptor_is_mouse(device))
        {
            InterceptorMouseStroke *mouse = (InterceptorMouseStroke *)&stroke;

            if (claimed_mouse == 0)
            {
                claimed_mouse = device;
                std::cout << "claimed INTERCEPTOR_MOUSE(" << (device - INTERCEPTOR_MOUSE(0)) << ")" << std::endl;
                std::cout <<
                    "Open a drawing program (e.g. Paint), select the pencil tool,\n"
                    "position the mouse over the canvas, then press a digit key (not\n"
                    "on the numpad) to draw the matching curve, or Escape to exit.\n" << std::endl;
            }

            position.x += mouse->x;
            position.y += mouse->y;

            if (position.x < 0.0) position.x = 0.0;
            if (position.x > screen_width - 1) position.x = screen_width - 1;
            if (position.y < 0.0) position.y = 0.0;
            if (position.y > screen_height - 1) position.y = screen_height - 1;

            mouse->flags = INTERCEPTOR_MOUSE_MOVE_ABSOLUTE;
            mouse->x = (int)to_absolute_coordinate(position.x, screen_width);
            mouse->y = (int)to_absolute_coordinate(position.y, screen_height);

            interceptor_send(context, device, &stroke, 1);
        }

        if (interceptor_is_keyboard(device) && claimed_mouse != 0)
        {
            InterceptorKeyStroke *key = (InterceptorKeyStroke *)&stroke;
            int digit = digit_from_scan_code(key->code);

            if (key->state == INTERCEPTOR_KEY_DOWN && digit >= 0)
            {
                const CurveSpec &curve = CURVES[digit];
                trace_curve(context, claimed_mouse, curve.fn, position, curve.t1, curve.t2, curve.steps,
                    delay_unit, screen_width, screen_height);
            }
            else if (key->state == INTERCEPTOR_KEY_DOWN)
            {
                interceptor_send(context, device, &stroke, 1);
            }
            else if (key->state == INTERCEPTOR_KEY_UP && digit >= 0)
            {
                /* Swallow: the matching key-down for this digit didn't pass
                   through either, so neither should its key-up. */
            }
            else if (key->state == INTERCEPTOR_KEY_UP)
            {
                interceptor_send(context, device, &stroke, 1);
            }
            else
            {
                /* E0/E1/TERMSRV variants etc.: pass through unchanged. */
                interceptor_send(context, device, &stroke, 1);
            }

            if (key->code == ESC_CODE)
                break;
        }
    }

    interceptor_destroy_context(context);
    return 0;
}
