#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <CommCtrl.h>
#include <intrin.h>

#define MIN_CPS 1
#define MAX_CPS 1000
#define DEFAULT_CPS 1000
#define MIN_WINDOW_WIDTH 560
#define MIN_WINDOW_HEIGHT 510
#define PRECISE_SPIN_MAX_US 200
#define ID_APPLY 100
#define ID_JITTER_CHECK 101
#define ID_CPS_SLIDER 102
#define ID_CPS_EDIT 103
#define ID_INPUT_MODE_CHECK 104
#define ID_HOTKEY_LEFT 201
#define ID_HOTKEY_RIGHT 202
#define ID_HOTKEY_EXIT 203
#define CLICK_NONE 0
#define CLICK_LEFT 1
#define CLICK_RIGHT 2
#define EXIT_HOTKEY_VK VK_F9

__declspec(noreturn) void WINAPI win_main_crt_startup(void);

#define UI_BG RGB(18, 18, 22)
#define UI_INPUT_BG RGB(31, 31, 38)
#define UI_TEXT RGB(245, 245, 247)

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static volatile LONG g_running = 1;
static volatile LONG g_click_button;
static volatile LONG g_jitter_on;
static volatile LONG g_cps = DEFAULT_CPS;
static volatile LONG g_si = 1;

static volatile LONG g_hk_left = MAKEWORD(VK_F6, 0);
static volatile LONG g_hk_right = MAKEWORD(VK_F7, 0);

static HANDLE g_wake_event;
static INPUT g_left_click[2] = {
    { INPUT_MOUSE, { { 0, 0, 0, MOUSEEVENTF_LEFTDOWN, 0, 0 } } },
    { INPUT_MOUSE, { { 0, 0, 0, MOUSEEVENTF_LEFTUP, 0, 0 } } }
};
static INPUT g_right_click[2] = {
    { INPUT_MOUSE, { { 0, 0, 0, MOUSEEVENTF_RIGHTDOWN, 0, 0 } } },
    { INPUT_MOUSE, { { 0, 0, 0, MOUSEEVENTF_RIGHTUP, 0, 0 } } }
};
static const INPUT g_jitter_move_template = { INPUT_MOUSE, { { 0, 0, 0, MOUSEEVENTF_MOVE, 0, 0 } } };

static HWND g_label_left;
static HWND g_label_right;
static HWND g_label_jitter;
static HWND g_label_input_mode;
static HWND g_label_low_cpu;
static HWND g_label_cps;
static HWND g_label_exit;
static HWND g_hk_left_hwnd;
static HWND g_hk_right_hwnd;
static HWND g_exit_value_hwnd;
static HWND g_jitter_check_hwnd;
static HWND g_input_mode_check_hwnd;
static HWND g_low_cpu_value_hwnd;
static HWND g_cps_slider_hwnd;
static HWND g_cps_edit_hwnd;
static HWND g_apply_hwnd;
static HFONT g_ui_font;
static HBRUSH g_bg_brush;
static HBRUSH g_input_brush;
static int g_cps_ui;
static int g_owns_ui_font;

static LONG read_flag(volatile LONG* value) {
    return *value;
}

static void write_flag(volatile LONG* value, LONG new_value) {
    InterlockedExchange((volatile LONG*)value, new_value);
}

static int is_clicking_enabled(void) {
    return read_flag(&g_click_button) != CLICK_NONE;
}

static LONG clamp_cps(LONG cps) {
    return cps < MIN_CPS ? MIN_CPS : (cps > MAX_CPS ? MAX_CPS : cps);
}

static WCHAR* append_uint(WCHAR* out, DWORD value) {
    WCHAR* p;
    DWORD temp = value;
    int digits = 1;

    while (temp >= 10u) {
        ++digits;
        temp /= 10u;
    }
    p = out + digits;
    do {
        *--p = (WCHAR)(L'0' + (value % 10u));
        value /= 10u;
    } while (value);
    return out + digits;
}

static void set_cps_edit_text(LONG cps) {
    WCHAR buffer[12];
    WCHAR* out = append_uint(buffer, (DWORD)cps);
    *out = 0;
    SendMessageW(g_cps_edit_hwnd, WM_SETTEXT, 0, (LPARAM)buffer);
}

