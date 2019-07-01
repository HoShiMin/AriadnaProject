#include <cstdio>
#include <thread>
#include <string>

#include "Ariadna/Ariadna.h"

using namespace Ariadna;

void UmsTesting()
{
    UmsScheduler& Scheduler = UmsScheduler::GetInstance();
    BOOL Status = Scheduler.StartUmsScheduler();
    if (!Status) {
        printf("Unable to run UMS\r\n");
        return;
    }

    printf("UMS started\r\n");

    for (int i = 0; i < 5; ++i) {
        Scheduler.StartThread([](PVOID) -> DWORD {
            for (int i = 0; i < 3; ++i) {
                printf("UMS Thread %i\r\n", static_cast<int>(GetCurrentThreadId()));
                Sleep(100);
            }
            printf("UMS Thread %i finished\r\n", static_cast<int>(GetCurrentThreadId()));
            return 0;
        });
    }

    Sleep(1300);

    Scheduler.StopUmsScheduler();
    printf("UMS finished\r\n");
}

void FibersTesting()
{
    Fibers::CallInFiber([](PVOID) {
        printf("Fiber %p in thread %i\r\n", Fibers::Current(), GetCurrentThreadId());
    });

    printf("Non-fiber main thread %i\r\n", GetCurrentThreadId());
}

void ThreadsTesting()
{
    HANDLE hThread = Threads::StartThread([](PVOID) -> DWORD {
        while (true) {
            printf("Thread %i\r\n", Threads::Id());
            Sleep(1000);
            Threads::Alert(); // Works
        }
    });

    BOOL Status = Threads::QueueApc(hThread, [](ULONG_PTR Arg) {
        printf("APC in thread %i\r\n", Threads::Id());
    });
    Threads::Alert(hThread); // No, it doesn't works at all...
}

class CustomThread : public AbstractThread {
private:
    int SampleValue;
protected:
    DWORD ThreadProc() override
    {
        printf("Hi from a CustomThread!\r\n");
        printf("- SampleValue = %i\r\n", SampleValue);
        return SampleValue;
    }
public:
    CustomThread(int Value) {
        SampleValue = Value;
    }
};

void TestThreadClass()
{
    CustomThread thread(1337);
    thread.Start();
    thread.Wait();
    printf("CustomThread is finished with value %i!\r\n", thread.GetExitCode());
}


int main()
{
    ThreadPool::DefaultQueueWrapped(NULL, [&](int a, int b) {
        printf("[%u] Hi from threadpool: %i\r\n", Threads::Id(), a + b);
    }, 10, 20);

    ThreadPool CustomThreadPool;
    CustomThreadPool.CreatePool(8, 16);
    CustomThreadPool.QueueWrapped([&](int a, const std::string& b) {
        printf("[%u] Hi from custom threadpool: %i, %s\r\n", Threads::Id(), a, b.c_str());
    }, 10, "Sample text");


    volatile int i = 1337; // Value that will be pass to another thread

    // Run async task:
    Threads::Async([&]() -> DWORD { 
        Sleep(1000);
        printf("[%u] External value = %i\r\n", Threads::Id(), i);
        return 0;
    });

    i = 0; // Changing the 'global' value

    printf("[%u] Value 'i' changed to %i\r\n", Threads::Id(), i);

    // To prevent an app closing:
    MSG Message = {};
    while (GetMessage(&Message, NULL, 0, 0)) {
        TranslateMessage(&Message);
        DispatchMessage(&Message);
    }

    return 0;
}