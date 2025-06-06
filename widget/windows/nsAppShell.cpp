/* -*- Mode: c++; tab-width: 2; indent-tabs-mode: nil; -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Attributes.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/ipc/MessageChannel.h"
#include "mozilla/ipc/WindowsMessageLoop.h"
#include "nsAppShell.h"
#include "nsToolkit.h"
#include "nsThreadUtils.h"
#include "WinUtils.h"
#include "WinTaskbar.h"
#include "WinMouseScrollHandler.h"
#include "nsWindowDefs.h"
#include "nsWindow.h"
#include "nsString.h"
#include "WinIMEHandler.h"
#include "mozilla/BackgroundHangMonitor.h"
#include "mozilla/Hal.h"
#include "nsIDOMWakeLockListener.h"
#include "nsIPowerManagerService.h"
#include "mozilla/ProfilerLabels.h"
#include "mozilla/StaticPtr.h"
#include "nsTHashtable.h"
#include "nsHashKeys.h"
#include "nsComponentManagerUtils.h"
#include "ScreenHelperWin.h"
#include "HeadlessScreenHelper.h"
#include "mozilla/widget/ScreenManager.h"
#include "mozilla/Atomics.h"
#include "mozilla/NativeNt.h"
#include "mozilla/WindowsDiagnostics.h"
#include "mozilla/WindowsProcessMitigations.h"

#include <winternl.h>

#if defined(ACCESSIBILITY)
#  include "mozilla/a11y/Compatibility.h"
#  include "mozilla/a11y/Platform.h"
#endif  // defined(ACCESSIBILITY)

using namespace mozilla;
using namespace mozilla::widget;

#define WAKE_LOCK_LOG(...) \
  MOZ_LOG(gWinWakeLockLog, mozilla::LogLevel::Debug, (__VA_ARGS__))
static mozilla::LazyLogModule gWinWakeLockLog("WinWakeLock");

// This wakelock listener is used for Window7 and above.
class WinWakeLockListener final : public nsIDOMMozWakeLockListener {
 public:
  NS_DECL_ISUPPORTS
  WinWakeLockListener() { MOZ_ASSERT(XRE_IsParentProcess()); }

 private:
  ~WinWakeLockListener() {
    ReleaseWakelockIfNeeded(PowerRequestDisplayRequired);
    ReleaseWakelockIfNeeded(PowerRequestExecutionRequired);
  }

  void SetHandle(HANDLE aHandle, POWER_REQUEST_TYPE aType) {
    switch (aType) {
      case PowerRequestDisplayRequired: {
        if (!aHandle && mDisplayHandle) {
          CloseHandle(mDisplayHandle);
        }
        mDisplayHandle = aHandle;
        return;
      }
      case PowerRequestExecutionRequired: {
        if (!aHandle && mNonDisplayHandle) {
          CloseHandle(mNonDisplayHandle);
        }
        mNonDisplayHandle = aHandle;
        return;
      }
      default:
        MOZ_ASSERT_UNREACHABLE("Invalid request type");
        return;
    }
  }

  HANDLE GetHandle(POWER_REQUEST_TYPE aType) const {
    switch (aType) {
      case PowerRequestDisplayRequired:
        return mDisplayHandle;
      case PowerRequestExecutionRequired:
        return mNonDisplayHandle;
      default:
        MOZ_ASSERT_UNREACHABLE("Invalid request type");
        return nullptr;
    }
  }

  HANDLE CreateHandle(POWER_REQUEST_TYPE aType) {
    MOZ_ASSERT(!GetHandle(aType));
    REASON_CONTEXT context = {0};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = RequestTypeLPWSTR(aType);
    HANDLE handle = PowerCreateRequest(&context);
    if (!handle) {
      WAKE_LOCK_LOG("Failed to create handle for %s, error=%lu",
                    RequestTypeStr(aType), GetLastError());
      return nullptr;
    }
    SetHandle(handle, aType);
    return handle;
  }

  LPWSTR RequestTypeLPWSTR(POWER_REQUEST_TYPE aType) const {
    switch (aType) {
      case PowerRequestDisplayRequired:
        return const_cast<LPWSTR>(L"display request");  // -Wwritable-strings
      case PowerRequestExecutionRequired:
        return const_cast<LPWSTR>(
            L"non-display request");  // -Wwritable-strings
      default:
        MOZ_ASSERT_UNREACHABLE("Invalid request type");
        return const_cast<LPWSTR>(L"unknown");  // -Wwritable-strings
    }
  }

  const char* RequestTypeStr(POWER_REQUEST_TYPE aType) const {
    switch (aType) {
      case PowerRequestDisplayRequired:
        return "display request";
      case PowerRequestExecutionRequired:
        return "non-display request";
      default:
        MOZ_ASSERT_UNREACHABLE("Invalid request type");
        return "unknown";
    }
  }

  void RequestWakelockIfNeeded(POWER_REQUEST_TYPE aType) {
    if (GetHandle(aType)) {
      WAKE_LOCK_LOG("Already requested lock for %s", RequestTypeStr(aType));
      return;
    }

    WAKE_LOCK_LOG("Prepare a wakelock for %s", RequestTypeStr(aType));
    HANDLE handle = CreateHandle(aType);
    if (!handle) {
      WAKE_LOCK_LOG("Failed due to no handle for %s", RequestTypeStr(aType));
      return;
    }

    if (PowerSetRequest(handle, aType)) {
      WAKE_LOCK_LOG("Requested %s lock", RequestTypeStr(aType));
    } else {
      WAKE_LOCK_LOG("Failed to request %s lock, error=%lu",
                    RequestTypeStr(aType), GetLastError());
      SetHandle(nullptr, aType);
    }
  }

  void ReleaseWakelockIfNeeded(POWER_REQUEST_TYPE aType) {
    if (!GetHandle(aType)) {
      WAKE_LOCK_LOG("Already released lock for %s", RequestTypeStr(aType));
      return;
    }

    WAKE_LOCK_LOG("Prepare to release wakelock for %s", RequestTypeStr(aType));
    if (!PowerClearRequest(GetHandle(aType), aType)) {
      WAKE_LOCK_LOG("Failed to release %s lock, error=%lu",
                    RequestTypeStr(aType), GetLastError());
      return;
    }
    SetHandle(nullptr, aType);
    WAKE_LOCK_LOG("Released wakelock for %s", RequestTypeStr(aType));
  }

  NS_IMETHOD Callback(const nsAString& aTopic,
                      const nsAString& aState) override {
    WAKE_LOCK_LOG("topic=%s, state=%s", NS_ConvertUTF16toUTF8(aTopic).get(),
                  NS_ConvertUTF16toUTF8(aState).get());
    if (!aTopic.EqualsASCII("screen") && !aTopic.EqualsASCII("audio-playing") &&
        !aTopic.EqualsASCII("video-playing")) {
      return NS_OK;
    }

    const bool isNonDisplayLock = aTopic.EqualsASCII("audio-playing");
    bool requestLock = false;
    if (isNonDisplayLock) {
      requestLock = aState.EqualsASCII("locked-foreground") ||
                    aState.EqualsASCII("locked-background");
    } else {
      requestLock = aState.EqualsASCII("locked-foreground");
    }

    if (isNonDisplayLock) {
      if (requestLock) {
        RequestWakelockIfNeeded(PowerRequestExecutionRequired);
      } else {
        ReleaseWakelockIfNeeded(PowerRequestExecutionRequired);
      }
    } else {
      if (requestLock) {
        RequestWakelockIfNeeded(PowerRequestDisplayRequired);
      } else {
        ReleaseWakelockIfNeeded(PowerRequestDisplayRequired);
      }
    }
    return NS_OK;
  }

  // Handle would only exist when we request wakelock successfully.
  HANDLE mDisplayHandle = nullptr;
  HANDLE mNonDisplayHandle = nullptr;
};
NS_IMPL_ISUPPORTS(WinWakeLockListener, nsIDOMMozWakeLockListener)
StaticRefPtr<nsIDOMMozWakeLockListener> sWakeLockListener;

static void AddScreenWakeLockListener() {
  nsCOMPtr<nsIPowerManagerService> sPowerManagerService =
      do_GetService(POWERMANAGERSERVICE_CONTRACTID);
  if (sPowerManagerService) {
    sWakeLockListener = new WinWakeLockListener();
    sPowerManagerService->AddWakeLockListener(sWakeLockListener);
  } else {
    NS_WARNING(
        "Failed to retrieve PowerManagerService, wakelocks will be broken!");
  }
}

static void RemoveScreenWakeLockListener() {
  nsCOMPtr<nsIPowerManagerService> sPowerManagerService =
      do_GetService(POWERMANAGERSERVICE_CONTRACTID);
  if (sPowerManagerService) {
    sPowerManagerService->RemoveWakeLockListener(sWakeLockListener);
    sPowerManagerService = nullptr;
    sWakeLockListener = nullptr;
  }
}

class SingleNativeEventPump final : public nsIThreadObserver {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSITHREADOBSERVER

  SingleNativeEventPump() {
    MOZ_ASSERT(!XRE_UseNativeEventProcessing(),
               "Should only be used when not properly processing events.");
  }

 private:
  ~SingleNativeEventPump() {}
};

NS_IMPL_ISUPPORTS(SingleNativeEventPump, nsIThreadObserver)

NS_IMETHODIMP
SingleNativeEventPump::OnDispatchedEvent() { return NS_OK; }

NS_IMETHODIMP
SingleNativeEventPump::OnProcessNextEvent(nsIThreadInternal* aThread,
                                          bool aMayWait) {
  MSG msg;
  bool gotMessage = WinUtils::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE);
  if (gotMessage) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
  return NS_OK;
}

NS_IMETHODIMP
SingleNativeEventPump::AfterProcessNextEvent(nsIThreadInternal* aThread,
                                             bool aMayWait) {
  return NS_OK;
}

// RegisterWindowMessage values
// Native event callback message
const wchar_t* kAppShellGeckoEventId = L"nsAppShell:EventID";
UINT sAppShellGeckoMsgId = 0x10001;  // initialize to invalid message ID
// Taskbar button creation message
const wchar_t* kTaskbarButtonEventId = L"TaskbarButtonCreated";
UINT sTaskbarButtonCreatedMsg = 0x10002;  // initialize to invalid message ID

/* static */
UINT nsAppShell::GetTaskbarButtonCreatedMessage() {
  return sTaskbarButtonCreatedMsg;
}