static LONG parse_cps_text(HWND hwnd, int* clamped) {
    WCHAR buffer[16];
    LRESULT length;
    LONG value = 0;
    int seen_digit = 0;
    int i;

    *clamped = 0;
    length = SendMessageW(hwnd, WM_GETTEXT, (WPARAM)(sizeof(buffer) / sizeof(buffer[0])), (LPARAM)buffer);
    if (length <= 0) {
        return 0;
    }

    for (i = 0; i < (int)length; ++i) {
        WCHAR ch = buffer[i];
        if (ch < L'0' || ch > L'9') {
            continue;
        }
        seen_digit = 1;
        value = (value * 10) + (LONG)(ch - L'0');
        if (value > MAX_CPS) {
            *clamped = 1;
            return MAX_CPS;
        }
    }
    if (!seen_digit) {
        return -1;
    }
    if (value < MIN_CPS) {
        *clamped = 1;
        return MIN_CPS;
    }
    return value;
}

static void set_target_cps(LONG cps, int update_slider, int update_edit) {
    cps = clamp_cps(cps);
    write_flag(&g_cps, cps);
    if (g_wake_event) {
        SetEvent(g_wake_event);
    }

    if (g_cps_ui) {
        return;
    }

    g_cps_ui = 1;
    if (update_slider && g_cps_slider_hwnd) {
        SendMessageW(g_cps_slider_hwnd, TBM_SETPOS, TRUE, cps);
    }
    if (update_edit && g_cps_edit_hwnd) {
        set_cps_edit_text(cps);
    }
    g_cps_ui = 0;
}

static __forceinline DWORD xor_shift32(DWORD* state) {
    DWORD x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static __forceinline LONG jitter_delta(DWORD* state) {
    return (LONG)(((ULONGLONG)xor_shift32(state) * 3u) >> 32) - 1;
}

static void advance_period(LONGLONG* counter, LONGLONG base, LONGLONG remainder, LONGLONG* carry, LONG cps) {
    *counter += base;
    *carry += remainder;
    if (*carry >= cps) {
        ++*counter;
        *carry -= cps;
    }
}

static LONGLONG counter_from_microseconds(const LARGE_INTEGER* frequency, LONGLONG microseconds) {
    return ((frequency->QuadPart * microseconds) + 999999) / 1000000;
}

static LONGLONG counter_to_100ns_ceil(LONGLONG counter_ticks, const LARGE_INTEGER* frequency) {
    return ((counter_ticks * 10000000) + frequency->QuadPart - 1) / frequency->QuadPart;
}

static LONGLONG timing_spin_guard(LONGLONG period_base, const LARGE_INTEGER* frequency) {
    LONGLONG max_guard = counter_from_microseconds(frequency, PRECISE_SPIN_MAX_US);
    LONGLONG period_guard = period_base / 4;

    if (period_guard < 1) {
        period_guard = 1;
    }
    return period_guard < max_guard ? period_guard : max_guard;
}

static void post_window_click(int left_button) {
    POINT point;
    HWND target;
    UINT down_msg;
    UINT up_msg;
    WPARAM down_wparam;
    LPARAM lparam;

    if (!GetCursorPos(&point)) {
        return;
    }
    target = WindowFromPoint(point);
    if (!target) {
        return;
    }
    ScreenToClient(target, &point);
    lparam = MAKELPARAM(point.x, point.y);

    if (left_button) {
        down_msg = WM_LBUTTONDOWN;
        up_msg = WM_LBUTTONUP;
        down_wparam = MK_LBUTTON;
    } else {
        down_msg = WM_RBUTTONDOWN;
        up_msg = WM_RBUTTONUP;
        down_wparam = MK_RBUTTON;
    }

    PostMessageW(target, down_msg, down_wparam, lparam);
    PostMessageW(target, up_msg, 0, lparam);
}

static int wait_precise_counter(HANDLE timer, const HANDLE* handles, LONGLONG target, const LARGE_INTEGER* frequency, LONGLONG spin_guard) {
    LARGE_INTEGER now;
    LARGE_INTEGER due;
    LONGLONG remaining;
    LONGLONG timer_ticks;
    DWORD wait_result;

    for (;;) {
        if (!read_flag(&g_running) || !is_clicking_enabled()) {
            return 0;
        }

        QueryPerformanceCounter(&now);
        if (now.QuadPart >= target) {
            return 1;
        }

        remaining = target - now.QuadPart;

        if (remaining <= spin_guard) {
            do {
                YieldProcessor();
                if (!read_flag(&g_running) || !is_clicking_enabled()) {
                    return 0;
                }
                QueryPerformanceCounter(&now);
            } while (now.QuadPart < target);
            return 1;
        }

        timer_ticks = remaining - spin_guard;
        due.QuadPart = -counter_to_100ns_ceil(timer_ticks, frequency);
        if (due.QuadPart == 0) {
            due.QuadPart = -1;
        }

        if (!SetWaitableTimer(timer, &due, 0, 0, 0, FALSE)) {
            Sleep(1);
            return 0;
        }

        wait_result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wait_result == WAIT_OBJECT_0 + 1) {
            return 0;
        }
        if (wait_result != WAIT_OBJECT_0) {
            return 0;
        }
    }
}

