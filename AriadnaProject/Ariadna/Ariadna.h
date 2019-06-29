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

#ifndef _DEQUE_
    #include <deque>
#endif

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

}