namespace mozilla {
namespace crashreporter {
void LSPAnnotate();
}  // namespace crashreporter
}  // namespace mozilla

using mozilla::crashreporter::LSPAnnotate;

//-------------------------------------------------------------------------

// Note that since we're on x86-ish processors here, ReleaseAcquire is the
// semantics that normal loads and stores would use anyway.
static Atomic<size_t, ReleaseAcquire> sOutstandingNativeEventCallbacks;

/*static*/ LRESULT CALLBACK nsAppShell::EventWindowProc(HWND hwnd, UINT uMsg,
                                                        WPARAM wParam,
                                                        LPARAM lParam) {
  NativeEventLogger eventLogger("AppShell", hwnd, uMsg, wParam, lParam);

  if (uMsg == sAppShellGeckoMsgId) {
    // The app shell might have been destroyed between this message being
    // posted and being executed, so be extra careful.
    if (!sOutstandingNativeEventCallbacks) {
      return TRUE;
    }

    nsAppShell* as = reinterpret_cast<nsAppShell*>(lParam);
    as->NativeEventCallback();
    --sOutstandingNativeEventCallbacks;
    return TRUE;
  }

  LRESULT ret = DefWindowProc(hwnd, uMsg, wParam, lParam);
  eventLogger.SetResult(ret, false);
  return ret;
}