static DWORD WINAPI clicker_thread_proc(void* unused) {
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    HANDLE low_cpu_timer;
    HANDLE wait_handles[2];
    INPUT left_batch[3];
    INPUT right_batch[3];
    DWORD rng_state;
    LONGLONG next_counter;
    LONGLONG period_base;
    LONGLONG period_remainder;
    LONGLONG period_carry;
    LONGLONG spin_guard;
    LONG active_cps;
    int priority_boosted;

    (void)unused;
    QueryPerformanceFrequency(&frequency);
    low_cpu_timer = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!low_cpu_timer) {
        low_cpu_timer = CreateWaitableTimerW(0, TRUE, 0);
    }
    wait_handles[0] = low_cpu_timer;
    wait_handles[1] = g_wake_event;

    left_batch[0] = g_jitter_move_template;
    left_batch[1] = g_left_click[0];
    left_batch[2] = g_left_click[1];
    right_batch[0] = g_jitter_move_template;
    right_batch[1] = g_right_click[0];
    right_batch[2] = g_right_click[1];

    period_base = frequency.QuadPart / DEFAULT_CPS;
    period_remainder = frequency.QuadPart % DEFAULT_CPS;
    period_carry = 0;
    spin_guard = timing_spin_guard(period_base, &frequency);
    active_cps = DEFAULT_CPS;
    priority_boosted = 0;
    rng_state = (DWORD)GetTickCount64() ^ (GetCurrentProcessId() << 16) ^ GetCurrentThreadId();
    if (!rng_state) {
        rng_state = 0x9e3779b9u;
    }

    while (read_flag(&g_running)) {
        while (read_flag(&g_running) && !is_clicking_enabled()) {
            if (priority_boosted) {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
                priority_boosted = 0;
            }
            WaitForSingleObject(g_wake_event, INFINITE);
        }
        if (!read_flag(&g_running)) {
            break;
        }

        if (!priority_boosted) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            priority_boosted = 1;
        }

        QueryPerformanceCounter(&counter);
        next_counter = counter.QuadPart;
        period_carry = 0;

        while (read_flag(&g_running) && is_clicking_enabled()) {
            int left_button;
            LONG click_button;
            LONGLONG batch_counter;
            LONGLONG batch_carry;
            LONGLONG wait_counter;
            LONG cps;
            LONG jitter;
            LONG use_sendinput;
            INPUT* click;

            click_button = read_flag(&g_click_button);
            if (click_button == CLICK_NONE) {
                break;
            }
            left_button = click_button == CLICK_LEFT;

            cps = read_flag(&g_cps);
            if (cps != active_cps) {
                active_cps = cps;
                period_base = frequency.QuadPart / active_cps;
                period_remainder = frequency.QuadPart % active_cps;
                period_carry = 0;
                spin_guard = timing_spin_guard(period_base, &frequency);
                QueryPerformanceCounter(&counter);
                next_counter = counter.QuadPart;
                advance_period(&next_counter, period_base, period_remainder, &period_carry, active_cps);
            }

            click = left_button ? g_left_click : g_right_click;
            use_sendinput = read_flag(&g_si);
            wait_counter = next_counter;

            QueryPerformanceCounter(&counter);
            if (counter.QuadPart < wait_counter) {
                if (low_cpu_timer) {
                    if (!wait_precise_counter(low_cpu_timer, wait_handles, wait_counter, &frequency, spin_guard)) {
                        continue;
                    }
                } else {
                    Sleep(1);
                }
                QueryPerformanceCounter(&counter);
            }

            batch_counter = next_counter;
            batch_carry = period_carry;
            advance_period(&batch_counter, period_base, period_remainder, &batch_carry, active_cps);
            if (batch_counter <= counter.QuadPart) {
                batch_counter = counter.QuadPart;
                batch_carry = 0;
                advance_period(&batch_counter, period_base, period_remainder, &batch_carry, active_cps);
            }

            jitter = read_flag(&g_jitter_on);
            if (jitter) {
                INPUT* batch = left_button ? left_batch : right_batch;
                batch[0].mi.dx = jitter_delta(&rng_state);
                batch[0].mi.dy = jitter_delta(&rng_state);
                if (use_sendinput) {
                    SendInput(3u, batch, sizeof(INPUT));
                } else {
                    SendInput(1u, batch, sizeof(INPUT));
                    post_window_click(left_button);
                    SwitchToThread();
                }
            } else {
                if (use_sendinput) {
                    SendInput(2u, click, sizeof(INPUT));
                } else {
                    post_window_click(left_button);
                    SwitchToThread();
                }
            }
            next_counter = batch_counter;
            period_carry = batch_carry;
        }
    }
    if (priority_boosted) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    }
    if (low_cpu_timer) {
        CloseHandle(low_cpu_timer);
    }
    return 0;
}

