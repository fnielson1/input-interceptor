/*
 * utils.c -- see utils.h. Original implementation; no code or structure
 * borrowed from anywhere in this repository's samples/ tree (see
 * SAMPLES-NOTES.md).
 */

#include "utils.h"

#include <windows.h>
#include <time.h>
#include <string.h>

void raise_process_priority(void)
{
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
}

void lower_process_priority(void)
{
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
}

int get_screen_width(void)
{
    return GetSystemMetrics(SM_CXSCREEN);
}

int get_screen_height(void)
{
    return GetSystemMetrics(SM_CYSCREEN);
}

/*
 * busy_wait's whole job is a loop with no observable side effect other than
 * consuming time, so an optimizing compiler is entitled to delete it
 * entirely. calculate_busy_wait_millisecond's calibration loop has the same
 * problem. Both are bracketed in a single optimize-off pragma rather than
 * marking the loop counters volatile, matching this project's established
 * technique for "don't let the optimizer eliminate this delay loop".
 */
#pragma optimize("", off)

void busy_wait(unsigned long count)
{
    while (count != 0)
    {
        count--;
    }
}

unsigned long calculate_busy_wait_millisecond(void)
{
    const unsigned long calibration_count = 2000000000UL;
    unsigned long counter;
    time_t start_time;
    time_t end_time;
    double elapsed_seconds;

    start_time = time(NULL);

    counter = calibration_count;
    while (counter != 0)
    {
        counter--;
    }

    end_time = time(NULL);
    elapsed_seconds = difftime(end_time, start_time);

    /* A calibration run finishing inside the same wall-clock second would
       otherwise divide by zero; treat that as "at least one second" so the
       result stays a sane (if conservatively low) iterations-per-ms
       estimate rather than blowing up. */
    if (elapsed_seconds < 1.0)
        elapsed_seconds = 1.0;

    return (unsigned long)((double)calibration_count / (elapsed_seconds * 1000.0));
}

#pragma optimize("", on)

void *try_open_single_program(const char *name)
{
    char mutex_name[256];
    size_t name_length;
    HANDLE mutex_handle;

    if (name == NULL)
        return NULL;

    /* "Global\" is 7 characters; keep a byte for the NUL terminator and
       silently truncate anything absurdly long rather than overflow. */
    name_length = strlen(name);
    if (name_length > sizeof(mutex_name) - 8)
        name_length = sizeof(mutex_name) - 8;

    memcpy(mutex_name, "Global\\", 7);
    memcpy(mutex_name + 7, name, name_length);
    mutex_name[7 + name_length] = '\0';

    mutex_handle = CreateMutexA(NULL, FALSE, mutex_name);
    if (mutex_handle == NULL)
        return NULL;

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mutex_handle);
        return NULL;
    }

    return (void *)mutex_handle;
}

void close_single_program(void *program_instance)
{
    if (program_instance != NULL)
        CloseHandle((HANDLE)program_instance);
}