nsAppShell::~nsAppShell() {
  hal::Shutdown();

  if (mEventWnd) {
    // DestroyWindow doesn't do anything when called from a non UI thread.
    // Since mEventWnd was created on the UI thread, it must be destroyed on
    // the UI thread.
    SendMessage(mEventWnd, WM_CLOSE, 0, 0);
  }

  // Cancel any outstanding native event callbacks.
  sOutstandingNativeEventCallbacks = 0;
}

NS_IMETHODIMP
nsAppShell::Observe(nsISupports* aSubject, const char* aTopic,
                    const char16_t* aData) {
  if (XRE_IsParentProcess()) {
    nsCOMPtr<nsIObserverService> obsServ(
        mozilla::services::GetObserverService());

    if (!strcmp(aTopic, "sessionstore-restoring-on-startup")) {
      nsWindow::SetIsRestoringSession(true);
      // Now that we've handled the observer notification, we can remove it
      obsServ->RemoveObserver(this, "sessionstore-restoring-on-startup");
      return NS_OK;
    }

    if (!strcmp(aTopic, "sessionstore-windows-restored")) {
      nsWindow::SetIsRestoringSession(false);
      // Now that we've handled the observer notification, we can remove it
      obsServ->RemoveObserver(this, "sessionstore-windows-restored");
      return NS_OK;
    }
  }

  return nsBaseAppShell::Observe(aSubject, aTopic, aData);
}

