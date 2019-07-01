#pragma once

#ifndef _WINDOWS_
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
        #define IS_FIRST_WIN32_LEAN_AND_MEAN
    #endif

    #ifndef WIN32_NO_STATUS
        #define WIN32_NO_STATUS
        #define IS_FIRST_WIN32_NO_STATUS
    #endif

    #include <Windows.h>

    #ifdef IS_FIRST_WIN32_NO_STATUS
        #undef WIN32_NO_STATUS
        #undef IS_FIRST_WIN32_NO_STATUS
    #endif

    #ifdef IS_FIRST_WIN32_LEAN_AND_MEAN
        #undef WIN32_LEAN_AND_MEAN
        #undef IS_FIRST_WIN32_LEAN_AND_MEAN
    #endif
#endif

#ifndef _WINTERNL_
    #include <winternl.h>
#endif

#ifndef _NTSTATUS_
    #include <ntstatus.h>
#endif

#ifndef _DEQUE_
    #include <deque>
#endif

#ifndef _FUNCTIONAL_
    #include <functional>
#endif

#pragma comment(lib, "ntdll.lib")

namespace Ariadna {

#ifdef _AMD64_
    class UmsScheduler {
    private:
        _Function_class_(RTL_UMS_SCHEDULER_ENTRY_POINT)
        static VOID NTAPI RtlUmsSchedulerEntryPoint(
            _In_ RTL_UMS_SCHEDULER_REASON Reason,
            _In_ ULONG_PTR ActivationPayload,
            _In_ PVOID SchedulerParam
        ) {
            UmsScheduler& Scheduler = UmsScheduler::GetInstance();

            switch (Reason) {
            case UmsSchedulerStartup: { // Scheduler startup:
                SetEvent(Scheduler.hStartupEvent);
                break;
            }
            case UmsSchedulerThreadYield: { // UMS-worker called UmsThreadYield:
                auto YieldedThread = reinterpret_cast<PUMS_CONTEXT>(ActivationPayload);
                Scheduler.RunnableThreads.push_back(YieldedThread); // Push it to the tail of queue
                break;
            }
            case UmsSchedulerThreadBlocked: { // UMS-worker locks on a kernel service:
                switch (ActivationPayload) {
                case 0: {
                    // Blocked on a trap (e.g. hard page fault) or an interrupt (e.g. APC):
                    // ... Place here any special handling if you need it ...
                    break;
                }
                case 1: {
                    // Blocked on a system call:
                    // ... Place here any special handling if you need it ...
                    break;
                }
                }
                break;
            }
            }

            while (true)
            {
                // Pump all ready threads:
                while (!Scheduler.RunnableThreads.empty())
                {
                    PUMS_CONTEXT Thread = Scheduler.RunnableThreads.front();
                    Scheduler.RunnableThreads.pop_front();

                    BOOLEAN IsTerminated = FALSE;
                    QueryUmsThreadInformation(Thread, UmsThreadIsTerminated, &IsTerminated, sizeof(IsTerminated), NULL);
                    if (IsTerminated) {
                        DeleteUmsThreadContext(Thread);
                    }
                    else {
                        ExecuteUmsThread(Thread);
                    }
                }

                // Wait for a new threads:
                HANDLE hEvents[2] = { Scheduler.hCompletionEvent, Scheduler.hStopEvent };
                DWORD SignaledEvent = WaitForMultipleObjects(_countof(hEvents), hEvents, FALSE, INFINITE);
                switch (SignaledEvent) {
                case 0: {
                    // Enumerating all ready threads:
                    PUMS_CONTEXT Thread = NULL;
                    if (DequeueUmsCompletionListItems(Scheduler.CompletionList, 0, &Thread)) {
                        while (Thread) {
                            Scheduler.RunnableThreads.push_back(Thread);
                            Thread = GetNextUmsListItem(Thread);
                        }
                    }
                    break;
                }
                case 1: {
                    // We're should to finish the scheduler thread:
                    ExitThread(0);
                    break;
                }
                }
            }
        }

        // Switch the current thread to the usermode scheduling mode:
        BOOL StartScheduling()
        {
            UMS_SCHEDULER_STARTUP_INFO StartupInfo = {};
            StartupInfo.UmsVersion = UMS_VERSION;
            StartupInfo.CompletionList = CompletionList;
            StartupInfo.SchedulerProc = RtlUmsSchedulerEntryPoint;
            StartupInfo.SchedulerParam = this;

            return EnterUmsSchedulingMode(&StartupInfo);
        }

