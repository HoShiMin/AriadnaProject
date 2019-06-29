# 🧶 The Ariadna Project
This library is an example of UMS-threads using and helps you to start with usermode threads scheduling.  

#### How to:
```cpp
#include <Windows.h>
#include "Ariadna.h"

DWORD WINAPI UmsThread(PVOID Arg)
{
    // Your code here
}

int main()
{
    using namespace Ariadna;
    UmsScheduler& Scheduler = UmsScheduler::GetInstance();
    
    // Initializing:
    Scheduler.StartUmsSheduler();
    
    // Starting threads:
    for (unsigned int i = 0; i < 5; ++i) {
        Scheduler.StartThread(UmsThread);
    }
    
    ...
    
    // Stopping scheduler:
    Scheduler.StopUmsScheduler();
    
    return 0;
}
```