namespace {

// Struct containing information about the user atom table. (See
// DiagnoseUserAtomTable(), below.)
struct AtomTableInformation {
  // Number of atoms in use. (Exactly 0x4000 == 16384, if all are.)
  UINT in_use = 0;
  // Number of atoms confirmed not in use.
  UINT free = 0;
  // Number of atoms which gave errors when checked.
  UINT errors = 0;

  // Last atom which gave an unexpected error...
  UINT lastErrorAtom = ~0u;
  // ... and the error it gave.
  WinErrorState lastErrorState;
};

// Return a summary of the state of the atom table.
MOZ_NEVER_INLINE static AtomTableInformation DiagnoseUserAtomTable() {
  // Restore error state on exit, for the sake of automated minidump analyses.
  auto const _restoreErrState =
      mozilla::MakeScopeExit([oldErrState = WinErrorState::Get()]() {
        WinErrorState::Apply(oldErrState);
      });

  AtomTableInformation retval;

  // Expected error-state on failure-return when the atom is assigned, but not
  // enough space was provided for the full string.
  constexpr WinErrorState kBufferTooSmall = {
      .error = ERROR_INSUFFICIENT_BUFFER,
      .ntStatus = ((NTSTATUS)0xC0000023),  // == STATUS_BUFFER_TOO_SMALL
  };
  // Expected error-state on failure-return when the atom is not assigned.
  constexpr WinErrorState kInvalidAtom = {
      .error = ERROR_INVALID_HANDLE,
      .ntStatus = ((NTSTATUS)STATUS_INVALID_HANDLE),
  };

  // Iterate over only the dynamic portion of the atom table.
  for (UINT atom = 0xC000; atom <= 0xFFFF; ++atom) {
    // The actual atom values are PII. Don't acquire them in their entirety, and
    // don't keep more information about them than is needed.
    WCHAR buf[2] = {};
    // USE OF UNDOCUMENTED BEHAVIOR: The user atom table is shared by message
    // names, window-class names, and clipboard-format names. Only the last has
    // a documented getter-mechanism.
    BOOL const ok = ::GetClipboardFormatNameW(atom, buf, 1);
    WinErrorState const errState = WinErrorState::Get();
    if (ok || errState == kBufferTooSmall) {
      ++retval.in_use;
    } else if (errState == kInvalidAtom) {
      ++retval.free;
    } else {
      // Unexpected error-state.
      ++retval.errors;
      retval.lastErrorAtom = atom;
      retval.lastErrorState = errState;
    }
  }

  return retval;
}

#if defined(MOZ_DIAGNOSTIC_ASSERT_ENABLED) && defined(_M_X64)
static constexpr int kMaxStepsUser32 = 0x1800;
static constexpr int kMaxErrorStatesUser32 = 0x200;
using User32SingleStepData =
    ModuleSingleStepData<kMaxStepsUser32, kMaxErrorStatesUser32>;

template <typename CallbackToRun, typename PostCollectionCallback>
WindowsDiagnosticsError CollectUser32SingleStepData(
    CallbackToRun aCallbackToRun,
    PostCollectionCallback aPostCollectionCallback) {
  return CollectModuleSingleStepData<kMaxStepsUser32, kMaxErrorStatesUser32>(
      L"user32.dll", std::move(aCallbackToRun),
      std::move(aPostCollectionCallback));
}
#endif  // MOZ_DIAGNOSTIC_ASSERT_ENABLED && _M_X64

}  // namespace

