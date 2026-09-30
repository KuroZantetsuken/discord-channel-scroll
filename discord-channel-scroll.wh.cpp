// ==WindhawkMod==
// @id              discord-channel-scroll
// @name            Discord Channel Scroll
// @description     Hold the right mouse button and scroll to switch Discord channels
// @version         1.2.0
// @author          Kuro
// @include         Discord.exe
// @architecture    x86
// @architecture    amd64
// @architecture    arm64
// @compilerOptions -lshell32
// @license         GPL-3.0-or-later
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Discord Channel Scroll

Hold the **right mouse button** and use the vertical mouse wheel:

- **Scroll up:** Switch to the previous channel.
- **Scroll down:** Switch to the next channel.

RMB input is otherwise untouched. The mod checks the button state recorded in
each vertical wheel event. Without RMB, the event passes through unchanged.
With RMB, it consumes the wheel and queues the channel switch immediately, including
the first scroll of each hold. There is no hold timer or synthetic mouse release.

After RMB+scroll, the right-button release is consumed inside Discord, after
Windows has processed the real input. This prevents a release-triggered context
menu without leaving the system's button state stuck. Ordinary right-clicks
and horizontal scrolling are unchanged.

An element that opens a custom menu on button-down has already done so before
the wheel moves; that cannot be prevented while leaving ordinary presses alone.

RMB is never claimed while another physical mouse button is held. This keeps
mouse-button window-drag gestures, including PowerToys FancyZones'
non-primary-button activation, untouched.

Discord's own `Alt+Up Arrow` and `Alt+Down Arrow` commands perform the actual
channel switch, avoiding hooks into version-specific Discord or Electron
internals. Channel ordering and behavior at the first and last channel follow
Discord's native shortcuts.
*/
// ==/WindhawkModReadme==

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <atomic>
#include <limits>

namespace
{

    constexpr UINT kSwitchToPreviousChannelMessage = WM_APP + 1;
    constexpr UINT kSwitchToNextChannelMessage = WM_APP + 2;
    constexpr short kWheelDelta = WHEEL_DELTA;

    enum class ChannelDirection : WPARAM
    {
        Previous = 1,
        Next = 2,
    };

    struct ThreadStartup
    {
        HANDLE readyEvent;
        BOOL succeeded;
    };

    std::atomic<DWORD> g_switchGestureGeneration{0};
    std::atomic<DWORD> g_nextGestureGeneration{0};
    std::atomic_bool g_uninitializing{false};

    HANDLE g_workerThread = nullptr;
    DWORD g_workerThreadId = 0;

    // Message queues belong to threads. Never share partially accumulated
    // wheel input or release suppression between different Discord UI threads.
    thread_local bool g_channelScrollGestureActive = false;
    thread_local DWORD g_rmbGestureGeneration = 0;
    thread_local int g_wheelRemainder = 0;
    thread_local HWND g_gestureWindow = nullptr;

    decltype(&PeekMessageW) PeekMessageW_Original = nullptr;

    bool IsDiscordProcess()
    {
        int argumentCount = 0;
        LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        if (!arguments)
        {
            return true;
        }

        bool isDiscordProcess = true;
        for (int i = 1; i < argumentCount; ++i)
        {
            if (_wcsnicmp(arguments[i], L"--type=", 7) == 0)
            {
                isDiscordProcess = false;
                break;
            }
        }

        LocalFree(arguments);
        return isDiscordProcess;
    }

    HWND GetForegroundWindowForThisProcess()
    {
        HWND window = GetForegroundWindow();
        if (!window)
        {
            return nullptr;
        }

        window = GetAncestor(window, GA_ROOT);
        if (!window)
        {
            return nullptr;
        }

        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        return processId == GetCurrentProcessId() ? window : nullptr;
    }

    bool IsPointInsideWindow(HWND window, POINT point)
    {
        RECT windowRect{};
        return GetWindowRect(window, &windowRect) &&
               PtInRect(&windowRect, point);
    }

