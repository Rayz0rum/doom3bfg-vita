/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
#pragma hdrstop
#include "precompiled.h"
#if defined(ID_TEST_MATH_NEON) && !defined(__vita__)
#include <xmmintrin.h>
#endif
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
bool Vita_Core3Available(); // vita_main.cpp
extern "C" void enable_runfast(); // math-neon: VFP flush-to-zero + default NaN for this thread
#endif

/*
================================================================================================
Contains the vartious ThreadingClass implementations.
================================================================================================
*/

/*
================================================================================================

        idSysThread

================================================================================================
*/

/*
========================
idSysThread::idSysThread
========================
*/
idSysThread::idSysThread()
    : threadHandle(0)
    , startCore(CORE_ANY)
    , isWorker(false)
    , isRunning(false)
    , isTerminating(false)
    , moreWorkToDo(false)
    , signalWorkerDone(true)
{
}

/*
========================
idSysThread::~idSysThread
========================
*/
idSysThread::~idSysThread()
{
    StopThread(true);
    if (threadHandle) {
        Sys_DestroyThread(threadHandle);
    }
}

/*
========================
idSysThread::StartThread
========================
*/
bool idSysThread::StartThread(const char* name_, core_t core, xthreadPriority priority, int stackSize)
{
    if (isRunning) {
        return false;
    }

    name = name_;
    startCore = core;

    isTerminating = false;

    if (threadHandle) {
        Sys_DestroyThread(threadHandle);
    }

    threadHandle = Sys_CreateThread((xthread_t)ThreadProc, this, priority, name, core, stackSize, false);

    isRunning = true;
    return true;
}

/*
========================
idSysThread::StartWorkerThread
========================
*/
bool idSysThread::StartWorkerThread(const char* name_, core_t core, xthreadPriority priority, int stackSize)
{
    if (isRunning) {
        return false;
    }

    isWorker = true;

    bool result = StartThread(name_, core, priority, stackSize);

    signalWorkerDone.Wait(idSysSignal::WAIT_INFINITE);

    return result;
}

/*
========================
idSysThread::StopThread
========================
*/
void idSysThread::StopThread(bool wait)
{
    if (!isRunning) {
        return;
    }
    if (isWorker) {
        signalMutex.Lock();
        moreWorkToDo = true;
        signalWorkerDone.Clear();
        isTerminating = true;
        signalMoreWorkToDo.Raise();
        signalMutex.Unlock();
    } else {
        isTerminating = true;
    }
    if (wait) {
        WaitForThread();
    }
}

/*
========================
idSysThread::WaitForThread
========================
*/
void idSysThread::WaitForThread()
{
    if (isWorker) {
        signalWorkerDone.Wait(idSysSignal::WAIT_INFINITE);
    } else if (isRunning) {
        Sys_DestroyThread(threadHandle);
        threadHandle = 0;
    }
}

/*
========================
idSysThread::SignalWork
========================
*/
void idSysThread::SignalWork()
{
    if (isWorker) {
        signalMutex.Lock();
        moreWorkToDo = true;
        signalWorkerDone.Clear();
        signalMoreWorkToDo.Raise();
        signalMutex.Unlock();
    }
}

/*
========================
idSysThread::IsWorkDone
========================
*/
bool idSysThread::IsWorkDone()
{
    if (isWorker) {
        // a timeout of 0 will return immediately with true if signaled
        if (signalWorkerDone.Wait(0)) {
            return true;
        }
    }
    return false;
}

/*
========================
idSysThread::ThreadProc
========================
*/
#if defined(__vita__)
// An error raised on a worker (the game/draw thread) is handed to the main
// thread, which raises it again in its own context (common_frame.cpp): the
// stock exit(0) below runs the global destructors from this thread, and the
// game thread's own destructor then waits for itself - a silent freeze.
char vita_threadError[MAX_PRINT_MSG];
volatile bool vita_threadErrorPending = false;
#endif

#if defined(__vita__)
/*
========================
Vita_PlaceThread

The Vita has three cores for games (and a fourth that the system keeps,
unless a plugin such as CapUnlocker lifts the affinity check). BFG's core
hints come from the Xbox 360; here CORE_0x/1x/2x pin to cores 0/1/2 and the
Vita-only value 6 to core 3. The main thread (back end) is pinned to core 0
at creation (vita_main.cpp) and Game/Draw asks for CORE_1B, so the two
busiest threads never share a core.
========================
*/
void Vita_RegisterThread(int sceUid); // vita_main.cpp
void Vita_NameThread(int sceUid, const char* name, int core); // vita_main.cpp: the name and core in the CPU log