// Collect data for bug 1571516. We don't automatically send up `GetLastError`
// or `GetLastNtStatus` data for beta/release builds, so extract the relevant
// error values and store them on the stack, where they can be viewed in
// minidumps -- in fact, do so after each individual API call. This takes the
// form of various local variables whose initial character is an underscore,
// most of which are also marked [[maybe_unused]].
//
// We tag this function `[[clang::optnone]]` to prevent the compiler from
// eliding those values as _actually_ unused, as well as to generally simplify
// the haruspex's task once the minidumps are in. (As this function should be
// called at most once per process, the minor performance hit is not a concern.)
//
/* static */ [[clang::optnone]] MOZ_NEVER_INLINE HWND
nsAppShell::StaticCreateEventWindow() {
  // This code path would fail at CreateWindowW under win32k lockdown.
  MOZ_RELEASE_ASSERT(!IsWin32kLockedDown());

  // note the incoming error-state; this may be relevant to errors we get later
  auto _initialErr [[maybe_unused]] = WinErrorState::Get();
  // reset the error-state, to avoid ambiguity below
  WinErrorState::Clear();

  // Diagnostic variable. Only collected in the event of a failure in one of the
  // functions that attempts to register an atom.
  AtomTableInformation _atomTableInfo [[maybe_unused]];

  // Attempt to register the window message. On failure, retain the initial
  // value of `sAppShellGeckoMsgId`.
  auto const _msgId = ::RegisterWindowMessageW(kAppShellGeckoEventId);
  if (_msgId) {
    sAppShellGeckoMsgId = _msgId;
  }
  auto const _sAppShellGeckoMsgId [[maybe_unused]] = sAppShellGeckoMsgId;
  auto const _rwmErr [[maybe_unused]] = WinErrorState::Get();
  if (!_msgId) _atomTableInfo = DiagnoseUserAtomTable();
  NS_ASSERTION(sAppShellGeckoMsgId,
               "Could not register hidden window event message!");

  WNDCLASSW wc;
  HINSTANCE const module = GetModuleHandle(nullptr);

  constexpr const wchar_t* kWindowClass = L"nsAppShell:EventWindowClass";
  // (Undocumented behavior note: on success, this will specifically be the
  // window-class atom. We don't rely on this.)
  BOOL const _gciwRet = ::GetClassInfoW(module, kWindowClass, &wc);
  auto const _gciwErr [[maybe_unused]] = WinErrorState::Get();
  WinErrorState::Clear();

  WinErrorState _rcErr [[maybe_unused]];
  if (!_gciwRet) {
    wc.style = 0;
    wc.lpfnWndProc = EventWindowProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = module;
    wc.hIcon = nullptr;
    wc.hCursor = nullptr;
    wc.hbrBackground = (HBRUSH) nullptr;
    wc.lpszMenuName = (LPCWSTR) nullptr;
    wc.lpszClassName = kWindowClass;

    ATOM _windowClassAtom = ::RegisterClassW(&wc);
    _rcErr = WinErrorState::Get();

    if (!_windowClassAtom) _atomTableInfo = DiagnoseUserAtomTable();

#if defined(MOZ_DIAGNOSTIC_ASSERT_ENABLED) && defined(_M_X64)
    if (!_windowClassAtom) {
      // Retry with single-step data collection
      WindowsDiagnosticsError rv = CollectUser32SingleStepData(
          [&wc, &_windowClassAtom]() {
            _windowClassAtom = ::RegisterClassW(&wc);
          },
          [&_windowClassAtom](const User32SingleStepData& aData) {
            // Crashing here gives access to the single step data on stack
            MOZ_DIAGNOSTIC_ASSERT(
                _windowClassAtom,
                "RegisterClassW for EventWindowClass failed twice");
          });
      auto const _cssdErr [[maybe_unused]] = WinErrorState::Get();
      MOZ_DIAGNOSTIC_ASSERT(
          rv == WindowsDiagnosticsError::None,
          "Failed to collect single step data for RegisterClassW");
      // If we reach this point then somehow the single-stepped call succeeded
      // and we can proceed
    }
#endif  // MOZ_DIAGNOSTIC_ASSERT_ENABLED && _M_X64

    MOZ_RELEASE_ASSERT(_windowClassAtom,
                       "RegisterClassW for EventWindowClass failed");
    WinErrorState::Clear();
  }

  HWND eventWnd =
      CreateWindowW(kWindowClass, L"nsAppShell:EventWindow", 0, 0, 0, 10, 10,
                    HWND_MESSAGE, nullptr, module, nullptr);
  auto const _cwErr [[maybe_unused]] = WinErrorState::Get();

#if defined(MOZ_DIAGNOSTIC_ASSERT_ENABLED) && defined(_M_X64)
  if (!eventWnd) {
    // Retry with single-step data collection
    WindowsDiagnosticsError rv = CollectUser32SingleStepData(
        [module, &eventWnd]() {
          eventWnd =
              CreateWindowW(kWindowClass, L"nsAppShell:EventWindow", 0, 0, 0,
                            10, 10, HWND_MESSAGE, nullptr, module, nullptr);
        },
        [&eventWnd](const User32SingleStepData& aData) {
          // Crashing here gives access to the single step data on stack
          MOZ_DIAGNOSTIC_ASSERT(eventWnd,
                                "CreateWindowW for EventWindow failed twice");
        });
    auto const _cssdErr [[maybe_unused]] = WinErrorState::Get();
    MOZ_DIAGNOSTIC_ASSERT(
        rv == WindowsDiagnosticsError::None,
        "Failed to collect single step data for CreateWindowW");
    // If we reach this point then somehow the single-stepped call succeeded and
    // we can proceed
  }
#endif  // MOZ_DIAGNOSTIC_ASSERT_ENABLED && _M_X64

  MOZ_RELEASE_ASSERT(eventWnd, "CreateWindowW for EventWindow failed");

  return eventWnd;
}

