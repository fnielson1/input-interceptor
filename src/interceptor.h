#ifndef _INTERCEPTOR_H_
#define _INTERCEPTOR_H_

#ifdef INTERCEPTOR_STATIC
    #define INTERCEPTOR_API
#else
    #if defined _WIN32 || defined __CYGWIN__
        #ifdef INTERCEPTOR_EXPORT
            #ifdef __GNUC__
                #define INTERCEPTOR_API __attribute__((dllexport))
            #else
                #define INTERCEPTOR_API __declspec(dllexport)
            #endif
        #else
            #ifdef __GNUC__
                #define INTERCEPTOR_API __attribute__((dllimport))
            #else
                #define INTERCEPTOR_API __declspec(dllimport)
            #endif
        #endif
    #else
        #if __GNUC__ >= 4
            #define INTERCEPTOR_API __attribute__ ((visibility("default")))
        #else
            #define INTERCEPTOR_API
        #endif
    #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define INTERCEPTOR_MAX_KEYBOARD 10

#define INTERCEPTOR_MAX_MOUSE 10

#define INTERCEPTOR_MAX_DEVICE ((INTERCEPTOR_MAX_KEYBOARD) + (INTERCEPTOR_MAX_MOUSE))

#define INTERCEPTOR_KEYBOARD(index) ((index) + 1)

#define INTERCEPTOR_MOUSE(index) ((INTERCEPTOR_MAX_KEYBOARD) + (index) + 1)

typedef void *InterceptorContext;

typedef int InterceptorDevice;

typedef int InterceptorPrecedence;

typedef unsigned short InterceptorFilter;

typedef int (*InterceptorPredicate)(InterceptorDevice device);

enum InterceptorKeyState
{
    INTERCEPTOR_KEY_DOWN             = 0x00,
    INTERCEPTOR_KEY_UP               = 0x01,
    INTERCEPTOR_KEY_E0               = 0x02,
    INTERCEPTOR_KEY_E1               = 0x04,
    INTERCEPTOR_KEY_TERMSRV_SET_LED  = 0x08,
    INTERCEPTOR_KEY_TERMSRV_SHADOW   = 0x10,
    INTERCEPTOR_KEY_TERMSRV_VKPACKET = 0x20
};

enum InterceptorFilterKeyState
{
    INTERCEPTOR_FILTER_KEY_NONE             = 0x0000,
    INTERCEPTOR_FILTER_KEY_ALL              = 0xFFFF,
    INTERCEPTOR_FILTER_KEY_DOWN             = INTERCEPTOR_KEY_UP,
    INTERCEPTOR_FILTER_KEY_UP               = INTERCEPTOR_KEY_UP << 1,
    INTERCEPTOR_FILTER_KEY_E0               = INTERCEPTOR_KEY_E0 << 1,
    INTERCEPTOR_FILTER_KEY_E1               = INTERCEPTOR_KEY_E1 << 1,
    INTERCEPTOR_FILTER_KEY_TERMSRV_SET_LED  = INTERCEPTOR_KEY_TERMSRV_SET_LED << 1,
    INTERCEPTOR_FILTER_KEY_TERMSRV_SHADOW   = INTERCEPTOR_KEY_TERMSRV_SHADOW << 1,
    INTERCEPTOR_FILTER_KEY_TERMSRV_VKPACKET = INTERCEPTOR_KEY_TERMSRV_VKPACKET << 1
};

enum InterceptorMouseState
{
    INTERCEPTOR_MOUSE_LEFT_BUTTON_DOWN   = 0x001,
    INTERCEPTOR_MOUSE_LEFT_BUTTON_UP     = 0x002,
    INTERCEPTOR_MOUSE_RIGHT_BUTTON_DOWN  = 0x004,
    INTERCEPTOR_MOUSE_RIGHT_BUTTON_UP    = 0x008,
    INTERCEPTOR_MOUSE_MIDDLE_BUTTON_DOWN = 0x010,
    INTERCEPTOR_MOUSE_MIDDLE_BUTTON_UP   = 0x020,