        // Scheduler thread stub:
        static DWORD WINAPI SchedulerThread(PVOID Arg)
        {
            auto Self = reinterpret_cast<UmsScheduler*>(Arg);
            return Self->StartScheduling();
        }

        HANDLE hStartupEvent, hStopEvent;
        HANDLE hSchedulerThread;
        DWORD SchedulerThreadId;

        PUMS_COMPLETION_LIST CompletionList;
        HANDLE hCompletionEvent;

        std::deque<PUMS_CONTEXT> RunnableThreads;

        UmsScheduler()
            : CompletionList(NULL),
            hCompletionEvent(NULL), hSchedulerThread(NULL),
            SchedulerThreadId(0),
            hStartupEvent(CreateEvent(NULL, TRUE, FALSE, NULL)),
            hStopEvent(CreateEvent(NULL, TRUE, FALSE, NULL)),
            RunnableThreads()
        {}

        ~UmsScheduler()
        {
            StopUmsScheduler();
        }

        // Creating the UMS completion list and completion event:
        BOOL RegisterUmsScheduler()
        {
            if (CompletionList) return FALSE; // Already registered

            // First of all we need to create a completion list for our scheduler:
            if (!CreateUmsCompletionList(&CompletionList))
                return FALSE;

            // Obtaining a completion event that will signal about new ready threads:
            if (!GetUmsCompletionListEvent(CompletionList, &hCompletionEvent)) {
                DeleteUmsCompletionList(CompletionList);
                CompletionList = NULL;
                hCompletionEvent = NULL;
                return FALSE;
            }

            return TRUE;
        }

        // Freeing the UMS completion list and completion event:
        VOID UnregisterUmsScheduler()
        {
            DeleteUmsCompletionList(CompletionList);
            CloseHandle(hCompletionEvent);
            CompletionList = NULL;
            hCompletionEvent = NULL;
        }

    public:
        UmsScheduler(const UmsScheduler&) = delete;
        UmsScheduler(UmsScheduler&&) = delete;
        UmsScheduler& operator = (const UmsScheduler&) = delete;
        UmsScheduler& operator = (UmsScheduler&&) = delete;

        static UmsScheduler& GetInstance()
        {
            static UmsScheduler Scheduler; // Instance
            return Scheduler;
        }

        BOOL StartUmsScheduler()
        {
            if (hSchedulerThread) return FALSE; // Scheduler thread already running

            // Setting up scheduler completion list and completion event:
            if (!RegisterUmsScheduler()) return FALSE;

            // Starting the scheduler thread:
            ResetEvent(hStopEvent);
            hSchedulerThread = CreateThread(NULL, 0, SchedulerThread, this, 0, &SchedulerThreadId);
            if (hSchedulerThread) {
                WaitForSingleObject(hStartupEvent, INFINITE);
                return TRUE;
            }
            else {
                // Freeing resources in case of thread creation failure:
                UnregisterUmsScheduler();
                return FALSE;
            }
        }

        BOOL StopUmsScheduler()
        {
            if (!hSchedulerThread) return FALSE; // Scheduler thread not running

            // Stopping the UMS-thread:
            SetEvent(hStopEvent);
            WaitForSingleObject(hSchedulerThread, INFINITE);
            CloseHandle(hSchedulerThread);
            hSchedulerThread = NULL;
            SchedulerThreadId = 0;

            // Freeing all UMS-specific resources:
            UnregisterUmsScheduler();

            return TRUE;
        }

        HANDLE StartThread(IN LPTHREAD_START_ROUTINE ThreadProc, IN PVOID Arg = NULL, OUT OPTIONAL PDWORD ThreadId = NULL)
        {
            if (!hSchedulerThread) return NULL; // UMS-thread not running

            // Initializing an UMS context for a new UMS-thread:
            PUMS_CONTEXT UmsContext = NULL;
            if (!CreateUmsThreadContext(&UmsContext))
                return NULL;

            // Obtaining attributes buffer size:
            SIZE_T AttributesSize = 0;
            InitializeProcThreadAttributeList(NULL, 1, 0, &AttributesSize);

            // Initializing attributes:
            auto Attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, AttributesSize));
            InitializeProcThreadAttributeList(Attributes, 1, 0, &AttributesSize);