HWND nsAppShell::sPrecachedEventWnd{};

/* static */ bool nsAppShell::PrecacheEventWindow() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_RELEASE_ASSERT(!sPrecachedEventWnd);

  sPrecachedEventWnd = StaticCreateEventWindow();
  return static_cast<bool>(sPrecachedEventWnd);
}

nsresult nsAppShell::InitEventWindow() {
  MOZ_ASSERT(NS_IsMainThread());

  if (sPrecachedEventWnd) {
    mEventWnd = sPrecachedEventWnd;
    sPrecachedEventWnd = nullptr;
  } else {
    mEventWnd = StaticCreateEventWindow();
  }

  mLastNativeEventScheduled = TimeStamp::NowLoRes();

  NS_ENSURE_STATE(mEventWnd);

  return NS_OK;
}

nsresult nsAppShell::Init() {
  LSPAnnotate();

  hal::Init();

  if (XRE_IsParentProcess()) {
    sTaskbarButtonCreatedMsg = ::RegisterWindowMessageW(kTaskbarButtonEventId);
    NS_ASSERTION(sTaskbarButtonCreatedMsg,
                 "Could not register taskbar button creation message");
  }

  // The hidden message window is used for interrupting the processing of native
  // events, so that we can process gecko events. Therefore, we only need it if
  // we are processing native events. Disabling this is required for win32k
  // syscall lockdown.
  if (XRE_UseNativeEventProcessing()) {
    if (nsresult rv = this->InitEventWindow(); NS_FAILED(rv)) {
      return rv;
    }
  } else {
    // Load winmm.dll because it is still needed by our event loop and might not
    // get loaded before we lower the sandbox.
    ::LoadLibraryW(L"winmm.dll");

    if (XRE_IsContentProcess() && !IsWin32kLockedDown()) {
      // We're not generally processing native events, but still using GDI and
      // we still have some internal windows, e.g. from calling CoInitializeEx.
      // So we use a class that will do a single event pump where previously we
      // might have processed multiple events to make sure any occasional
      // messages to these windows are processed. This also allows any internal
      // Windows messages to be processed to ensure the GDI data remains fresh.
      nsCOMPtr<nsIThreadInternal> threadInt =
          do_QueryInterface(NS_GetCurrentThread());
      if (threadInt) {
        threadInt->SetObserver(new SingleNativeEventPump());
      }
    }
  }

  if (XRE_IsParentProcess()) {
    ScreenManager& screenManager = ScreenManager::GetSingleton();
    if (gfxPlatform::IsHeadless()) {
      screenManager.SetHelper(mozilla::MakeUnique<HeadlessScreenHelper>());
    } else {
      screenManager.SetHelper(mozilla::MakeUnique<ScreenHelperWin>());
      ScreenHelperWin::RefreshScreens();
    }

    nsCOMPtr<nsIObserverService> obsServ(
        mozilla::services::GetObserverService());

    obsServ->AddObserver(this, "sessionstore-restoring-on-startup", false);
    obsServ->AddObserver(this, "sessionstore-windows-restored", false);
  }

  if (!WinUtils::GetTimezoneName(mTimezoneName)) {
    NS_WARNING("Unable to get system timezone name, timezone may be invalid\n");
  }

  return nsBaseAppShell::Init();
}