    INTERCEPTOR_MOUSE_BUTTON_1_DOWN      = INTERCEPTOR_MOUSE_LEFT_BUTTON_DOWN,
    INTERCEPTOR_MOUSE_BUTTON_1_UP        = INTERCEPTOR_MOUSE_LEFT_BUTTON_UP,
    INTERCEPTOR_MOUSE_BUTTON_2_DOWN      = INTERCEPTOR_MOUSE_RIGHT_BUTTON_DOWN,
    INTERCEPTOR_MOUSE_BUTTON_2_UP        = INTERCEPTOR_MOUSE_RIGHT_BUTTON_UP,
    INTERCEPTOR_MOUSE_BUTTON_3_DOWN      = INTERCEPTOR_MOUSE_MIDDLE_BUTTON_DOWN,
    INTERCEPTOR_MOUSE_BUTTON_3_UP        = INTERCEPTOR_MOUSE_MIDDLE_BUTTON_UP,

    INTERCEPTOR_MOUSE_BUTTON_4_DOWN      = 0x040,
    INTERCEPTOR_MOUSE_BUTTON_4_UP        = 0x080,
    INTERCEPTOR_MOUSE_BUTTON_5_DOWN      = 0x100,
    INTERCEPTOR_MOUSE_BUTTON_5_UP        = 0x200,

    INTERCEPTOR_MOUSE_WHEEL              = 0x400,
    INTERCEPTOR_MOUSE_HWHEEL             = 0x800
};

enum InterceptorFilterMouseState
{
    INTERCEPTOR_FILTER_MOUSE_NONE               = 0x0000,
    INTERCEPTOR_FILTER_MOUSE_ALL                = 0xFFFF,

    INTERCEPTOR_FILTER_MOUSE_LEFT_BUTTON_DOWN   = INTERCEPTOR_MOUSE_LEFT_BUTTON_DOWN,
    INTERCEPTOR_FILTER_MOUSE_LEFT_BUTTON_UP     = INTERCEPTOR_MOUSE_LEFT_BUTTON_UP,
    INTERCEPTOR_FILTER_MOUSE_RIGHT_BUTTON_DOWN  = INTERCEPTOR_MOUSE_RIGHT_BUTTON_DOWN,
    INTERCEPTOR_FILTER_MOUSE_RIGHT_BUTTON_UP    = INTERCEPTOR_MOUSE_RIGHT_BUTTON_UP,
    INTERCEPTOR_FILTER_MOUSE_MIDDLE_BUTTON_DOWN = INTERCEPTOR_MOUSE_MIDDLE_BUTTON_DOWN,
    INTERCEPTOR_FILTER_MOUSE_MIDDLE_BUTTON_UP   = INTERCEPTOR_MOUSE_MIDDLE_BUTTON_UP,

    INTERCEPTOR_FILTER_MOUSE_BUTTON_1_DOWN      = INTERCEPTOR_MOUSE_BUTTON_1_DOWN,
    INTERCEPTOR_FILTER_MOUSE_BUTTON_1_UP        = INTERCEPTOR_MOUSE_BUTTON_1_UP,
    INTERCEPTOR_FILTER_MOUSE_BUTTON_2_DOWN      = INTERCEPTOR_MOUSE_BUTTON_2_DOWN,
    INTERCEPTOR_FILTER_MOUSE_BUTTON_2_UP        = INTERCEPTOR_MOUSE_BUTTON_2_UP,
    INTERCEPTOR_FILTER_MOUSE_BUTTON_3_DOWN      = INTERCEPTOR_MOUSE_BUTTON_3_DOWN,
    INTERCEPTOR_FILTER_MOUSE_BUTTON_3_UP        = INTERCEPTOR_MOUSE_BUTTON_3_UP,

    INTERCEPTOR_FILTER_MOUSE_BUTTON_4_DOWN      = INTERCEPTOR_MOUSE_BUTTON_4_DOWN,
    INTERCEPTOR_FILTER_MOUSE_BUTTON_4_UP        = INTERCEPTOR_MOUSE_BUTTON_4_UP,
    INTERCEPTOR_FILTER_MOUSE_BUTTON_5_DOWN      = INTERCEPTOR_MOUSE_BUTTON_5_DOWN,
    INTERCEPTOR_FILTER_MOUSE_BUTTON_5_UP        = INTERCEPTOR_MOUSE_BUTTON_5_UP,