    bool PostWorkerMessage(UINT message, HWND window, WPARAM wParam = 0)
    {
        if (!g_workerThreadId ||
            !PostThreadMessageW(g_workerThreadId, message, wParam,
                                reinterpret_cast<LPARAM>(window)))
        {
            Wh_Log(L"Failed to queue worker message: %lu", GetLastError());
            return false;
        }

        return true;
    }

    bool IsOtherMouseButtonDownMessage(WPARAM message)
    {
        return message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK ||
               message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK ||
               message == WM_XBUTTONDOWN || message == WM_XBUTTONDBLCLK ||
               message == WM_NCLBUTTONDOWN || message == WM_NCLBUTTONDBLCLK ||
               message == WM_NCMBUTTONDOWN || message == WM_NCMBUTTONDBLCLK ||
               message == WM_NCXBUTTONDOWN || message == WM_NCXBUTTONDBLCLK;
    }

    DWORD AllocateGestureGeneration()
    {
        DWORD generation;
        do
        {
            generation = g_nextGestureGeneration.fetch_add(
                             1, std::memory_order_relaxed) +
                         1;
        } while (!generation);

        return generation;
    }

    void ResetChannelScrollGesture()
    {
        g_channelScrollGestureActive = false;
        g_rmbGestureGeneration = 0;
        g_wheelRemainder = 0;
        g_gestureWindow = nullptr;
    }

    bool PostChannelSwitch(HWND window,
                           ChannelDirection direction,
                           DWORD generation)
    {
        const UINT message = direction == ChannelDirection::Previous
                                 ? kSwitchToPreviousChannelMessage
                                 : kSwitchToNextChannelMessage;
        return PostWorkerMessage(message, window, generation);
    }

    void CancelDiscordRightButtonInteraction(HWND window, HWND targetWindow)
    {
        // Cancel capture/drag state without waiting for Discord or injecting
        // input. The real RMB release will update Windows normally; only its
        // delivery to Discord is suppressed after an actual RMB+wheel gesture.
        HWND captureWindow = GetCapture();
        if (captureWindow && captureWindow != targetWindow &&
            captureWindow != window &&
            GetAncestor(captureWindow, GA_ROOT) == window)
        {
            PostMessageW(captureWindow, WM_CANCELMODE, 0, 0);
        }

        if (targetWindow != window)
        {
            PostMessageW(targetWindow, WM_CANCELMODE, 0, 0);
        }

        PostMessageW(window, WM_CANCELMODE, 0, 0);
    }