NS_IMETHODIMP
nsAppShell::Run(void) {
  if (XRE_IsParentProcess()) {
    // Add an observer that disables the screen saver when requested by Gecko.
    // For example when we're playing video in the foreground tab. Whole firefox
    // only needs one wakelock instance, so we would only create one listener in
    // chrome process to prevent requesting unnecessary wakelock.
    AddScreenWakeLockListener();
  }

  nsresult rv = nsBaseAppShell::Run();

  if (XRE_IsParentProcess()) {
    RemoveScreenWakeLockListener();
  }

  return rv;
}

void nsAppShell::DoProcessMoreGeckoEvents() {
  // Called by nsBaseAppShell's NativeEventCallback() after it has finished
  // processing pending gecko events and there are still gecko events pending
  // for the thread. (This can happen if NS_ProcessPendingEvents reached it's
  // starvation timeout limit.) The default behavior in nsBaseAppShell is to
  // call ScheduleNativeEventCallback to post a follow up native event callback
  // message. This triggers an additional call to NativeEventCallback for more
  // gecko event processing.

  // There's a deadlock risk here with certain internal Windows modal loops. In
  // our dispatch code, we prioritize messages so that input is handled first.
  // However Windows modal dispatch loops often prioritize posted messages. If
  // we find ourselves in a tight gecko timer loop where NS_ProcessPendingEvents
  // takes longer than the timer duration, NS_HasPendingEvents(thread) will
  // always be true. ScheduleNativeEventCallback will be called on every
  // NativeEventCallback callback, and in a Windows modal dispatch loop, the
  // callback message will be processed first -> input gets starved, dead lock.

  // To avoid, don't post native callback messages from NativeEventCallback
  // when we're in a modal loop. This gets us back into the Windows modal
  // dispatch loop dispatching input messages. Once we drop out of the modal
  // loop, we use mNativeCallbackPending to fire off a final NativeEventCallback
  // if we need it, which insures NS_ProcessPendingEvents gets called and all
  // gecko events get processed.
  if (mEventloopNestingLevel < 2) {
    OnDispatchedEvent();
    mNativeCallbackPending = false;
  } else {
    mNativeCallbackPending = true;
  }
}

void nsAppShell::ScheduleNativeEventCallback() {
  MOZ_ASSERT(mEventWnd,
             "We should have created mEventWnd in Init, if this is called.");

  // Post a message to the hidden message window
  ++sOutstandingNativeEventCallbacks;
  {
    MutexAutoLock lock(mLastNativeEventScheduledMutex);
    // Time stamp this event so we can detect cases where the event gets
    // dropping in sub classes / modal loops we do not control.
    mLastNativeEventScheduled = TimeStamp::NowLoRes();
  }
  ::PostMessage(mEventWnd, sAppShellGeckoMsgId, 0,
                reinterpret_cast<LPARAM>(this));
}