            // Updating thread attributes:
            UMS_CREATE_THREAD_ATTRIBUTES UmsAttributes = {};
            UmsAttributes.UmsVersion = UMS_VERSION;
            UmsAttributes.UmsContext = UmsContext;
            UmsAttributes.UmsCompletionList = CompletionList;
            UpdateProcThreadAttribute(Attributes, 0, PROC_THREAD_ATTRIBUTE_UMS_THREAD, &UmsAttributes, sizeof(UmsAttributes), NULL, NULL);

            // Creating thread:
            HANDLE hThread = CreateRemoteThreadEx(GetCurrentProcess(), NULL, 0, ThreadProc, Arg, 0, Attributes, ThreadId);

            // Freeing resources:
            DeleteProcThreadAttributeList(Attributes);
            HeapFree(GetProcessHeap(), 0, Attributes);
            if (!hThread)
                DeleteUmsThreadContext(UmsContext);

            return hThread;
        }

        HANDLE GetSchedulerThread() const { return hSchedulerThread; }
        DWORD GetSchedulerThreadId() const { return SchedulerThreadId; }

        static BOOL UmsYieldThread() { return UmsThreadYield(&UmsScheduler::GetInstance()); }
    };
#endif

    class Fibers {
    public:
        // Returns address of the fiber:
        static inline PVOID ThreadToFiber(IN OPTIONAL PVOID Parameter = NULL) { return ConvertThreadToFiber(Parameter); }
        static inline BOOL FiberToThread() { return ConvertFiberToThread(); }
        static inline PVOID GetData() { return GetFiberData(); }
        static inline PVOID Current() {
            const PVOID FiberMagic = reinterpret_cast<PVOID>(0x1E00);
            PVOID CurrentFiber = GetCurrentFiber();
            return CurrentFiber != FiberMagic ? CurrentFiber : NULL;
        }
        static inline PVOID Create(IN LPFIBER_START_ROUTINE FiberProc, IN PVOID Arg) { return CreateFiber(0, FiberProc, Arg); }
        static inline VOID SwitchTo(IN PVOID Fiber) { return SwitchToFiber(Fiber); }
        static BOOL CallInFiber(IN LPFIBER_START_ROUTINE FiberProc, IN OPTIONAL PVOID Arg = NULL)
        {
            PVOID CurrentFiber = Current();
            BOOLEAN IsAlreadyFiber = CurrentFiber != NULL;
            
            if (!IsAlreadyFiber)
                CurrentFiber = ConvertThreadToFiber(NULL);

            struct FIBER_ARGS {
                LPFIBER_START_ROUTINE FiberProc;
                PVOID Arg;
                PVOID PreviousFiber;
            } FiberArgs = { FiberProc, Arg, CurrentFiber };

            PVOID Fiber = CreateFiber(0, [](PVOID Arg) {
                auto Args = reinterpret_cast<FIBER_ARGS*>(Arg);
                Args->FiberProc(Args->Arg);
                SwitchToFiber(Args->PreviousFiber);
            }, &FiberArgs);

            if (!Fiber) return FALSE;
            SwitchToFiber(Fiber);

            if (!IsAlreadyFiber)
                ConvertFiberToThread();

            return TRUE;
        }
    };

    namespace NtApi {
        extern "C" NTSYSAPI NTSTATUS NTAPI NtQueueApcThread(
            IN HANDLE ThreadHandle,
            IN PIO_APC_ROUTINE ApcRoutine,
            IN OPTIONAL PVOID ApcRoutineContext,
            IN OPTIONAL PIO_STATUS_BLOCK ApcStatusBlock,
            IN OPTIONAL ULONG ApcReserved
        );
        extern "C" NTSYSAPI NTSTATUS NTAPI NtTestAlert();
        extern "C" NTSYSAPI NTSTATUS NTAPI NtAlertThread(IN HANDLE ThreadHandle);
    }

    class Threads {
    private:
        template <class Tuple, size_t... indices>
        static unsigned __stdcall stub(void* arg) noexcept
        {
            const std::unique_ptr<Tuple> fn_vals(static_cast<Tuple*>(arg));
            Tuple& tuple = *fn_vals;
            return std::invoke(std::move(std::get<indices>(tuple))...);
        }

        template <class Tuple, size_t... indices>
        static constexpr auto get_invoke(std::index_sequence<indices...>) noexcept
        {
            return &stub<Tuple, indices...>;
        }

    public:
        static inline HANDLE StartThread(IN LPTHREAD_START_ROUTINE ThreadProc, IN PVOID Arg = NULL, OUT OPTIONAL PDWORD ThreadId = NULL)
        {
            return CreateThread(NULL, 0, ThreadProc, Arg, 0, ThreadId);
        }

        static inline HANDLE CreateSuspended(IN LPTHREAD_START_ROUTINE ThreadProc, IN PVOID Arg = NULL, OUT OPTIONAL PDWORD ThreadId = NULL)
        {
            return CreateThread(NULL, 0, ThreadProc, Arg, CREATE_SUSPENDED, ThreadId);
        }

        static inline BOOL CallAsync(IN LPTHREAD_START_ROUTINE ThreadProc, IN PVOID Arg = NULL, OUT OPTIONAL PDWORD ThreadId = NULL)
        {
            HANDLE hThread = StartThread(ThreadProc, Arg, ThreadId);
            if (hThread) {
                return CloseHandle(hThread);
            }
            else {
                return FALSE;
            }
        }

        template <class Func, class... Args, class = std::enable_if_t<!std::is_same_v<std::_Remove_cvref_t<Func>, Threads>>>
        static HANDLE Run(Func&& func, Args&& ... args)
        {
            using FuncTuple = std::tuple<std::decay_t<Func>, std::decay_t<Args>...>;
            auto decay_copied = std::make_unique<FuncTuple>(std::forward<Func>(func), std::forward<Args>(args)...);
            constexpr auto invoker = get_invoke<FuncTuple>(std::make_index_sequence<1 + sizeof...(Args)>{});
            HANDLE hThread = CreateThread(NULL, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(invoker), decay_copied.get(), 0, NULL);
            if (hThread) decay_copied.release(); // Ownership transferred to the thread
            return hThread;
        }

        template <class Func, class... Args, class = std::enable_if_t<!std::is_same_v<std::_Remove_cvref_t<Func>, Threads>>>
        static BOOL Async(Func&& func, Args&& ... args)
        {
            using FuncTuple = std::tuple<std::decay_t<Func>, std::decay_t<Args>...>;
            auto decay_copied = std::make_unique<FuncTuple>(std::forward<Func>(func), std::forward<Args>(args)...);
            constexpr auto invoker = get_invoke<FuncTuple>(std::make_index_sequence<1 + sizeof...(Args)>{});
            HANDLE hThread = CreateThread(NULL, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(invoker), decay_copied.get(), 0, NULL);
            if (hThread) {
                decay_copied.release(); // Ownership transferred to the thread
                return CloseHandle(hThread);
            }
            else {
                return FALSE;
            }
        }

        static inline HANDLE Current() { return GetCurrentThread(); }
        static inline DWORD Id() { return GetCurrentThreadId(); }

        static inline HANDLE Open(IN DWORD ThreadId, IN OPTIONAL DWORD Access = THREAD_ALL_ACCESS) { return OpenThread(Access, FALSE, ThreadId); }
        static inline BOOL Close(IN HANDLE hThread) { return CloseHandle(hThread); }
        static inline BOOL Close(IN OUT PHANDLE hThread) {
            if (hThread) {
                BOOL Status = Close(*hThread);
                if (Status) *hThread = NULL;
                return Status;
            } else {
                return FALSE;
            }
        }
        static inline DWORD GetTid(IN HANDLE hThread) { return GetThreadId(hThread); }
        static inline DWORD GetPid(IN HANDLE hThread) { return GetProcessIdOfThread(hThread); }

        static inline DWORD Wait(IN HANDLE hThread, IN DWORD Timeout) { return WaitForSingleObject(hThread, Timeout); }
        static inline BOOL IsTerminated(IN HANDLE hThread) { return Wait(hThread, 0) == WAIT_OBJECT_0; }
        static inline BOOL Terminate(IN HANDLE hThread, IN OPTIONAL DWORD ExitCode = 0) {
#pragma warning(push)
#pragma warning(disable : 6258) // Yes, TerminateThread is unsafe
            return TerminateThread(hThread, ExitCode);
#pragma warning(pop)
        }
        static inline BOOL GetExitCode(IN HANDLE hThread, OUT PDWORD ExitCode) { return GetExitCodeThread(hThread, ExitCode); }
        static inline DWORD GetExitCode(IN HANDLE hThread, IN OPTIONAL DWORD DefaultValue = 0) {
            DWORD ExitCode = 0;
            return GetExitCode(hThread, &ExitCode) ? ExitCode : DefaultValue;
        }

        static inline DWORD Suspend(IN HANDLE hThread) { return SuspendThread(hThread); }
        static inline DWORD Resume(IN HANDLE hThread) { return ResumeThread(hThread); }
        
        static inline BOOL GetContext(IN HANDLE hThread, OUT PCONTEXT Context) { return GetThreadContext(hThread, Context); }
        static inline BOOL SetContext(IN HANDLE hThread, IN PCONTEXT Context) { return SetThreadContext(hThread, Context); }
        
        static inline DWORD QueueApc(IN HANDLE hThread, IN PAPCFUNC ApcFunc, IN OPTIONAL PVOID Arg = NULL) { return QueueUserAPC(ApcFunc, hThread, reinterpret_cast<ULONG_PTR>(Arg)); }
        static inline BOOL Alert() { return NT_SUCCESS(NtApi::NtTestAlert()); }
        static inline BOOL Alert(IN HANDLE hThread) { return NT_SUCCESS(NtApi::NtAlertThread(hThread)); }
        
        static inline BOOL YieldThread() { return SwitchToThread(); }

        static inline INT GetPriority(IN HANDLE hThread) { return GetThreadPriority(hThread); }
        static inline BOOL SetPriority(IN HANDLE hThread, IN OPTIONAL INT Priority = THREAD_PRIORITY_NORMAL) { return SetThreadPriority(hThread, Priority); }
        static inline BOOL SetIdlePriority(IN HANDLE hThread) { return SetPriority(hThread, THREAD_PRIORITY_IDLE); }
        static inline BOOL SetLowestPriority(IN HANDLE hThread) { return SetPriority(hThread, THREAD_PRIORITY_LOWEST); }
        static inline BOOL SetLowerPriority(IN HANDLE hThread) { return SetPriority(hThread, THREAD_PRIORITY_BELOW_NORMAL); }
        static inline BOOL SetNormalPriority(IN HANDLE hThread) { return SetPriority(hThread, THREAD_PRIORITY_NORMAL); }
        static inline BOOL SetHigherPriority(IN HANDLE hThread) { return SetPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL); }
        static inline BOOL SetHighestPriority(IN HANDLE hThread) { return SetPriority(hThread, THREAD_PRIORITY_HIGHEST); }
        static inline BOOL SetRealtimePriority(IN HANDLE hThread) { return SetPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL); }
        static inline BOOL ResetPriority(IN HANDLE hThread) { return SetNormalPriority(hThread); }
        
        static inline SIZE_T SetAffinity(IN HANDLE hThread, IN SIZE_T AffinityMask) { return SetThreadAffinityMask(hThread, AffinityMask); }
        static inline HRESULT SetName(IN HANDLE hThread, IN LPCWSTR Name) { return SetThreadDescription(hThread, Name); }

        static DWORD GetProcessorsCount() {
            SYSTEM_INFO Info = {};
            GetNativeSystemInfo(&Info);
            return Info.dwNumberOfProcessors;
        }
        static SIZE_T GetActiveProcessorsMask() {
            SYSTEM_INFO Info = {};
            GetNativeSystemInfo(&Info);
            return Info.dwActiveProcessorMask;
        }
    };

    class Thread {
    protected:
        LPTHREAD_START_ROUTINE ThreadProc;
        PVOID Arg;
        HANDLE hThread;
        DWORD ThreadId;
    public:
        Thread(const Thread&) = delete;
        Thread(Thread&&) = delete;
        Thread& operator = (const Thread&) = delete;
        Thread& operator = (Thread&&) = delete;

        Thread(LPTHREAD_START_ROUTINE ThreadProc, IN OPTIONAL PVOID Arg = NULL)
            : hThread(NULL), ThreadId(0)
        {
            this->ThreadProc = ThreadProc;
            this->Arg = Arg;
        }

        ~Thread() {
            if (hThread) {
                if (!IsTerminated()) {
                    Threads::Terminate(hThread);
                }
                Threads::Close(&hThread);
            }
        }

        inline VOID Detach() {
            if (hThread) {
                Threads::Close(&hThread);
            }
        }

        inline HANDLE Start(IN BOOL Suspended = FALSE) {
            hThread = CreateThread(NULL, 0, ThreadProc, Arg, Suspended ? CREATE_SUSPENDED : 0, &ThreadId);
            return hThread;
        }

        inline BOOL Stop()
        {
            if (hThread) {
                BOOL Status = Threads::Terminate(hThread);
                if (Status) Detach();
                return Status;
            }
            else {
                return FALSE;
            }
        }

        inline DWORD Wait(DWORD Timeout = INFINITE) const { return Threads::Wait(hThread, Timeout); }
        inline BOOL IsTerminated() const { return Threads::IsTerminated(hThread); }
        inline BOOL GetExitCode(OUT PDWORD ExitCode) const { return Threads::GetExitCode(hThread, ExitCode); }
        inline DWORD GetExitCode(IN OPTIONAL DWORD DefaultValue = 0) const { return Threads::GetExitCode(hThread, DefaultValue); }
        
        inline DWORD Suspend() { return Threads::Suspend(hThread); }
        inline DWORD Resume() { return Threads::Resume(hThread); }

        inline BOOL Alert() { return Threads::Alert(); }
        inline DWORD QueueApc(IN PAPCFUNC ApcFunc, IN OPTIONAL PVOID Arg = NULL) { return Threads::QueueApc(hThread, ApcFunc, Arg); }

        inline BOOL GetContext(OUT PCONTEXT Context) { return Threads::GetContext(hThread, Context); }
        inline BOOL SetContext(IN PCONTEXT Context) { return Threads::SetContext(hThread, Context); }

        inline BOOL YieldThread() { return Threads::YieldThread(); }

        inline HANDLE GetHandle() const { return hThread; }
        inline DWORD GetThreadId() const { return ThreadId; }

        inline INT GetPriority() { return Threads::GetPriority(hThread); }
        inline BOOL SetPriority(IN OPTIONAL INT Priority = THREAD_PRIORITY_NORMAL) { return Threads::SetPriority(hThread, Priority); }
        inline BOOL SetIdlePriority() { return Threads::SetPriority(hThread, THREAD_PRIORITY_IDLE); }
        inline BOOL SetLowestPriority() { return Threads::SetPriority(hThread, THREAD_PRIORITY_LOWEST); }
        inline BOOL SetLowerPriority() { return Threads::SetPriority(hThread, THREAD_PRIORITY_BELOW_NORMAL); }
        inline BOOL SetNormalPriority() { return Threads::SetPriority(hThread, THREAD_PRIORITY_NORMAL); }
        inline BOOL SetHigherPriority() { return Threads::SetPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL); }
        inline BOOL SetHighestPriority() { return Threads::SetPriority(hThread, THREAD_PRIORITY_HIGHEST); }
        inline BOOL SetRealtimePriority() { return Threads::SetPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL); }
        inline BOOL ResetPriority() { return Threads::SetNormalPriority(hThread); }

        inline SIZE_T SetAffinity(IN SIZE_T AffinityMask) { return Threads::SetAffinity(hThread, AffinityMask); }
        inline HRESULT SetName(IN LPCWSTR Name) { return Threads::SetName(hThread, Name); }
    };

    class ThreadWrapped : public Thread {
    private:
        template <class Tuple, size_t... indices>
        static unsigned __stdcall stub(void* arg) noexcept
        {
            const std::unique_ptr<Tuple> fn_vals(static_cast<Tuple*>(arg));
            Tuple& tuple = *fn_vals;
            return std::invoke(std::move(std::get<indices>(tuple))...);
        }

        template <class Tuple, size_t... indices>
        static constexpr auto get_invoke(std::index_sequence<indices...>) noexcept
        {
            return &stub<Tuple, indices...>;
        }
    public:
        template <class Func, class... Args, class = std::enable_if_t<!std::is_same_v<std::_Remove_cvref_t<Func>, Thread>>>
        ThreadWrapped(Func&& func, Args&& ... args)
            : Thread(NULL, NULL)
        {
            using FuncTuple = std::tuple<std::decay_t<Func>, std::decay_t<Args>...>;
            auto decay_copied = std::make_unique<FuncTuple>(std::forward<Func>(func), std::forward<Args>(args)...);
            constexpr auto invoker = get_invoke<FuncTuple>(std::make_index_sequence<1 + sizeof...(Args)>{});
            ThreadProc = reinterpret_cast<LPTHREAD_START_ROUTINE>(invoker);
            Arg = decay_copied.get();
            decay_copied.release(); // Ownership transferred to the thread
        }
    };

    class AbstractThread : public Thread {
    protected:
        static DWORD WINAPI Win32ThreadProc(PVOID Arg)
        {
            auto Self = reinterpret_cast<AbstractThread*>(Arg);
            return Self->ThreadProc();
        }

        AbstractThread() : Thread(Win32ThreadProc, this) {}

    public:
        AbstractThread(const AbstractThread&) = delete;
        AbstractThread(AbstractThread&&) = delete;
        AbstractThread& operator = (const AbstractThread&) = delete;
        AbstractThread& operator = (AbstractThread&&) = delete;

        virtual DWORD ThreadProc() = 0;
    };

    class ThreadPool {
    public:
        typedef VOID (WINAPI *THREADPOOL_CALLBACK)(IN PVOID Arg);
    private:
        PTP_POOL Pool;
        PTP_CLEANUP_GROUP CleanupGroup;
        TP_CALLBACK_ENVIRON TpEnv;

        struct WORK_CALLBACK_INFO {
            THREADPOOL_CALLBACK Callback;
            PVOID Arg;
        };

        static VOID NTAPI TpCallbackWrapper(
            IN OUT PTP_CALLBACK_INSTANCE Instance,
            IN PVOID Context,
            IN OUT PTP_WORK Work
        ) {
            UNREFERENCED_PARAMETER(Instance);
            UNREFERENCED_PARAMETER(Work);

            if (!Context) return;
            
            auto Info = reinterpret_cast<WORK_CALLBACK_INFO*>(Context);
            THREADPOOL_CALLBACK Callback = Info->Callback;
            PVOID Arg = Info->Arg;
            
            HeapFree(GetProcessHeap(), 0, Context);
            
            Callback(Arg);
        }

        template <class Tuple, size_t... indices>
        static VOID NTAPI TpTemplatedWrapper(
            IN OUT PTP_CALLBACK_INSTANCE Instance,
            IN PVOID Context,
            IN OUT PTP_WORK Work
        ) noexcept {
            const std::unique_ptr<Tuple> fn_vals(static_cast<Tuple*>(Context));
            Tuple& tuple = *fn_vals;
            std::invoke(std::move(std::get<indices>(tuple))...);
        }

        template <class Tuple, size_t... indices>
        static constexpr auto get_invoke(std::index_sequence<indices...>) noexcept
        {
            return &TpTemplatedWrapper<Tuple, indices...>;
        }
    public:
        ThreadPool()
            : Pool(NULL), CleanupGroup(NULL), TpEnv({})
        {}

        BOOL CreatePool(IN DWORD ThreadsMinimum, IN DWORD ThreadsMaximum)
        {
            if (Pool) return FALSE; // Already created
            Pool = CreateThreadpool(NULL);
            if (!Pool) return FALSE;

            if (!SetThreadpoolThreadMinimum(Pool, ThreadsMinimum)) {
                CloseThreadpool(Pool);
                Pool = NULL;
                return FALSE;
            }
            SetThreadpoolThreadMaximum(Pool, ThreadsMaximum);

            InitializeThreadpoolEnvironment(&TpEnv);
            SetThreadpoolCallbackPool(&TpEnv, Pool);

            CleanupGroup = CreateThreadpoolCleanupGroup();
            if (!CleanupGroup) {
                CloseThreadpool(Pool);
                Pool = NULL;
                TpEnv = {};
                return FALSE;
            }

            SetThreadpoolCallbackCleanupGroup(&TpEnv, CleanupGroup, NULL);
            return TRUE;
        }

        BOOL IsPoolCreated() const { return Pool != NULL; }

        BOOL DestroyPool(BOOL WaitForCallbacksCompletion)
        {
            if (!Pool) return FALSE; // Not existed
            CloseThreadpoolCleanupGroupMembers(CleanupGroup, !WaitForCallbacksCompletion, NULL);
            CloseThreadpoolCleanupGroup(CleanupGroup);
            CloseThreadpool(Pool);
            Pool = NULL;
            CleanupGroup = NULL;
            TpEnv = {};
            return TRUE;
        }

        static inline VOID Submit(IN PTP_WORK Work) { SubmitThreadpoolWork(Work); }

        static inline PTP_WORK CreateWork(
            IN OPTIONAL PTP_CALLBACK_ENVIRON Env,
            IN PTP_WORK_CALLBACK Callback,
            IN OPTIONAL PVOID Arg = NULL
        ) {
            return CreateThreadpoolWork(Callback, Arg, Env);
        }

        static inline PTP_WORK Queue(
            IN OPTIONAL PTP_CALLBACK_ENVIRON Env,
            IN PTP_WORK_CALLBACK Callback,
            IN OPTIONAL PVOID Arg = NULL
        ) {
            // It is not necessary to close work item, cleanup manager closes it automatically:
            PTP_WORK Work = CreateWork(Env, Callback, Arg);
            if (!Work) return NULL;
            Submit(Work);
            return Work;
        }

        static PTP_WORK Queue(
            IN OPTIONAL PTP_CALLBACK_ENVIRON Env,
            IN THREADPOOL_CALLBACK Callback,
            IN OPTIONAL PVOID Arg = NULL
        ) {
            auto CallbackInfo = reinterpret_cast<WORK_CALLBACK_INFO*>(HeapAlloc(GetProcessHeap(), 0, sizeof(WORK_CALLBACK_INFO)));
            if (!CallbackInfo) return FALSE;
            CallbackInfo->Callback = Callback;
            CallbackInfo->Arg = Arg;
            PTP_WORK Work = Queue(Env, TpCallbackWrapper, CallbackInfo);
            if (!Work)
                HeapFree(GetProcessHeap(), 0, CallbackInfo);
            return Work;
        }

        // Queue to the default threadpool:
        static PTP_WORK QueueDefault(
            IN PTP_WORK_CALLBACK Callback,
            IN OPTIONAL PVOID Arg = NULL
        ) {
            return Queue(NULL, Callback, Arg);
        }

        // Queue to the default threadpool:
        static PTP_WORK QueueDefault(
            IN THREADPOOL_CALLBACK Callback,
            IN OPTIONAL PVOID Arg = NULL
        ) {
            return Queue(NULL, Callback, Arg);
        }

        static PTP_WORK Wait(IN PTP_WORK Work, IN BOOL CancelPendingCallbacks) { WaitForThreadpoolWorkCallbacks(Work, CancelPendingCallbacks); }

        static inline VOID CloseWork(IN PTP_WORK Work) { CloseThreadpoolWork(Work); }

        PTP_WORK CreateWork(
            IN PTP_WORK_CALLBACK Callback,
            IN OPTIONAL PVOID Arg = NULL
        ) {
            return CreateWork(&TpEnv, Callback, Arg);
        }

        PTP_WORK Queue(IN PTP_WORK_CALLBACK Callback, IN OPTIONAL PVOID Arg = NULL)
        {
            return Pool ? Queue(&TpEnv, Callback, Arg) : NULL;
        }

        PTP_WORK Queue(
            IN THREADPOOL_CALLBACK Callback,
            IN OPTIONAL PVOID Arg = NULL
        ) {
            return Pool ? Queue(&TpEnv, Callback, Arg) : NULL;
        }

        template <class Func, class... Args, class = std::enable_if_t<!std::is_same_v<std::_Remove_cvref_t<Func>, Thread>>>
        static PTP_WORK DefaultQueueWrapped(IN OPTIONAL PTP_CALLBACK_ENVIRON Env, Func&& func, Args&& ... args)
        {
            using FuncTuple = std::tuple<std::decay_t<Func>, std::decay_t<Args>...>;
            auto decay_copied = std::make_unique<FuncTuple>(std::forward<Func>(func), std::forward<Args>(args)...);
            constexpr auto invoker = get_invoke<FuncTuple>(std::make_index_sequence<1 + sizeof...(Args)>{});
            PTP_WORK Work = Queue(Env, reinterpret_cast<PTP_WORK_CALLBACK>(invoker), decay_copied.get());
            if (Work)
                decay_copied.release(); // Ownership transferred to the thread
            return Work;
        }

        template <class Func, class... Args, class = std::enable_if_t<!std::is_same_v<std::_Remove_cvref_t<Func>, Thread>>>
        PTP_WORK QueueWrapped(Func&& func, Args&& ... args)
        {
            using FuncTuple = std::tuple<std::decay_t<Func>, std::decay_t<Args>...>;
            auto decay_copied = std::make_unique<FuncTuple>(std::forward<Func>(func), std::forward<Args>(args)...);
            constexpr auto invoker = get_invoke<FuncTuple>(std::make_index_sequence<1 + sizeof...(Args)>{});
            PTP_WORK Work = Queue(&TpEnv, reinterpret_cast<PTP_WORK_CALLBACK>(invoker), decay_copied.get());
            if (Work)
                decay_copied.release(); // Ownership transferred to the thread
            return Work;
        }
    };
}