static WORD read_hotkey(HWND hwnd, WORD fallback) {
    WORD hotkey = (WORD)SendMessageW(hwnd, HKM_GETHOTKEY, 0, 0);
    return LOBYTE(hotkey) ? hotkey : fallback;
}

static void apply_font(HWND hwnd) {
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    }
}

static void show_window_in_front(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);
}

static __forceinline void toggle_click_button(LONG button) {
    write_flag(&g_click_button, read_flag(&g_click_button) == button ? CLICK_NONE : button);
    SetEvent(g_wake_event);
}

static UINT hotkey_mods(WORD hotkey) {
    BYTE flags = HIBYTE(hotkey);
    return MOD_NOREPEAT |
        ((flags & HOTKEYF_CONTROL) ? MOD_CONTROL : 0) |
        ((flags & HOTKEYF_SHIFT) ? MOD_SHIFT : 0) |
        ((flags & HOTKEYF_ALT) ? MOD_ALT : 0);
}

static void unregister_app_hotkeys(HWND hwnd) {
    UnregisterHotKey(hwnd, ID_HOTKEY_LEFT);
    UnregisterHotKey(hwnd, ID_HOTKEY_RIGHT);
    UnregisterHotKey(hwnd, ID_HOTKEY_EXIT);
}

static int register_one_hotkey(HWND hwnd, int id, WORD hotkey) {
    UINT vk = LOBYTE(hotkey);
    return vk && RegisterHotKey(hwnd, id, hotkey_mods(hotkey), vk);
}

static int register_app_hotkeys(HWND hwnd, WORD left, WORD right) {
    if (register_one_hotkey(hwnd, ID_HOTKEY_LEFT, left) &&
        register_one_hotkey(hwnd, ID_HOTKEY_RIGHT, right) &&
        RegisterHotKey(hwnd, ID_HOTKEY_EXIT, MOD_NOREPEAT, EXIT_HOTKEY_VK)) {
        return 1;
    }
    unregister_app_hotkeys(hwnd);
    return 0;
}