static void Vita_PlaceThread(const char* name, core_t core)
{
    Vita_RegisterThread(sceKernelGetThreadId());
#if defined(ID_VITA_NEON_MATH)
    // the FPSCR is per thread: flush-to-zero + default NaN (off by default,
    // see the note in idlib/math/Math.h)
    enable_runfast();
#endif
    int mask = 0;
    switch ((int)core) {
    case CORE_0A:
    case CORE_0B:
        mask = SCE_KERNEL_CPU_MASK_USER_0;
        break;
    case CORE_1A:
    case CORE_1B:
        mask = SCE_KERNEL_CPU_MASK_USER_1;
        break;
    case CORE_2A:
    case CORE_2B:
        mask = SCE_KERNEL_CPU_MASK_USER_2;
        break;
    case 6:
        if (Vita_Core3Available()) {
            mask = SCE_KERNEL_CPU_MASK_SYSTEM; // core 3
        }
        break;
    case 7:
        // the job workers: cores 1 and 2 (see VITA_JOB_THREAD_CORES)
        mask = SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2;
        break;
    case 8:
        // the sound worker (AL_SoundStream.cpp): cores 0 and 2, off Game/Draw's
        mask = SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_2;
        break;
    default:
        // CORE_ANY: any of the game's cores 0-2, never core 3 (which the OS
        // takes when it wants - only work nothing waits for goes there)
        mask = SCE_KERNEL_CPU_MASK_USER_ALL;
        break;
    }
    Vita_NameThread(sceKernelGetThreadId(), name, mask == SCE_KERNEL_CPU_MASK_SYSTEM ? 3 : (mask == SCE_KERNEL_CPU_MASK_USER_2 ? 2 : (mask == SCE_KERNEL_CPU_MASK_USER_1 ? 1 : (mask == SCE_KERNEL_CPU_MASK_USER_0 ? 0 : -1))));
    if (mask == 0) {
        return;
    }
    const int r = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
    idLib::Printf("vita: thread '%s' pinned to %s%s\n", name,
        mask == SCE_KERNEL_CPU_MASK_SYSTEM ? "core 3" : (mask == SCE_KERNEL_CPU_MASK_USER_2 ? "core 2" : (mask == SCE_KERNEL_CPU_MASK_USER_1 ? "core 1" : (mask == SCE_KERNEL_CPU_MASK_USER_0 ? "core 0" : (mask == (SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2) ? "cores 1-2" : (mask == (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_2) ? "cores 0 and 2" : "cores 0-2"))))),
        r < 0 ? va(" - refused (0x%08X), left to the scheduler", (unsigned int)r) : "");
}
#endif

int idSysThread::ThreadProc(idSysThread* thread)
{
    int retVal = 0;
#if defined(__vita__)
    Vita_PlaceThread(thread->name.c_str(), thread->startCore);
#elif defined(ID_TEST_MATH_NEON)
    _mm_setcsr(_mm_getcsr() | 0x8040); // flush-to-zero + denormals-are-zero, like the Vita's RunFast
#endif

    try {
        if (thread->isWorker) {
            for (;;) {
                thread->signalMutex.Lock();
                if (thread->moreWorkToDo) {
                    thread->moreWorkToDo = false;
                    thread->signalMoreWorkToDo.Clear();
                    thread->signalMutex.Unlock();
                } else {
                    thread->signalWorkerDone.Raise();
                    thread->signalMutex.Unlock();
                    thread->signalMoreWorkToDo.Wait(idSysSignal::WAIT_INFINITE);
                    continue;
                }

                if (thread->isTerminating) {
                    break;
                }

#if defined(__vita__)
                try {
                    retVal = thread->Run();
                } catch (idException& ex) {
                    idStr::Copynz(vita_threadError, ex.GetError(), sizeof(vita_threadError));
                    vita_threadErrorPending = true;
                }
#else
                retVal = thread->Run();
#endif
            }
            thread->signalWorkerDone.Raise();
        } else {
            retVal = thread->Run();
        }
    } catch (idException& ex) {
        idLib::Warning("Fatal error in thread %s: %s", thread->GetName(), ex.GetError());

        // We don't handle threads terminating unexpectedly very well, so just terminate the whole process
        exit(0);
    }

    thread->isRunning = false;

    return retVal;
}

/*
========================
idSysThread::Run
========================
*/
int idSysThread::Run()
{
    // The Run() is not pure virtual because on destruction of a derived class
    // the virtual function pointer will be set to NULL before the idSysThread
    // destructor actually stops the thread.
    return 0;
}

/*
================================================================================================

        test

================================================================================================
*/

/*
================================================
idMyThread test class.
================================================
*/
class idMyThread : public idSysThread {
public:
    virtual int Run()
    {
        // run threaded code here
        return 0;
    }
    // specify thread data here
};

/*
========================
TestThread
========================
*/
void TestThread()
{
    idMyThread thread;
    thread.StartThread("myThread", CORE_ANY);
}

/*
========================
TestWorkers
========================
*/
void TestWorkers()
{
    idSysWorkerThreadGroup<idMyThread> workers("myWorkers", 4);
    for (;;) {
        for (int i = 0; i < workers.GetNumThreads(); i++) {
            // workers.GetThread( i )-> // setup work for this thread
        }
        workers.SignalWorkAndWait();
    }
}
