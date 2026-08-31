#ifndef _INTERCEPTOR_DRIVER_PROTOCOL_H_
#define _INTERCEPTOR_DRIVER_PROTOCOL_H_

/*
 * User-mode wire protocol for \\.\interceptionNN.
 *
 * This is fixed by src/interceptor.c in this repository, which remains the
 * single source of truth for the DLL side -- nothing here may drift from
 * it. The IOCTL codes below are the driver-side mirror of the same
 * CTL_CODE(...) macros interceptor.c already computes; the filter bit
 * values are copied from src/interceptor.h's InterceptorFilterKeyState /
 * InterceptorFilterMouseState enums. Keep both in lockstep by hand if the
 * DLL side ever changes -- there is deliberately no shared header between
 * user mode and kernel mode, since interceptor.h pulls in dllimport/
 * dllexport declarations that have no meaning here.
 */

#define IOCTL_INTERCEPTOR_SET_PRECEDENCE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_INTERCEPTOR_GET_PRECEDENCE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_INTERCEPTOR_SET_FILTER      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_INTERCEPTOR_GET_FILTER      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_INTERCEPTOR_SET_EVENT       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_INTERCEPTOR_WRITE           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_INTERCEPTOR_READ            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x840, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_INTERCEPTOR_GET_HARDWARE_ID CTL_CODE(FILE_DEVICE_UNKNOWN, 0x880, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* InterceptorFilterKeyState, from src/interceptor.h. */
#define INTERCEPTOR_FILTER_KEY_NONE             0x0000
#define INTERCEPTOR_FILTER_KEY_ALL               0xFFFF
#define INTERCEPTOR_FILTER_KEY_DOWN              0x0001
#define INTERCEPTOR_FILTER_KEY_UP                0x0002
#define INTERCEPTOR_FILTER_KEY_E0                0x0004
#define INTERCEPTOR_FILTER_KEY_E1                0x0008
#define INTERCEPTOR_FILTER_KEY_TERMSRV_SET_LED   0x0010
#define INTERCEPTOR_FILTER_KEY_TERMSRV_SHADOW    0x0020
#define INTERCEPTOR_FILTER_KEY_TERMSRV_VKPACKET  0x0040

/* InterceptorFilterMouseState, from src/interceptor.h -- the button/wheel
   bits are numerically identical to the raw MOUSE_INPUT_DATA.ButtonFlags
   bits (MOUSE_LEFT_BUTTON_DOWN etc. in <ntddmou.h>); FILTER_MOUSE_MOVE has
   no raw counterpart and is synthesised, see queue.c. */
#define INTERCEPTOR_FILTER_MOUSE_NONE               0x0000
#define INTERCEPTOR_FILTER_MOUSE_ALL                0xFFFF
#define INTERCEPTOR_FILTER_MOUSE_LEFT_BUTTON_DOWN   0x0001
#define INTERCEPTOR_FILTER_MOUSE_LEFT_BUTTON_UP     0x0002
#define INTERCEPTOR_FILTER_MOUSE_RIGHT_BUTTON_DOWN  0x0004
#define INTERCEPTOR_FILTER_MOUSE_RIGHT_BUTTON_UP    0x0008
#define INTERCEPTOR_FILTER_MOUSE_MIDDLE_BUTTON_DOWN 0x0010
#define INTERCEPTOR_FILTER_MOUSE_MIDDLE_BUTTON_UP   0x0020
#define INTERCEPTOR_FILTER_MOUSE_BUTTON_4_DOWN      0x0040
#define INTERCEPTOR_FILTER_MOUSE_BUTTON_4_UP        0x0080
#define INTERCEPTOR_FILTER_MOUSE_BUTTON_5_DOWN      0x0100
#define INTERCEPTOR_FILTER_MOUSE_BUTTON_5_UP        0x0200
#define INTERCEPTOR_FILTER_MOUSE_WHEEL              0x0400
#define INTERCEPTOR_FILTER_MOUSE_HWHEEL             0x0800
#define INTERCEPTOR_FILTER_MOUSE_MOVE                0x1000

#endif