static void layout_controls(HWND hwnd) {
    RECT rc;
    int width;
    int margin_x = 28;
    int label_w = 176;
    int gap = 14;
    int hotkey_x;
    int hotkey_w;
    int edit_w = 86;
    int slider_w;
    int button_w;
    int button_x;
    int y;

    GetClientRect(hwnd, &rc);
    width = rc.right - rc.left;
    hotkey_x = margin_x + label_w + gap;
    hotkey_w = width > hotkey_x + margin_x ? width - hotkey_x - margin_x : 150;

    y = 26;
    MoveWindow(g_label_left, margin_x, y + 4, label_w, 22, TRUE);
    MoveWindow(g_hk_left_hwnd, hotkey_x, y, hotkey_w, 28, TRUE);
    y += 44;

    MoveWindow(g_label_right, margin_x, y + 4, label_w, 22, TRUE);
    MoveWindow(g_hk_right_hwnd, hotkey_x, y, hotkey_w, 28, TRUE);
    y += 44;

    MoveWindow(g_label_jitter, margin_x, y + 4, label_w, 22, TRUE);
    MoveWindow(g_jitter_check_hwnd, hotkey_x, y + 2, hotkey_w, 26, TRUE);
    y += 48;

    MoveWindow(g_label_input_mode, margin_x, y + 4, label_w, 22, TRUE);
    MoveWindow(g_input_mode_check_hwnd, hotkey_x, y + 2, hotkey_w, 26, TRUE);
    y += 48;

    MoveWindow(g_label_low_cpu, margin_x, y + 4, label_w, 22, TRUE);
    MoveWindow(g_low_cpu_value_hwnd, hotkey_x, y + 2, hotkey_w, 26, TRUE);
    y += 48;

    slider_w = hotkey_w - edit_w - gap;
    if (slider_w < 160) {
        slider_w = 160;
    }
    MoveWindow(g_label_cps, margin_x, y + 6, label_w, 22, TRUE);
    MoveWindow(g_cps_slider_hwnd, hotkey_x, y, slider_w, 34, TRUE);
    MoveWindow(g_cps_edit_hwnd, hotkey_x + slider_w + gap, y + 2, edit_w, 28, TRUE);
    y += 52;

    MoveWindow(g_label_exit, margin_x, y + 4, label_w, 22, TRUE);
    MoveWindow(g_exit_value_hwnd, hotkey_x, y + 4, hotkey_w, 22, TRUE);
    y += 58;

    button_w = width > 270 ? 220 : width - margin_x * 2;
    button_x = (width - button_w) / 2;
    MoveWindow(g_apply_hwnd, button_x, y, button_w, 38, TRUE);
}

static HWND create_label(HWND parent, HINSTANCE instance, LPCWSTR text) {
    HWND hwnd = CreateWindowW(
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE,
        0,
        0,
        0,
        0,
        parent,
        0,
        instance,
        0);
    apply_font(hwnd);
    return hwnd;
}

static HWND create_hotkey(HWND parent, HINSTANCE instance, WORD hotkey) {
    HWND hwnd = CreateWindowW(
        HOTKEY_CLASSW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER,
        0,
        0,
        0,
        0,
        parent,
        0,
        instance,
        0);
    apply_font(hwnd);
    SendMessageW(hwnd, HKM_SETHOTKEY, hotkey, 0);
    return hwnd;
}

static HWND create_button(HWND parent, HINSTANCE instance, LPCWSTR text, DWORD style, int id) {
    HWND hwnd = CreateWindowW(
        L"BUTTON",
        text,
        WS_CHILD | WS_VISIBLE | style,
        0,
        0,
        0,
        0,
        parent,
        (HMENU)(INT_PTR)id,
        instance,
        0);
    apply_font(hwnd);
    return hwnd;
}

static void delete_ui_objects(void) {
    if (g_owns_ui_font && g_ui_font) {
        DeleteObject(g_ui_font);
    }
    if (g_bg_brush) {
        DeleteObject(g_bg_brush);
    }
    if (g_input_brush) {
        DeleteObject(g_input_brush);
    }
    g_ui_font = 0;
    g_bg_brush = 0;
    g_input_brush = 0;
    g_owns_ui_font = 0;
}