bool nsAppShell::ProcessNextNativeEvent(bool mayWait) {
  // Notify ipc we are spinning a (possibly nested) gecko event loop.
  mozilla::ipc::MessageChannel::NotifyGeckoEventDispatch();

  bool gotMessage = false;

  do {
    MSG msg;

    if (!gotMessage) {
      gotMessage = WinUtils::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE);
    }

    if (gotMessage) {
      if (msg.message == WM_QUIT) {
        ::PostQuitMessage(msg.wParam);
        Exit();
      } else {
        // If we had UI activity we would be processing it now so we know we
        // have either kUIActivity or kActivityNoUIAVail.
        mozilla::BackgroundHangMonitor().NotifyActivity();

        if (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST &&
            IMEHandler::ProcessRawKeyMessage(msg)) {
          continue;  // the message is consumed.
        }

#if defined(_X86_)
        // Store Printer dialog messages for reposting on x86, because on x86
        // Windows 7 they are not processed by a window procedure, but are
        // explicitly waited for in the winspool.drv code that will be further
        // up the stack (winspool!WaitForCompletionMessage). These are
        // undocumented Windows Message identifiers found in winspool.drv.
        if (msg.message == 0x5b7a || msg.message == 0x5b7f ||
            msg.message == 0x5b80 || msg.message == 0x5b81) {
          mMsgsToRepost.push_back(msg);
          continue;
        }
#endif

        // Windows documentation suggets that WM_SETTINGSCHANGE is the message
        // to watch for timezone changes, but experimentation showed that it
        // doesn't fire on changing the timezone, but that WM_TIMECHANGE does,
        // even if there's no immediate effect on the clock (e.g., changing
        // from Pacific Daylight at UTC-7 to Arizona at UTC-7).
        if (msg.message == WM_TIMECHANGE) {
          // The message may not give us sufficient information to determine
          // if the timezone changed, so keep track of it ourselves.
          wchar_t systemTimezone[128];
          bool getSystemTimeSucceeded =
              WinUtils::GetTimezoneName(systemTimezone);
          if (getSystemTimeSucceeded && wcscmp(systemTimezone, mTimezoneName)) {
            nsBaseAppShell::OnSystemTimezoneChange();

            wcscpy_s(mTimezoneName, 128, systemTimezone);
          }
        }

        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
      }
    } else if (mayWait) {
      // Block and wait for any posted application message
      mozilla::BackgroundHangMonitor().NotifyWait();
      {
        AUTO_PROFILER_LABEL("nsAppShell::ProcessNextNativeEvent::Wait", IDLE);
        WinUtils::WaitForMessage();
      }
    }
  } while (!gotMessage && mayWait);

  // See DoProcessNextNativeEvent, mEventloopNestingLevel will be
  // one when a modal loop unwinds.
  if (mNativeCallbackPending && mEventloopNestingLevel == 1)
    DoProcessMoreGeckoEvents();

  // Check for starved native callbacks. If we haven't processed one
  // of these events in NATIVE_EVENT_STARVATION_LIMIT, fire one off.
  static const mozilla::TimeDuration nativeEventStarvationLimit =
      mozilla::TimeDuration::FromSeconds(NATIVE_EVENT_STARVATION_LIMIT);

  TimeDuration timeSinceLastNativeEventScheduled;
  {
    MutexAutoLock lock(mLastNativeEventScheduledMutex);
    timeSinceLastNativeEventScheduled =
        TimeStamp::NowLoRes() - mLastNativeEventScheduled;
  }
  if (timeSinceLastNativeEventScheduled > nativeEventStarvationLimit) {
    ScheduleNativeEventCallback();
  }

  return gotMessage;
}

nsresult nsAppShell::AfterProcessNextEvent(nsIThreadInternal* /* unused */,
                                           bool /* unused */) {
  if (!mMsgsToRepost.empty()) {
    for (MSG msg : mMsgsToRepost) {
      ::PostMessageW(msg.hwnd, msg.message, msg.wParam, msg.lParam);
    }
    mMsgsToRepost.clear();
  }
  return NS_OK;
}