    bool ShouldConsumeMouseMessage(const MSG &message)
    {
        if (!message.hwnd || g_uninitializing.load(std::memory_order_acquire))
        {
            return false;
        }

        if (message.message == WM_RBUTTONDOWN ||
            message.message == WM_RBUTTONDBLCLK ||
            message.message == WM_NCRBUTTONDOWN ||
            message.message == WM_NCRBUTTONDBLCLK)
        {
            // Observe the boundary of a new click, but never delay, consume,
            // replay, or otherwise change a button press.
            g_switchGestureGeneration.store(0, std::memory_order_release);
            ResetChannelScrollGesture();
            return false;
        }

        if (IsOtherMouseButtonDownMessage(message.message))
        {
            g_switchGestureGeneration.store(0, std::memory_order_release);
            return false;
        }

        if (message.message == WM_RBUTTONUP ||
            message.message == WM_NCRBUTTONUP)
        {
            const bool suppressRelease =
                g_channelScrollGestureActive &&
                GetAncestor(message.hwnd, GA_ROOT) == g_gestureWindow;
            ResetChannelScrollGesture();
            // Keep already queued detents valid, even for a quick scroll and
            // release. A subsequent press will invalidate the old generation.
            return suppressRelease;
        }

        if (message.message != WM_MOUSEWHEEL)
        {
            return false;
        }

        // Use the state at the time of this wheel event, not the current
        // asynchronous state (the user may already have released RMB).
        const UINT buttons = GET_KEYSTATE_WPARAM(message.wParam);
        constexpr UINT kOtherButtons =
            MK_LBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2;
        if (!(buttons & MK_RBUTTON))
        {
            // Also discard stale state if RMB was released outside Discord.
            ResetChannelScrollGesture();
            return false;
        }
        if (buttons & kOtherButtons)
        {
            g_switchGestureGeneration.store(0, std::memory_order_release);
            return false;
        }

        const short delta = GET_WHEEL_DELTA_WPARAM(message.wParam);
        if (!delta)
        {
            return false;
        }

        HWND foregroundWindow = GetForegroundWindowForThisProcess();
        const POINT point{GET_X_LPARAM(message.lParam),
                          GET_Y_LPARAM(message.lParam)};
        if (!foregroundWindow ||
            GetAncestor(message.hwnd, GA_ROOT) != foregroundWindow ||
            !IsPointInsideWindow(foregroundWindow, point))
        {
            g_switchGestureGeneration.store(0, std::memory_order_release);
            return false;
        }

        if (!g_channelScrollGestureActive)
        {
            g_channelScrollGestureActive = true;
            g_rmbGestureGeneration = AllocateGestureGeneration();
            g_wheelRemainder = 0;
            g_gestureWindow = foregroundWindow;
            g_switchGestureGeneration.store(g_rmbGestureGeneration,
                                            std::memory_order_release);
            CancelDiscordRightButtonInteraction(g_gestureWindow, message.hwnd);
        }
        else if (foregroundWindow != g_gestureWindow ||
                 g_switchGestureGeneration.load(std::memory_order_acquire) !=
                     g_rmbGestureGeneration)
        {
            // An interrupted gesture must not keep eating ordinary input.
            g_switchGestureGeneration.store(0, std::memory_order_release);
            return false;
        }

        g_wheelRemainder += delta;
        while (g_wheelRemainder >= kWheelDelta)
        {
            PostChannelSwitch(g_gestureWindow, ChannelDirection::Previous,
                              g_rmbGestureGeneration);
            g_wheelRemainder -= kWheelDelta;
        }
        while (g_wheelRemainder <= -kWheelDelta)
        {
            PostChannelSwitch(g_gestureWindow, ChannelDirection::Next,
                              g_rmbGestureGeneration);
            g_wheelRemainder += kWheelDelta;
        }

        return true;
    }

    void FilterRemovedMessage(MSG *message)
    {
        if (ShouldConsumeMouseMessage(*message))
        {
            // The input has already been removed from Windows' queue, so its
            // button-state update is not lost. Return a harmless message rather
            // than changing PeekMessage's return behavior.
            message->hwnd = nullptr;
            message->message = WM_NULL;
            message->wParam = 0;
            message->lParam = 0;
        }
    }

    BOOL WINAPI PeekMessageW_Hook(LPMSG message, HWND window,
                                  UINT filterMin, UINT filterMax, UINT remove)
    {
        const BOOL result =
            PeekMessageW_Original(message, window, filterMin, filterMax, remove);
        // PM_NOREMOVE is observation only. Filtering it would switch channels more
        // than once and could hide the release before Windows processes it.
        if (result && (remove & PM_REMOVE))
        {
            FilterRemovedMessage(message);
        }
        return result;
    }

    bool IsExpectedForegroundWindow(HWND window)
    {
        return IsWindow(window) && GetForegroundWindowForThisProcess() == window;
    }

    bool IsCurrentSwitchGesture(DWORD generation)
    {
        return generation != 0 &&
               g_switchGestureGeneration.load(std::memory_order_acquire) ==
                   generation;
    }

    bool SendDiscordChannelShortcut(ChannelDirection direction)
    {
        const bool altAlreadyDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

        INPUT inputs[4]{};
        UINT inputCount = 0;

        if (!altAlreadyDown)
        {
            inputs[inputCount].type = INPUT_KEYBOARD;
            inputs[inputCount].ki.wVk = VK_MENU;
            ++inputCount;
        }

        inputs[inputCount].type = INPUT_KEYBOARD;
        inputs[inputCount].ki.wVk = direction == ChannelDirection::Previous
                                        ? VK_UP
                                        : VK_DOWN;
        inputs[inputCount].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
        ++inputCount;

        inputs[inputCount] = inputs[inputCount - 1];
        inputs[inputCount].ki.dwFlags =
            KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        ++inputCount;

        if (!altAlreadyDown)
        {
            inputs[inputCount].type = INPUT_KEYBOARD;
            inputs[inputCount].ki.wVk = VK_MENU;
            inputs[inputCount].ki.dwFlags = KEYEVENTF_KEYUP;
            ++inputCount;
        }

        if (SendInput(inputCount, inputs, sizeof(INPUT)) != inputCount)
        {
            Wh_Log(L"Failed to send Discord channel shortcut: %lu",
                   GetLastError());
            return false;
        }

        return true;
    }

