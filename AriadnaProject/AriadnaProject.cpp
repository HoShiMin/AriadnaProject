#include <cstdio>

#include "Ariadna/Ariadna.h"

DWORD WINAPI UmsThread(PVOID Arg)
{
    for (int i = 0; i < 3; ++i) {
        printf("UMS Thread %i\r\n", static_cast<int>(GetCurrentThreadId()));
        Sleep(100);
    }
    printf("UMS Thread %i finished\r\n", static_cast<int>(GetCurrentThreadId()));
    return 0;
}

int main()
{
    using namespace Ariadna;

    UmsScheduler& Scheduler = UmsScheduler::GetInstance();
    BOOL Status = Scheduler.StartUmsScheduler();
    if (!Status) {
        printf("Unable to run UMS\r\n");
        return 0;
    }

    printf("UMS started\r\n");

    Scheduler.StartThread(UmsThread);
    Scheduler.StartThread(UmsThread);
    Scheduler.StartThread(UmsThread);
    Scheduler.StartThread(UmsThread);

    Sleep(1300);

    Scheduler.StopUmsScheduler();
    printf("UMS finished\r\n");

    MSG Message = {};
    while (GetMessage(&Message, NULL, 0, 0)) {
        TranslateMessage(&Message);
        DispatchMessage(&Message);
    }

    return 0;
}