static int fail_window(HWND hwnd) {
    DestroyWindow(hwnd);
    CloseHandle(g_wake_event);
    return 1;
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_APPLY) {
                WORD old_left = (WORD)read_flag(&g_hk_left);
                WORD old_right = (WORD)read_flag(&g_hk_right);
                WORD new_left = read_hotkey(g_hk_left_hwnd, old_left);
                WORD new_right = read_hotkey(g_hk_right_hwnd, old_right);

                write_flag(&g_click_button, CLICK_NONE);
                SetEvent(g_wake_event);

                unregister_app_hotkeys(hwnd);
                if (register_app_hotkeys(hwnd, new_left, new_right)) {
                    write_flag(&g_hk_left, new_left);
                    write_flag(&g_hk_right, new_right);
                    MessageBoxW(hwnd, L"Hotkeys updated successfully.", L"CoreAutoclicker", MB_OK);
                } else {
                    register_app_hotkeys(hwnd, old_left, old_right);
                    SendMessageW(g_hk_left_hwnd, HKM_SETHOTKEY, old_left, 0);
                    SendMessageW(g_hk_right_hwnd, HKM_SETHOTKEY, old_right, 0);
                    MessageBoxW(hwnd, L"That hotkey is unavailable.", L"CoreAutoclicker", MB_OK);
                }
            } else if (LOWORD(wParam) == ID_JITTER_CHECK) {
                write_flag(&g_jitter_on, SendMessageW(g_jitter_check_hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
            } else if (LOWORD(wParam) == ID_INPUT_MODE_CHECK) {
                write_flag(&g_si, SendMessageW(g_input_mode_check_hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SetEvent(g_wake_event);
            } else if (LOWORD(wParam) == ID_CPS_EDIT && HIWORD(wParam) == EN_CHANGE && !g_cps_ui) {
                int clamped;
                LONG cps = parse_cps_text(g_cps_edit_hwnd, &clamped);
                if (cps >= 0) {
                    set_target_cps(cps, 1, clamped);
                }
            }
            return 0;

        case WM_HOTKEY:
            if (wParam == ID_HOTKEY_LEFT) {
                toggle_click_button(CLICK_LEFT);
            } else if (wParam == ID_HOTKEY_RIGHT) {
                toggle_click_button(CLICK_RIGHT);
            } else if (wParam == ID_HOTKEY_EXIT) {
                write_flag(&g_running, 0);
                SetEvent(g_wake_event);
                PostQuitMessage(0);
            }
            return 0;

        case WM_HSCROLL:
            if ((HWND)lParam == g_cps_slider_hwnd) {
                LONG cps = (LONG)SendMessageW(g_cps_slider_hwnd, TBM_GETPOS, 0, 0);
                set_target_cps(cps, 0, 1);
                return 0;
            }
            return 0;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        {
            HDC hdc = (HDC)wParam;
            HBRUSH brush = g_bg_brush ? g_bg_brush : GetSysColorBrush(COLOR_WINDOW);
            SetTextColor(hdc, UI_TEXT);
            SetBkColor(hdc, UI_BG);
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)brush;
        }

        case WM_CTLCOLOREDIT:
        {
            HDC hdc = (HDC)wParam;
            HBRUSH brush = g_input_brush ? g_input_brush : GetSysColorBrush(COLOR_WINDOW);
            SetTextColor(hdc, UI_TEXT);
            SetBkColor(hdc, UI_INPUT_BG);
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)brush;
        }

        case WM_SIZE:
            layout_controls(hwnd);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* info = (MINMAXINFO*)lParam;
            info->ptMinTrackSize.x = MIN_WINDOW_WIDTH;
            info->ptMinTrackSize.y = MIN_WINDOW_HEIGHT;
            return 0;
        }

        case WM_DESTROY:
            write_flag(&g_running, 0);
            SetEvent(g_wake_event);
            unregister_app_hotkeys(hwnd);
            delete_ui_objects();
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static int app_main(void) {
    HINSTANCE instance = GetModuleHandleW(0);
    INITCOMMONCONTROLSEX icex = {0};
    WNDCLASSW wc = {0};
    HWND hwnd;
    HANDLE clicker_thread;
    MSG msg = {0};

    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_HOTKEY_CLASS | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    g_wake_event = CreateEventW(0, FALSE, FALSE, 0);
    if (!g_wake_event) {
        return 1;
    }
    g_bg_brush = CreateSolidBrush(UI_BG);
    g_input_brush = CreateSolidBrush(UI_INPUT_BG);

    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(0, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = g_bg_brush ? g_bg_brush : (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"coreautoclicker";
    RegisterClassW(&wc);

    hwnd = CreateWindowExW(
        0,
        L"coreautoclicker",
        L"CoreAutoclicker",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        MIN_WINDOW_WIDTH,
        MIN_WINDOW_HEIGHT,
        0,
        0,
        instance,
        0);
    if (!hwnd) {
        delete_ui_objects();
        CloseHandle(g_wake_event);
        return 1;
    }

    g_ui_font = CreateFontW(
        -15,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH,
        L"Comic Sans MS");
    if (!g_ui_font) {
        g_ui_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    } else {
        g_owns_ui_font = 1;
    }

    g_label_left = create_label(hwnd, instance, L"Left Click Toggle:");
    g_hk_left_hwnd = create_hotkey(hwnd, instance, (WORD)read_flag(&g_hk_left));
    g_label_right = create_label(hwnd, instance, L"Right Click Toggle:");
    g_hk_right_hwnd = create_hotkey(hwnd, instance, (WORD)read_flag(&g_hk_right));
    g_label_jitter = create_label(hwnd, instance, L"Jitter Enabled:");
    g_jitter_check_hwnd = create_button(hwnd, instance, L"", BS_AUTOCHECKBOX, ID_JITTER_CHECK);
    g_label_input_mode = create_label(hwnd, instance, L"Universal SendInput:");
    g_input_mode_check_hwnd = create_button(hwnd, instance, L"", BS_AUTOCHECKBOX, ID_INPUT_MODE_CHECK);
    SendMessageW(g_input_mode_check_hwnd, BM_SETCHECK, BST_CHECKED, 0);
    g_label_low_cpu = create_label(hwnd, instance, L"Low CPU mode:");
    g_low_cpu_value_hwnd = create_label(hwnd, instance, L"Always on");
    g_label_cps = create_label(hwnd, instance, L"Clicks per second:");
    g_cps_slider_hwnd = CreateWindowW(
        TRACKBAR_CLASSW,
        L"",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        0,
        0,
        0,
        0,
        hwnd,
        (HMENU)(INT_PTR)ID_CPS_SLIDER,
        instance,
        0);
    SendMessageW(g_cps_slider_hwnd, TBM_SETRANGE, TRUE, MAKELPARAM(MIN_CPS, MAX_CPS));
    SendMessageW(g_cps_slider_hwnd, TBM_SETPAGESIZE, 0, 100);
    g_cps_edit_hwnd = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
        0,
        0,
        0,
        0,
        hwnd,
        (HMENU)(INT_PTR)ID_CPS_EDIT,
        instance,
        0);
    apply_font(g_cps_edit_hwnd);
    set_target_cps(DEFAULT_CPS, 1, 1);
    g_label_exit = create_label(hwnd, instance, L"Emergency close:");
    g_exit_value_hwnd = create_label(hwnd, instance, L"F9 (fixed)");

    g_apply_hwnd = create_button(hwnd, instance, L"Apply Hotkeys", BS_PUSHBUTTON, ID_APPLY);

    layout_controls(hwnd);
    show_window_in_front(hwnd);

    if (!register_app_hotkeys(hwnd, (WORD)read_flag(&g_hk_left), (WORD)read_flag(&g_hk_right))) {
        return fail_window(hwnd);
    }

    clicker_thread = CreateThread(0, 0, clicker_thread_proc, 0, 0, 0);
    if (!clicker_thread) {
        return fail_window(hwnd);
    }

    while (GetMessageW(&msg, 0, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    write_flag(&g_running, 0);
    SetEvent(g_wake_event);

    WaitForSingleObject(clicker_thread, INFINITE);
    CloseHandle(clicker_thread);
    CloseHandle(g_wake_event);

    return (int)msg.wParam;
}

__declspec(noreturn) void WINAPI win_main_crt_startup(void) {
    ExitProcess((UINT)app_main());
}