    DWORD WINAPI ChannelSwitchWorkerThreadProc(void *parameter)
    {
        auto *startup = static_cast<ThreadStartup *>(parameter);

        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        startup->succeeded = TRUE;
        SetEvent(startup->readyEvent);

        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            const HWND window = reinterpret_cast<HWND>(message.lParam);
            const DWORD generation = static_cast<DWORD>(message.wParam);

            ChannelDirection direction;
            if (message.message == kSwitchToPreviousChannelMessage)
            {
                direction = ChannelDirection::Previous;
            }
            else if (message.message == kSwitchToNextChannelMessage)
            {
                direction = ChannelDirection::Next;
            }
            else
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
                continue;
            }

            if (!IsCurrentSwitchGesture(generation) ||
                !IsExpectedForegroundWindow(window))
            {
                continue;
            }

            SendDiscordChannelShortcut(direction);
        }

        return 0;
    }

    bool StartMessageThread(LPTHREAD_START_ROUTINE threadProcedure,
                            HANDLE *threadHandle,
                            DWORD *threadId)
    {
        HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!readyEvent)
        {
            return false;
        }

        ThreadStartup startup{
            .readyEvent = readyEvent,
            .succeeded = FALSE,
        };

        *threadHandle =
            CreateThread(nullptr, 0, threadProcedure, &startup, 0, threadId);
        if (!*threadHandle)
        {
            CloseHandle(readyEvent);
            return false;
        }

        const DWORD waitResult = WaitForSingleObject(readyEvent, INFINITE);
        CloseHandle(readyEvent);

        if (waitResult != WAIT_OBJECT_0 || !startup.succeeded)
        {
            WaitForSingleObject(*threadHandle, INFINITE);
            CloseHandle(*threadHandle);
            *threadHandle = nullptr;
            *threadId = 0;
            return false;
        }

        return true;
    }

    void StopMessageThread(HANDLE *threadHandle, DWORD *threadId)
    {
        if (!*threadHandle)
        {
            return;
        }

        PostThreadMessageW(*threadId, WM_QUIT, 0, 0);
        // The DLL must remain loaded until all callbacks and worker code return.
        WaitForSingleObject(*threadHandle, INFINITE);
        CloseHandle(*threadHandle);
        *threadHandle = nullptr;
        *threadId = 0;
    }

} // namespace

BOOL Wh_ModInit()
{
    if (!IsDiscordProcess())
    {
        return FALSE;
    }

    if (!StartMessageThread(ChannelSwitchWorkerThreadProc, &g_workerThread,
                            &g_workerThreadId))
    {
        Wh_Log(L"Couldn't start the channel switch worker thread");
        return FALSE;
    }

    if (!Wh_SetFunctionHook(reinterpret_cast<void *>(&PeekMessageW),
                            reinterpret_cast<void *>(&PeekMessageW_Hook),
                            reinterpret_cast<void **>(&PeekMessageW_Original)))
    {
        Wh_Log(L"Couldn't hook Discord's message retrieval");
        StopMessageThread(&g_workerThread, &g_workerThreadId);
        return FALSE;
    }

    Wh_Log(L"Discord Channel Scroll initialized");
    return TRUE;
}

void Wh_ModBeforeUninit()
{
    g_uninitializing.store(true, std::memory_order_release);
    g_switchGestureGeneration.store(0, std::memory_order_release);
}

void Wh_ModUninit()
{
    StopMessageThread(&g_workerThread, &g_workerThreadId);
    Wh_Log(L"Discord Channel Scroll uninitialized");
}