    INTERCEPTOR_FILTER_MOUSE_WHEEL              = INTERCEPTOR_MOUSE_WHEEL,
    INTERCEPTOR_FILTER_MOUSE_HWHEEL             = INTERCEPTOR_MOUSE_HWHEEL,

    INTERCEPTOR_FILTER_MOUSE_MOVE               = 0x1000
};

enum InterceptorMouseFlag
{
    INTERCEPTOR_MOUSE_MOVE_RELATIVE      = 0x000,
    INTERCEPTOR_MOUSE_MOVE_ABSOLUTE      = 0x001,
    INTERCEPTOR_MOUSE_VIRTUAL_DESKTOP    = 0x002,
    INTERCEPTOR_MOUSE_ATTRIBUTES_CHANGED = 0x004,
    INTERCEPTOR_MOUSE_MOVE_NOCOALESCE    = 0x008,
    INTERCEPTOR_MOUSE_TERMSRV_SRC_SHADOW = 0x100
};

typedef struct
{
    unsigned short state;
    unsigned short flags;
    short rolling;
    int x;
    int y;
    unsigned int information;
} InterceptorMouseStroke;

typedef struct
{
    unsigned short code;
    unsigned short state;
    unsigned int information;
} InterceptorKeyStroke;

typedef char InterceptorStroke[sizeof(InterceptorMouseStroke)];

InterceptorContext INTERCEPTOR_API interceptor_create_context(void);

void INTERCEPTOR_API interceptor_destroy_context(InterceptorContext context);

InterceptorPrecedence INTERCEPTOR_API interceptor_get_precedence(InterceptorContext context, InterceptorDevice device);

void INTERCEPTOR_API interceptor_set_precedence(InterceptorContext context, InterceptorDevice device, InterceptorPrecedence precedence);

InterceptorFilter INTERCEPTOR_API interceptor_get_filter(InterceptorContext context, InterceptorDevice device);

void INTERCEPTOR_API interceptor_set_filter(InterceptorContext context, InterceptorPredicate predicate, InterceptorFilter filter);

/*
 * interceptor_set_filter's passive counterpart, and not part of upstream
 * Interception's API: a device matching a monitor filter is still delivered
 * to the rest of the system -- a copy is *also* queued here for
 * interceptor_receive, but nothing is withheld the way interceptor_set_filter
 * withholds a match.
 *
 * Exists so a caller that already holds a context never needs its own raw
 * input registration for devices it isn't capturing. Windows allows only one
 * raw-input registration per device class per process (a second one silently
 * steals delivery from the first -- see src/interceptor.c's file header and
 * InterceptorEnumerateExistingDevices for the registration this shares), so
 * anything that wants to pick up motion from mice it is not filtering should
 * use this rather than RegisterRawInputDevices of its own.
 */
InterceptorFilter INTERCEPTOR_API interceptor_get_monitor(InterceptorContext context, InterceptorDevice device);

void INTERCEPTOR_API interceptor_set_monitor(InterceptorContext context, InterceptorPredicate predicate, InterceptorFilter filter);

InterceptorDevice INTERCEPTOR_API interceptor_wait(InterceptorContext context);

InterceptorDevice INTERCEPTOR_API interceptor_wait_with_timeout(InterceptorContext context, unsigned long milliseconds);

int INTERCEPTOR_API interceptor_send(InterceptorContext context, InterceptorDevice device, const InterceptorStroke *stroke, unsigned int nstroke);

int INTERCEPTOR_API interceptor_receive(InterceptorContext context, InterceptorDevice device, InterceptorStroke *stroke, unsigned int nstroke);

unsigned int INTERCEPTOR_API interceptor_get_hardware_id(InterceptorContext context, InterceptorDevice device, void *hardware_id_buffer, unsigned int buffer_size);

int INTERCEPTOR_API interceptor_is_invalid(InterceptorDevice device);

int INTERCEPTOR_API interceptor_is_keyboard(InterceptorDevice device);

int INTERCEPTOR_API interceptor_is_mouse(InterceptorDevice device);

#ifdef __cplusplus
}
#endif

#endif
