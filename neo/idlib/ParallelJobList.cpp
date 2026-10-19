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
#include "ParallelJobList.h"
#include "../sys/vita/vita_zones.h" // the game/draw thread's tree of passes (VitaBench.cpp)

/*
================================================================================================

        Job and Job-List names

================================================================================================
*/

const char* jobNames[] = {
    ASSERT_ENUM_STRING(JOBLIST_RENDERER_FRONTEND, 0),
    ASSERT_ENUM_STRING(JOBLIST_RENDERER_BACKEND, 1),
    ASSERT_ENUM_STRING(JOBLIST_UTILITY, 9),
};

static const int MAX_REGISTERED_JOBS = 128;
struct registeredJob {
    jobRun_t function;
    const char* name;
} registeredJobs[MAX_REGISTERED_JOBS];
static int numRegisteredJobs;

const char* GetJobListName(jobListId_t id)
{
    // jobNames holds three names, but the ids are 0, 1 and 9 (and any other
    // number up to MAX_JOBLISTS): indexing it with the id read past its end.
    // The classic DOOM frame copy waits on a JOBLIST_UTILITY (9) list, and
    // formatting that garbage pointer into "wait: job list '%s'" crashed the
    // first DOOM I frame (strlen on 0xFFFFFFF8).
    switch (id) {
    case JOBLIST_RENDERER_FRONTEND:
        return jobNames[0];
    case JOBLIST_RENDERER_BACKEND:
        return jobNames[1];
    case JOBLIST_UTILITY:
        return jobNames[2];
    default:
        break;
    }
    static const char* const others[MAX_JOBLISTS] = {
        "job list 0", "job list 1", "job list 2", "job list 3", "job list 4", "job list 5", "job list 6", "job list 7",
        "job list 8", "job list 9", "job list 10", "job list 11", "job list 12", "job list 13", "job list 14", "job list 15",
        "job list 16", "job list 17", "job list 18", "job list 19", "job list 20", "job list 21", "job list 22", "job list 23",
        "job list 24", "job list 25", "job list 26", "job list 27", "job list 28", "job list 29", "job list 30", "job list 31"
    };
    return ((int)id >= 0 && (int)id < MAX_JOBLISTS) ? others[id] : "job list ?";
}

/*
========================
IsRegisteredJob
========================
*/
static bool IsRegisteredJob(jobRun_t function)
{
    for (int i = 0; i < numRegisteredJobs; i++) {
        if (registeredJobs[i].function == function) {
            return true;
        }
    }
    return false;
}

/*
========================
RegisterJob
========================
*/
void RegisterJob(jobRun_t function, const char* name)
{
    if (IsRegisteredJob(function)) {
        return;
    }
    registeredJobs[numRegisteredJobs].function = function;
    registeredJobs[numRegisteredJobs].name = name;
    numRegisteredJobs++;
}

/*
========================
GetJobName
========================
*/
const char* GetJobName(jobRun_t function)
{
    for (int i = 0; i < numRegisteredJobs; i++) {
        if (registeredJobs[i].function == function) {
            return registeredJobs[i].name;
        }
    }
    return "unknown";
}

/*
========================
idParallelJobRegistration::idParallelJobRegistration
========================
*/
idParallelJobRegistration::idParallelJobRegistration(jobRun_t function, const char* name)
{
    RegisterJob(function, name);
}

int globalSpuLocalStoreActive;
void* globalSpuLocalStoreStart;
void* globalSpuLocalStoreEnd;
idSysMutex globalSpuLocalStoreMutex;

/*
================================================================================================

        PS3

================================================================================================
*/

/*
================================================================================================

idParallelJobList_Threads

================================================================================================
*/

#if defined(__vita__)
int vita_longJobs = 0, vita_longJobsPrinted = 0;
#endif
// jobs run per job thread (slot 31: the threads that wait on a list and help)
// and the latest jobs over 3 ms, for the benchmark (VitaBench.cpp)
int vita_benchJobCount[32];
uint64 vita_benchJobUs[32];
// The tail of a wait: once the waiting thread finds nothing left to take, it
// idles until the jobs still running elsewhere end (run 7: 3.1 ms a frame in
// the slow fights on the front end's list). Each thread notes when its
// current job started; the tail is charged to the thread holding the oldest
// one - the straggler - so the log says who the frame waited on (a core 1-2
// worker sharing its core, or the core-0 helper under the main thread and
// the OpenAL mixer).
volatile uint64 vita_jobRunningSince[32];
uint64 vita_benchTailWaitUs[32];
int vita_benchTailWaits[32];
vitaBenchSlowJob_t vita_benchSlowJobs[VITA_BENCH_SLOW_JOBS];
idSysInterlockedInteger vita_benchSlowJobNext;
// every job by type (VitaBench.cpp prints them per segment)
vitaBenchJobType_t vita_benchJobTypes[VITA_BENCH_JOB_TYPES];
volatile int vita_benchNumJobTypes = 0;
static char vitaJobNodeNames[VITA_BENCH_JOB_TYPES][64];
static volatile int vitaJobTypeLock = 0;

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
bool VitaGD_OnThread(); // VitaBench.cpp: running on the game/draw thread
#endif

static int Vita_BenchJobTypeIndex(jobRun_t function)
{
    const int n = vita_benchNumJobTypes;
    for (int i = 0; i < n; i++) {
        if (vita_benchJobTypes[i].function == (const void*)function) {
            return i;
        }
    }
    // a type not seen yet: added under a spin lock (once per type)
    while (__sync_lock_test_and_set(&vitaJobTypeLock, 1) != 0) {
    }
    int k = -1;
    for (int i = 0; i < vita_benchNumJobTypes; i++) {
        if (vita_benchJobTypes[i].function == (const void*)function) {
            k = i;
        }
    }
    if (k < 0 && vita_benchNumJobTypes < VITA_BENCH_JOB_TYPES) {
        k = vita_benchNumJobTypes;
        vitaBenchJobType_t& jt = vita_benchJobTypes[k];
        memset(&jt, 0, sizeof(jt));
        jt.function = (const void*)function;
        jt.name = GetJobName(function);
        idStr::snPrintf(vitaJobNodeNames[k], sizeof(vitaJobNodeNames[k]), "job: %s", jt.name);
        __sync_synchronize();
        vita_benchNumJobTypes = k + 1;
    }
    __sync_lock_release(&vitaJobTypeLock);
    return k;
}

// the name of a job list's wait in the game/draw thread's tree
static const char* Vita_WaitNodeName(int id)
{
    static char names[64][64];
    if (id < 0 || id >= 64) {
        return "wait: job list";
    }
    if (names[id][0] == 0) {
        idStr::snPrintf(names[id], sizeof(names[id]), "wait: job list '%s'", GetJobListName((jobListId_t)id));
    }
    return names[id];
}
#if defined(__vita__)
uint64 vita_longJobWorstUs = 0;
const char* vita_longJobWorstName = "";
#endif
static idCVar jobs_longJobMicroSec("jobs_longJobMicroSec", "10000", CVAR_INTEGER, "print a warning for jobs that take more than this number of microseconds");

const static int MAX_THREADS = 32;

struct threadJobListState_t {
    threadJobListState_t()
        : jobList(NULL)
        , version(0xFFFFFFFF)
        , signalIndex(0)
        , lastJobIndex(0)
        , nextJobIndex(-1)
    {
    }
    threadJobListState_t(int _version)
        : jobList(NULL)
        , version(_version)
        , signalIndex(0)
        , lastJobIndex(0)
        , nextJobIndex(-1)
    {
    }
    idParallelJobList_Threads* jobList;
    int version;
    int signalIndex;
    int lastJobIndex;
    int nextJobIndex;
};

struct threadStats_t {
    unsigned int numExecutedJobs;
    unsigned int numExecutedSyncs;
    uint64 submitTime;
    uint64 startTime;
    uint64 endTime;
    uint64 waitTime;
    uint64 threadExecTime[MAX_THREADS];
    uint64 threadTotalTime[MAX_THREADS];
};

class idParallelJobList_Threads {
public:
    idParallelJobList_Threads(jobListId_t id, jobListPriority_t priority, unsigned int maxJobs, unsigned int maxSyncs);
    ~idParallelJobList_Threads();

    //------------------------
    // These are called from the one thread that manages this list.
    //------------------------
    ID_INLINE void AddJob(jobRun_t function, void* data);
    ID_INLINE void InsertSyncPoint(jobSyncType_t syncType);
    void Submit(idParallelJobList_Threads* waitForJobList_, int parallelism);
    void Wait();
    bool TryWait();
    bool IsSubmitted() const;

    unsigned int GetNumExecutedJobs() const
    {
        return threadStats.numExecutedJobs;
    }
    unsigned int GetNumSyncs() const
    {
        return threadStats.numExecutedSyncs;
    }
    uint64 GetSubmitTimeMicroSec() const
    {
        return threadStats.submitTime;
    }
    uint64 GetStartTimeMicroSec() const
    {
        return threadStats.startTime;
    }
    uint64 GetFinishTimeMicroSec() const
    {
        return threadStats.endTime;
    }
    uint64 GetWaitTimeMicroSec() const
    {
        return threadStats.waitTime;
    }
    uint64 GetTotalProcessingTimeMicroSec() const;
    uint64 GetTotalWastedTimeMicroSec() const;
    uint64 GetUnitProcessingTimeMicroSec(int unit) const;
    uint64 GetUnitWastedTimeMicroSec(int unit) const;

    jobListId_t GetId() const
    {
        return listId;
    }
    jobListPriority_t GetPriority() const
    {
        return listPriority;
    }
    int GetVersion()
    {
        return version.GetValue();
    }

    bool WaitForOtherJobList();

    //------------------------
    // This is thread safe and called from the job threads.
    //------------------------
    enum runResult_t {
        RUN_OK = 0,
        RUN_PROGRESS = BIT(0),
        RUN_DONE = BIT(1),
        RUN_STALLED = BIT(2)
    };

    int RunJobs(unsigned int threadNum, threadJobListState_t& state, bool singleJob);

private:
    static const int NUM_DONE_GUARDS = 4; // cycle through 4 guards so we can cyclicly chain job lists

    bool threaded;
    bool done;
    bool hasSignal;
    jobListId_t listId;
    jobListPriority_t listPriority;
    unsigned int maxJobs;
    unsigned int maxSyncs;
    unsigned int numSyncs;
    int lastSignalJob;
    idSysInterlockedInteger* waitForGuard;
    idSysInterlockedInteger doneGuards[NUM_DONE_GUARDS];
    int currentDoneGuard;
    idSysInterlockedInteger version;
    struct job_t {
        jobRun_t function;
        void* data;
        int executed;
    };
    idList<job_t, TAG_JOBLIST> jobList;
    idList<idSysInterlockedInteger, TAG_JOBLIST> signalJobCount;
    idSysInterlockedInteger currentJob;
    idSysInterlockedInteger fetchLock;
    idSysInterlockedInteger numThreadsExecuting;

    threadStats_t deferredThreadStats;
    threadStats_t threadStats;

    int RunJobsInternal(unsigned int threadNum, threadJobListState_t& state, bool singleJob);

    static void Nop(void* data) { }

    static int JOB_SIGNAL;
    static int JOB_SYNCHRONIZE;
    static int JOB_LIST_DONE;
};

int idParallelJobList_Threads::JOB_SIGNAL;
int idParallelJobList_Threads::JOB_SYNCHRONIZE;
int idParallelJobList_Threads::JOB_LIST_DONE;

/*
========================
idParallelJobList_Threads::idParallelJobList_Threads
========================
*/
idParallelJobList_Threads::idParallelJobList_Threads(jobListId_t id, jobListPriority_t priority, unsigned int maxJobs, unsigned int maxSyncs)
    : threaded(true)
    , done(true)
    , hasSignal(false)
    , listId(id)
    , listPriority(priority)
    , numSyncs(0)
    , lastSignalJob(0)
    , waitForGuard(NULL)
    , currentDoneGuard(0)
    , jobList()
{

    assert(listPriority != JOBLIST_PRIORITY_NONE);

    this->maxJobs = maxJobs;
    this->maxSyncs = maxSyncs;
    jobList.AssureSize(maxJobs + maxSyncs * 2 + 1); // syncs go in as dummy jobs and one more to update the doneCount
    jobList.SetNum(0);
    signalJobCount.AssureSize(maxSyncs + 1); // need one extra for submit
    signalJobCount.SetNum(0);

    memset(&deferredThreadStats, 0, sizeof(threadStats_t));
    memset(&threadStats, 0, sizeof(threadStats_t));
}

/*
========================
idParallelJobList_Threads::~idParallelJobList_Threads
========================
*/
idParallelJobList_Threads::~idParallelJobList_Threads()
{
    Wait();
}

/*
========================
idParallelJobList_Threads::AddJob
========================
*/
ID_INLINE void idParallelJobList_Threads::AddJob(jobRun_t function, void* data)
{
    assert(done);
#if defined(_DEBUG)
    // make sure there isn't already a job with the same function and data in the list
    if (jobList.Num() < 1000) // don't do this N^2 slow check on big lists
    {
        for (int i = 0; i < jobList.Num(); i++) {
            assert(jobList[i].function != function || jobList[i].data != data);
        }
    }
#endif
    if (1) // JDC: this never worked in tech5!  !jobList.IsFull() ) {
    {
        job_t& job = jobList.Alloc();
        job.function = function;
        job.data = data;
        job.executed = 0;
    } else {
        // debug output to show us what is overflowing
        int currentJobCount[MAX_REGISTERED_JOBS] = {};

        for (int i = 0; i < jobList.Num(); ++i) {
            const char* jobName = GetJobName(jobList[i].function);
            for (int j = 0; j < numRegisteredJobs; ++j) {
                if (jobName == registeredJobs[j].name) {
                    currentJobCount[j]++;
                    break;
                }
            }
        }

        // print the quantity of each job type
        for (int i = 0; i < numRegisteredJobs; ++i) {
            if (currentJobCount[i] > 0) {
                idLib::Printf("Job: %s, # %d", registeredJobs[i].name, currentJobCount[i]);
            }
        }
        idLib::Error("Can't add job '%s', too many jobs %d", GetJobName(function), jobList.Num());
    }
}

/*
========================
idParallelJobList_Threads::InsertSyncPoint
========================
*/
ID_INLINE void idParallelJobList_Threads::InsertSyncPoint(jobSyncType_t syncType)
{
    assert(done);
    switch (syncType) {
    case SYNC_SIGNAL: {
        assert(!hasSignal);
        if (jobList.Num()) {
            assert(!hasSignal);
            signalJobCount.Alloc();
            signalJobCount[signalJobCount.Num() - 1].SetValue(jobList.Num() - lastSignalJob);
            lastSignalJob = jobList.Num();
            job_t& job = jobList.Alloc();
            job.function = Nop;
            job.data = &JOB_SIGNAL;
            hasSignal = true;
        }
        break;
    }
    case SYNC_SYNCHRONIZE: {
        if (hasSignal) {
            job_t& job = jobList.Alloc();
            job.function = Nop;
            job.data = &JOB_SYNCHRONIZE;
            hasSignal = false;
            numSyncs++;
        }
        break;
    }
    }
}

/*
========================
idParallelJobList_Threads::Submit
========================
*/
void idParallelJobList_Threads::Submit(idParallelJobList_Threads* waitForJobList, int parallelism)
{
    assert(done);
    assert(numSyncs <= maxSyncs);
    assert((unsigned int)jobList.Num() <= maxJobs + numSyncs * 2);
    assert(fetchLock.GetValue() == 0);

    done = false;
    currentJob.SetValue(0);

    memset(&deferredThreadStats, 0, sizeof(deferredThreadStats));
    deferredThreadStats.numExecutedJobs = jobList.Num() - numSyncs * 2;
    deferredThreadStats.numExecutedSyncs = numSyncs;
    deferredThreadStats.submitTime = Sys_Microseconds();
    deferredThreadStats.startTime = 0;
    deferredThreadStats.endTime = 0;
    deferredThreadStats.waitTime = 0;

    if (jobList.Num() == 0) {
        return;
    }

    if (waitForJobList != NULL) {
        waitForGuard = &waitForJobList->doneGuards[waitForJobList->currentDoneGuard];
    } else {
        waitForGuard = NULL;
    }

    currentDoneGuard = (currentDoneGuard + 1) & (NUM_DONE_GUARDS - 1);
    doneGuards[currentDoneGuard].SetValue(1);

    signalJobCount.Alloc();
    signalJobCount[signalJobCount.Num() - 1].SetValue(jobList.Num() - lastSignalJob);

    job_t& job = jobList.Alloc();
    job.function = Nop;
    job.data = &JOB_LIST_DONE;

    if (threaded) {
        // hand over to the manager
        void SubmitJobList(idParallelJobList_Threads * jobList, int parallelism);
        SubmitJobList(this, parallelism);
    } else {
        // run all the jobs right here
        threadJobListState_t state(GetVersion());
        RunJobs(0, state, false);
    }
}

/*
========================
idParallelJobList_Threads::Wait
========================
*/
void idParallelJobList_Threads::Wait()
{
    if (jobList.Num() > 0) {
        // don't lock up but return if the job list was never properly submitted
        if (!verify(!done && signalJobCount.Num() > 0)) {
            return;
        }

        bool waited = false;
        uint64 waitStart = Sys_Microseconds();
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
        // on the game/draw thread: a node of its tree, with the jobs it runs
        // itself below it (the node's own time is the idle wait)
        vitaGDScope_t gdWait(Vita_WaitNodeName(GetId()));
#endif

#if defined(__vita__)
        // Help instead of spinning: the waiting thread (Game/Draw waiting on
        // the front end's model, light and shadow jobs) takes jobs from the
        // list like a worker would - the fetch is atomic and each worker
        // keeps its own state - so its core works through the queue too.
        // Slot 31 of the per-thread stats is free (MAX_THREADS is 32).
        {
            threadJobListState_t helper(version.GetValue());
            helper.jobList = this;
            while (signalJobCount[signalJobCount.Num() - 1].GetValue() > 0) {
                const int r = RunJobs(MAX_THREADS - 1, helper, false);
                if ((r & RUN_DONE) != 0) {
                    break; // nothing left to fetch; the rest is being executed elsewhere
                }
                if ((r & RUN_PROGRESS) == 0) {
                    Sys_Yield();
                }
                waited = true;
            }
        }
#endif
        {
            // the tail (see vita_jobRunningSince): charged to the straggler
            const uint64 tailStart = Sys_Microseconds();
            int straggler = -1;
            uint64 oldest = 0;
            for (int t = 0; t < MAX_THREADS - 1; t++) {
                const uint64 since = vita_jobRunningSince[t];
                if (since != 0 && (straggler < 0 || since < oldest)) {
                    straggler = t;
                    oldest = since;
                }
            }
            bool tail = false;
            while (signalJobCount[signalJobCount.Num() - 1].GetValue() > 0) {
                Sys_Yield();
                waited = true;
                tail = true;
            }
            if (tail && straggler >= 0) {
                vita_benchTailWaitUs[straggler] += Sys_Microseconds() - tailStart;
                vita_benchTailWaits[straggler]++;
            }
        }
        version.Increment();
        while (numThreadsExecuting.GetValue() > 0) {
            Sys_Yield();
            waited = true;
        }

        jobList.SetNum(0);
        signalJobCount.SetNum(0);
        numSyncs = 0;
        lastSignalJob = 0;

        uint64 waitEnd = Sys_Microseconds();
        deferredThreadStats.waitTime = waited ? (waitEnd - waitStart) : 0;
    }
    memcpy(&threadStats, &deferredThreadStats, sizeof(threadStats));
    done = true;
}

/*
========================
idParallelJobList_Threads::TryWait
========================
*/
bool idParallelJobList_Threads::TryWait()
{
    if (jobList.Num() == 0 || signalJobCount[signalJobCount.Num() - 1].GetValue() <= 0) {
        Wait();
        return true;
    }
    return false;
}

/*
========================
idParallelJobList_Threads::IsSubmitted
========================
*/
bool idParallelJobList_Threads::IsSubmitted() const
{
    return !done;
}

/*
========================
idParallelJobList_Threads::GetTotalProcessingTimeMicroSec
========================
*/
uint64 idParallelJobList_Threads::GetTotalProcessingTimeMicroSec() const
{
    uint64 total = 0;
    for (int unit = 0; unit < MAX_THREADS; unit++) {
        total += threadStats.threadExecTime[unit];
    }
    return total;
}

/*
========================
idParallelJobList_Threads::GetTotalWastedTimeMicroSec
========================
*/
uint64 idParallelJobList_Threads::GetTotalWastedTimeMicroSec() const
{
    uint64 total = 0;
    for (int unit = 0; unit < MAX_THREADS; unit++) {
        total += threadStats.threadTotalTime[unit] - threadStats.threadExecTime[unit];
    }
    return total;
}

/*
========================
idParallelJobList_Threads::GetUnitProcessingTimeMicroSec
========================
*/
uint64 idParallelJobList_Threads::GetUnitProcessingTimeMicroSec(int unit) const
{
    if (unit < 0 || unit >= MAX_THREADS) {
        return 0;
    }
    return threadStats.threadExecTime[unit];
}

/*
========================
idParallelJobList_Threads::GetUnitWastedTimeMicroSec
========================
*/
uint64 idParallelJobList_Threads::GetUnitWastedTimeMicroSec(int unit) const
{
    if (unit < 0 || unit >= MAX_THREADS) {
        return 0;
    }
    return threadStats.threadTotalTime[unit] - threadStats.threadExecTime[unit];
}

#ifndef _DEBUG
volatile float longJobTime;
volatile jobRun_t longJobFunc;
volatile void* longJobData;
#endif

/*
========================
idParallelJobList_Threads::RunJobsInternal
========================
*/
int idParallelJobList_Threads::RunJobsInternal(unsigned int threadNum, threadJobListState_t& state, bool singleJob)
{
    if (state.version != version.GetValue()) {
        // trying to run an old version of this list that is already done
        return RUN_DONE;
    }

    assert(threadNum < MAX_THREADS);

    if (deferredThreadStats.startTime == 0) {
        deferredThreadStats.startTime = Sys_Microseconds(); // first time any thread is running jobs from this list
    }

    int result = RUN_OK;

    do {

        // run through all signals and syncs before the last job that has been or is being executed
        // this loop is really an optimization to minimize the time spent in the fetchLock section below
        for (; state.lastJobIndex < (int)currentJob.GetValue() && state.lastJobIndex < jobList.Num(); state.lastJobIndex++) {
            if (jobList[state.lastJobIndex].data == &JOB_SIGNAL) {
                state.signalIndex++;
                assert(state.signalIndex < signalJobCount.Num());
            } else if (jobList[state.lastJobIndex].data == &JOB_SYNCHRONIZE) {
                assert(state.signalIndex > 0);
                if (signalJobCount[state.signalIndex - 1].GetValue() > 0) {
                    // stalled on a synchronization point
                    return (result | RUN_STALLED);
                }
            } else if (jobList[state.lastJobIndex].data == &JOB_LIST_DONE) {
                if (signalJobCount[signalJobCount.Num() - 1].GetValue() > 0) {
                    // stalled on a synchronization point
                    return (result | RUN_STALLED);
                }
            }
        }

        // try to lock to fetch a new job
        if (fetchLock.Increment() == 1) {

            // grab a new job
            state.nextJobIndex = currentJob.Increment() - 1;

            // run through any remaining signals and syncs (this should rarely iterate more than once)
            for (; state.lastJobIndex <= state.nextJobIndex && state.lastJobIndex < jobList.Num(); state.lastJobIndex++) {
                if (jobList[state.lastJobIndex].data == &JOB_SIGNAL) {
                    state.signalIndex++;
                    assert(state.signalIndex < signalJobCount.Num());
                } else if (jobList[state.lastJobIndex].data == &JOB_SYNCHRONIZE) {
                    assert(state.signalIndex > 0);
                    if (signalJobCount[state.signalIndex - 1].GetValue() > 0) {
                        // return this job to the list
                        currentJob.Decrement();
                        // release the fetch lock
                        fetchLock.Decrement();
                        // stalled on a synchronization point
                        return (result | RUN_STALLED);
                    }
                } else if (jobList[state.lastJobIndex].data == &JOB_LIST_DONE) {
                    if (signalJobCount[signalJobCount.Num() - 1].GetValue() > 0) {
                        // return this job to the list
                        currentJob.Decrement();
                        // release the fetch lock
                        fetchLock.Decrement();
                        // stalled on a synchronization point
                        return (result | RUN_STALLED);
                    }
                    // decrement the done count
                    doneGuards[currentDoneGuard].Decrement();
                }
            }
            // release the fetch lock
            fetchLock.Decrement();
        } else {
            // release the fetch lock
            fetchLock.Decrement();
            // another thread is fetching right now so consider stalled
            return (result | RUN_STALLED);
        }

        // if at the end of the job list we're done
        if (state.nextJobIndex >= jobList.Num()) {
            return (result | RUN_DONE);
        }

        // execute the next job
        {
            uint64 jobStart = Sys_Microseconds();
            vita_jobRunningSince[threadNum] = jobStart;
            const int jobType = Vita_BenchJobTypeIndex(jobList[state.nextJobIndex].function);
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
            const bool onGD = (threadNum == MAX_THREADS - 1) && VitaGD_OnThread();
            const int gdParent = onGD && jobType >= 0 ? VitaGD_PushAt(vitaJobNodeNames[jobType], jobStart) : -1;
#endif

            jobList[state.nextJobIndex].function(jobList[state.nextJobIndex].data);
            jobList[state.nextJobIndex].executed = 1;

            uint64 jobEnd = Sys_Microseconds();
            vita_jobRunningSince[threadNum] = 0;
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
            VitaGD_PopAt(gdParent, jobEnd);
#else
            const bool onGD = false;
#endif
            if (jobType >= 0) {
                vitaBenchJobType_t& jt = vita_benchJobTypes[jobType];
                const int jus = (int)(jobEnd - jobStart);
                __sync_fetch_and_add(&jt.count, 1);
                __sync_fetch_and_add(&jt.us, (uint64)jus);
                if (onGD) {
                    __sync_fetch_and_add(&jt.gdCount, 1);
                    __sync_fetch_and_add(&jt.gdUs, (uint64)jus);
                }
                if (jus > jt.maxUs) {
                    jt.maxUs = jus; // racy, a report figure only
                }
            }
            deferredThreadStats.threadExecTime[threadNum] += jobEnd - jobStart;
            {
                // for the benchmark's log (VitaBench.cpp): jobs per thread,
                // and every job over 3 ms with its list and thread
                const uint64 jobUs = jobEnd - jobStart;
                vita_benchJobCount[threadNum]++;
                vita_benchJobUs[threadNum] += jobUs;
                if (jobUs >= 3000) {
                    const int slot = (vita_benchSlowJobNext.Increment() - 1) & (VITA_BENCH_SLOW_JOBS - 1);
                    vitaBenchSlowJob_t& s = vita_benchSlowJobs[slot];
                    s.name = GetJobName(jobList[state.nextJobIndex].function);
                    s.list = GetJobListName(GetId());
                    s.us = (int)jobUs;
                    s.thread = (int)threadNum;
                    s.endUs = jobEnd;
                }
            }

#ifndef _DEBUG
            if (jobs_longJobMicroSec.GetInteger() > 0) {
                if (jobEnd - jobStart > jobs_longJobMicroSec.GetInteger()
                    && GetId() != JOBLIST_UTILITY) {
                    longJobTime = (jobEnd - jobStart) * (1.0f / 1000.0f);
                    longJobFunc = jobList[state.nextJobIndex].function;
                    longJobData = jobList[state.nextJobIndex].data;
                    const char* jobName = GetJobName(jobList[state.nextJobIndex].function);
                    const char* jobListName = GetJobListName(GetId());
#if defined(__vita__)
                    // dozens a second in mars_city2, each a log write to the
                    // card (and a float format on a job thread): counted and
                    // summed up in the perf line instead, the first 8 printed
                    extern int vita_longJobs, vita_longJobsPrinted;
                    extern uint64 vita_longJobWorstUs;
                    extern const char* vita_longJobWorstName;
                    vita_longJobs++;
                    if (jobEnd - jobStart > vita_longJobWorstUs) {
                        vita_longJobWorstUs = jobEnd - jobStart;
                        vita_longJobWorstName = jobName;
                    }
                    if (vita_longJobsPrinted < 8) {
                        vita_longJobsPrinted++;
                        idLib::Printf("%d microseconds for a single '%s' job from job list %s on thread %d\n", (int)(jobEnd - jobStart), jobName, jobListName, threadNum);
                    }
#else
                    idLib::Printf("%1.1f milliseconds for a single '%s' job from job list %s on thread %d\n", longJobTime, jobName, jobListName, threadNum);
#endif
                }
            }
#endif
        }

        result |= RUN_PROGRESS;

        // decrease the job count for the current signal
        if (signalJobCount[state.signalIndex].Decrement() == 0) {
            // if this was the very last job of the job list
            if (state.signalIndex == signalJobCount.Num() - 1) {
                deferredThreadStats.endTime = Sys_Microseconds();
                return (result | RUN_DONE);
            }
        }

    } while (!singleJob);

    return result;
}

/*
========================
idParallelJobList_Threads::RunJobs
========================
*/
int idParallelJobList_Threads::RunJobs(unsigned int threadNum, threadJobListState_t& state, bool singleJob)
{
    uint64 start = Sys_Microseconds();

    numThreadsExecuting.Increment();

    int result = RunJobsInternal(threadNum, state, singleJob);

    numThreadsExecuting.Decrement();

    deferredThreadStats.threadTotalTime[threadNum] += Sys_Microseconds() - start;

    return result;
}

/*
========================
idParallelJobList_Threads::WaitForOtherJobList
========================
*/
bool idParallelJobList_Threads::WaitForOtherJobList()
{
    if (waitForGuard != NULL) {
        if (waitForGuard->GetValue() > 0) {
            return true;
        }
    }
    return false;
}

/*
================================================================================================

idParallelJobList

================================================================================================
*/

/*
========================
idParallelJobList::idParallelJobList
========================
*/
idParallelJobList::idParallelJobList(jobListId_t id, jobListPriority_t priority, unsigned int maxJobs, unsigned int maxSyncs, const idColor* color)
{
    assert(priority > JOBLIST_PRIORITY_NONE);
    this->jobListThreads = new (TAG_JOBLIST) idParallelJobList_Threads(id, priority, maxJobs, maxSyncs);
    this->color = color;
}

/*
========================
idParallelJobList::~idParallelJobList
========================
*/
idParallelJobList::~idParallelJobList()
{
    delete jobListThreads;
}

/*
========================
idParallelJobList::AddJob
========================
*/
void idParallelJobList::AddJob(jobRun_t function, void* data)
{
    assert(IsRegisteredJob(function));
    jobListThreads->AddJob(function, data);
}

/*
========================
idParallelJobList::AddJobSPURS
========================
*/
CellSpursJob128* idParallelJobList::AddJobSPURS()
{
    return NULL;
}

/*
========================
idParallelJobList::InsertSyncPoint
========================
*/
void idParallelJobList::InsertSyncPoint(jobSyncType_t syncType)
{
    jobListThreads->InsertSyncPoint(syncType);
}

/*
========================
idParallelJobList::Wait
========================
*/
void idParallelJobList::Wait()
{
    if (jobListThreads != NULL) {
        jobListThreads->Wait();
    }
}

/*
========================
idParallelJobList::TryWait
========================
*/
bool idParallelJobList::TryWait()
{
    bool done = true;
    if (jobListThreads != NULL) {
        done &= jobListThreads->TryWait();
    }
    return done;
}

/*
========================
idParallelJobList::Submit
========================
*/
void idParallelJobList::Submit(idParallelJobList* waitForJobList, int parallelism)
{
    assert(waitForJobList != this);
    jobListThreads->Submit((waitForJobList != NULL) ? waitForJobList->jobListThreads : NULL, parallelism);
}

/*
========================
idParallelJobList::IsSubmitted
========================
*/
bool idParallelJobList::IsSubmitted() const
{
    return jobListThreads->IsSubmitted();
}

/*
========================
idParallelJobList::GetNumExecutedJobs
========================
*/
unsigned int idParallelJobList::GetNumExecutedJobs() const
{
    return jobListThreads->GetNumExecutedJobs();
}

/*
========================
idParallelJobList::GetNumSyncs
========================
*/
unsigned int idParallelJobList::GetNumSyncs() const
{
    return jobListThreads->GetNumSyncs();
}

/*
========================
idParallelJobList::GetSubmitTimeMicroSec
========================
*/
uint64 idParallelJobList::GetSubmitTimeMicroSec() const
{
    return jobListThreads->GetSubmitTimeMicroSec();
}

/*
========================
idParallelJobList::GetStartTimeMicroSec
========================
*/
uint64 idParallelJobList::GetStartTimeMicroSec() const
{
    return jobListThreads->GetStartTimeMicroSec();
}

/*
========================
idParallelJobList::GetFinishTimeMicroSec
========================
*/
uint64 idParallelJobList::GetFinishTimeMicroSec() const
{
    return jobListThreads->GetFinishTimeMicroSec();
}

/*
========================
idParallelJobList::GetWaitTimeMicroSec
========================
*/
uint64 idParallelJobList::GetWaitTimeMicroSec() const
{
    return jobListThreads->GetWaitTimeMicroSec();
}

/*
========================
idParallelJobList::GetTotalProcessingTimeMicroSec
========================
*/
uint64 idParallelJobList::GetTotalProcessingTimeMicroSec() const
{
    return jobListThreads->GetTotalProcessingTimeMicroSec();
}

/*
========================
idParallelJobList::GetTotalWastedTimeMicroSec
========================
*/
uint64 idParallelJobList::GetTotalWastedTimeMicroSec() const
{
    return jobListThreads->GetTotalWastedTimeMicroSec();
}

/*
========================
idParallelJobList::GetUnitProcessingTimeMicroSec
========================
*/
uint64 idParallelJobList::GetUnitProcessingTimeMicroSec(int unit) const
{
    return jobListThreads->GetUnitProcessingTimeMicroSec(unit);
}

/*
========================
idParallelJobList::GetUnitWastedTimeMicroSec
========================
*/
uint64 idParallelJobList::GetUnitWastedTimeMicroSec(int unit) const
{
    return jobListThreads->GetUnitWastedTimeMicroSec(unit);
}

/*
========================
idParallelJobList::GetId
========================
*/
jobListId_t idParallelJobList::GetId() const
{
    return jobListThreads->GetId();
}

/*
================================================================================================

idJobThread

================================================================================================
*/

const int JOB_THREAD_STACK_SIZE = 256 * 1024; // same size as the SPU local store

struct threadJobList_t {
    idParallelJobList_Threads* jobList;
    int version;
};

static idCVar jobs_prioritize("jobs_prioritize", "1", CVAR_BOOL | CVAR_NOCHEAT, "prioritize job lists");

class idJobThread : public idSysThread {
public:
    idJobThread();
    ~idJobThread();

    void Start(core_t core, unsigned int threadNum);

    void AddJobList(idParallelJobList_Threads* jobList);

private:
    threadJobList_t jobLists[MAX_JOBLISTS]; // cyclic buffer with job lists
    unsigned int firstJobList;              // index of the last job list the thread grabbed
    unsigned int lastJobList;               // index where the next job list to work on will be added
    idSysMutex addJobMutex;

    unsigned int threadNum;

    virtual int Run();
};

/*
========================
idJobThread::idJobThread
========================
*/
idJobThread::idJobThread()
    : firstJobList(0)
    , lastJobList(0)
    , threadNum(0)
{
}

/*
========================
idJobThread::~idJobThread
========================
*/
idJobThread::~idJobThread()
{
}

/*
========================
idJobThread::Start
========================
*/
void idJobThread::Start(core_t core, unsigned int threadNum)
{
    this->threadNum = threadNum;
    // DG: change threadname from "JobListProcessor_%d" to "JLProc_%d", because Linux
    // has a 15 (+ \0) char limit for threadnames.
    // furthermore: va is not thread safe, use snPrintf instead
    char name[16];
    idStr::snPrintf(name, 16, "JLProc_%d", threadNum);
    StartWorkerThread(name, core, THREAD_NORMAL, JOB_THREAD_STACK_SIZE);
    // DG end
}

/*
========================
idJobThread::AddJobList
========================
*/
void idJobThread::AddJobList(idParallelJobList_Threads* jobList)
{
    // must lock because multiple threads may try to add new job lists at the same time
    addJobMutex.Lock();
    // wait until there is space available because in rare cases multiple versions of the same job lists may still be queued
    while (lastJobList - firstJobList >= MAX_JOBLISTS) {
        Sys_Yield();
    }
    assert(lastJobList - firstJobList < MAX_JOBLISTS);
    jobLists[lastJobList & (MAX_JOBLISTS - 1)].jobList = jobList;
    jobLists[lastJobList & (MAX_JOBLISTS - 1)].version = jobList->GetVersion();
    lastJobList++;
    addJobMutex.Unlock();
}

/*
========================
idJobThread::Run
========================
*/
#if defined(__vita__)
// the core-0 worker's gate (see VITA_JOB_THREAD_CORES): open while the main
// thread is blocked waiting for Game/Draw
#define VITA_CORE0_WORKER 3

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
// Where the three core 1-2 workers run (vita_jobWorkerCores): Game/Draw is on
// core 1 at a lower priority than the workers, so a worker landing on core 1
// preempts Game/Draw's own serial front-end work - the frame's critical path -
// for as long as its job lasts. 1 keeps them on core 2: core 1 is Game/Draw's
// alone (it still takes jobs while it waits for a list), core 0's gated helper
// as before. Applied by each worker to itself between job lists, so the
// benchmark can switch it mid-run and compare (VitaBench script: set).
idCVar vita_jobWorkerCores("vita_jobWorkerCores", "0", CVAR_INTEGER, "the core 1-2 job workers: 0 on cores 1 and 2 (shared with Game/Draw on core 1), 1 on core 2 only (core 1 left to Game/Draw)", 0, 1);
static volatile int vitaJobWorkerCoresWanted = 0;
static int vitaJobWorkerCoresApplied[32]; // (MAX_JOB_THREADS is defined below)
#endif
static volatile int vitaMainWaiting = 0;
static idSysSignal vitaMainWaitingSignal;
void Vita_JobsMainWaiting(bool waiting)
{
    vitaMainWaiting = waiting ? 1 : 0;
    if (waiting) {
        vitaMainWaitingSignal.Raise();
    }
}
#endif

int idJobThread::Run()
{
    threadJobListState_t threadJobListState[MAX_JOBLISTS];
    int numJobLists = 0;
    int lastStalledJobList = -1;
#if defined(__vita__)
    if (threadNum != VITA_CORE0_WORKER && threadNum < 32 && vitaJobWorkerCoresApplied[threadNum] != vitaJobWorkerCoresWanted + 1) {
        const int wanted = vitaJobWorkerCoresWanted;
        const int mask = (wanted == 1) ? SCE_KERNEL_CPU_MASK_USER_2 : (SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2);
        const int r = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
        vitaJobWorkerCoresApplied[threadNum] = wanted + 1;
        idLib::Printf("vita: job worker %d now on %s (vita_jobWorkerCores %d)%s\n", threadNum, wanted == 1 ? "core 2 only" : "cores 1-2", wanted,
            r < 0 ? va(" - REFUSED 0x%08X", (unsigned int)r) : "");
    }
#endif

    while (!IsTerminating()) {

        // fetch any new job lists and add them to the local list
        if (numJobLists < MAX_JOBLISTS && firstJobList < lastJobList) {
            threadJobListState[numJobLists].jobList = jobLists[firstJobList & (MAX_JOBLISTS - 1)].jobList;
            threadJobListState[numJobLists].version = jobLists[firstJobList & (MAX_JOBLISTS - 1)].version;
            threadJobListState[numJobLists].signalIndex = 0;
            threadJobListState[numJobLists].lastJobIndex = 0;
            threadJobListState[numJobLists].nextJobIndex = -1;
            numJobLists++;
            firstJobList++;
        }
        if (numJobLists == 0) {
            break;
        }
#if defined(__vita__)
        if (threadNum == VITA_CORE0_WORKER && !vitaMainWaiting) {
            // The main thread may wake and need core 0 at any moment: take
            // no job until it is waiting for Game/Draw. But keep the queue
            // moving - lists are queued to this thread whatever it does, and
            // a full queue makes Submit (AddJobList) spin: a map load, with
            // the main thread never waiting for Game/Draw, froze that way.
            // Lists already done (their version moved on, the test RunJobs
            // makes) are dropped; the rest are finished by the others.
            for (int i = 0; i < numJobLists;) {
                if (threadJobListState[i].version != threadJobListState[i].jobList->GetVersion()) {
                    threadJobListState[i] = threadJobListState[--numJobLists];
                } else {
                    i++;
                }
            }
            lastStalledJobList = -1;
            while (numJobLists < MAX_JOBLISTS && firstJobList < lastJobList) {
                threadJobListState[numJobLists].jobList = jobLists[firstJobList & (MAX_JOBLISTS - 1)].jobList;
                threadJobListState[numJobLists].version = jobLists[firstJobList & (MAX_JOBLISTS - 1)].version;
                threadJobListState[numJobLists].signalIndex = 0;
                threadJobListState[numJobLists].lastJobIndex = 0;
                threadJobListState[numJobLists].nextJobIndex = -1;
                numJobLists++;
                firstJobList++;
            }
            if (numJobLists == 0) {
                break; // nothing pending: sleep until the next list is queued
            }
            vitaMainWaitingSignal.Wait(1);
            continue;
        }
#endif

        int currentJobList = 0;
        jobListPriority_t priority = JOBLIST_PRIORITY_NONE;
        if (lastStalledJobList < 0) {
            // find the job list with the highest priority
            for (int i = 0; i < numJobLists; i++) {
                if (threadJobListState[i].jobList->GetPriority() > priority && !threadJobListState[i].jobList->WaitForOtherJobList()) {
                    priority = threadJobListState[i].jobList->GetPriority();
                    currentJobList = i;
                }
            }
        } else {
            // try to hide the stall with a job from a list that has equal or higher priority
            currentJobList = lastStalledJobList;
            priority = threadJobListState[lastStalledJobList].jobList->GetPriority();
            for (int i = 0; i < numJobLists; i++) {
                if (i != lastStalledJobList && threadJobListState[i].jobList->GetPriority() >= priority && !threadJobListState[i].jobList->WaitForOtherJobList()) {
                    priority = threadJobListState[i].jobList->GetPriority();
                    currentJobList = i;
                }
            }
        }

        // if the priority is high then try to run through the whole list to reduce the overhead
        // otherwise run a single job and re-evaluate priorities for the next job
        bool singleJob = (priority == JOBLIST_PRIORITY_HIGH) ? false : jobs_prioritize.GetBool();
#if defined(__vita__)
        if (threadNum == VITA_CORE0_WORKER) {
            singleJob = true; // back to the gate between jobs
        }
#endif

        // try running one or more jobs from the current job list
        int result = threadJobListState[currentJobList].jobList->RunJobs(threadNum, threadJobListState[currentJobList], singleJob);

        if ((result & idParallelJobList_Threads::RUN_DONE) != 0) {
            // done with this job list so remove it from the local list
            for (int i = currentJobList; i < numJobLists - 1; i++) {
                threadJobListState[i] = threadJobListState[i + 1];
            }
            numJobLists--;
            lastStalledJobList = -1;
        } else if ((result & idParallelJobList_Threads::RUN_STALLED) != 0) {
            // yield when stalled on the same job list again without making any progress
            if (currentJobList == lastStalledJobList) {
                if ((result & idParallelJobList_Threads::RUN_PROGRESS) == 0) {
                    Sys_Yield();
                }
            }
            lastStalledJobList = currentJobList;
        } else {
            lastStalledJobList = -1;
        }
    }
    return 0;
}

/*
================================================================================================

idParallelJobManagerLocal

================================================================================================
*/

extern void Sys_CPUCount(int& logicalNum, int& coreNum, int& packageNum);

// WINDOWS LOGICAL PROCESSOR LIMITS:
//
// http://download.microsoft.com/download/5/7/7/577a5684-8a83-43ae-9272-ff260a9c20e2/Hyper-thread_Windows.doc
//
//											Physical	Logical (Cores + HT)
// Windows XP Home Edition					1			2
// Windows XP Professional					2			4
// Windows Server 2003, Standard Edition	4			8
// Windows Server 2003, Enterprise Edition	8			16
// Windows Server 2003, Datacenter Edition	32			32
//
// Windows Vista							?			?
//
// Windows 7 Starter						1			32/64
// Windows 7 Home Basic						1			32/64
// Windows 7 Professional					2			32/64
//
//
// Hyperthreading is not dead yet.  Intel's Core i7 Processor is quad-core with HT for 8 logicals.

// DOOM3: We don't have that many jobs, so just set this fairly low so we don't spin up a ton of idle threads
#if defined(__vita__)
// Init() starts MAX_JOB_THREADS threads unconditionally and then only ever uses
// jobs_numThreads (default 2) of them. On a desktop the 30 idle threads cost
// almost nothing because their stacks are never faulted in; on the Vita every
// stack is committed up front - 32 * JOB_THREAD_STACK_SIZE is 8 MB - and each
// one is a real kernel thread on a console with three usable cores. This also
// clamps the jobs_numThreads cvar, which is declared with MAX_JOB_THREADS as
// its upper bound, so it can never select a thread that was not started.
#define MAX_JOB_THREADS 4
#else
#define MAX_JOB_THREADS 32
#endif
#if defined(__vita__)
#define NUM_JOB_THREADS "4"
// Every job list here is waited on within the frame (the front end's model,
// light and shadow jobs, classic DOOM's columns), so a job held by a thread
// the OS stops is a frame hitch. Core 3 is the OS's when it wants it (even
// with CapUnlocker), so no job worker goes there - core 3 only gets work
// nobody waits for (the log writer, the watchdog, savegame compression; see
// vita_main.cpp and File_SaveGame.cpp). The workers run on cores 1 and 2
// (Thread.cpp: core_t 7), never on core 0: there the render back end and
// OpenAL's mixer run at priority 0x40/0x41, above the workers (0x9F), and
// stopped whatever job a worker held - the front end then waited for that
// one job (the benchmark measured 3-7 ms "single jobs", 6% of the shadow
// volume jobs, for shapes the PC builds in 10 us). Core 2 has nothing above
// them now (the mixer moved to core 0, vita_main.cpp), and on core 1 they
// share with Game/Draw at the same priority - which, waiting on a list,
// takes jobs too.
//
// The fourth worker is on core 0 - but only takes jobs while the main thread
// is blocked waiting for Game/Draw (Vita_JobsMainWaiting below). The front
// end's jobs ran on cores 1 and 2 only, Game/Draw sharing core 1 with a
// worker, so in the benchmark's fights Game/Draw ran half of them itself
// (27 of 54 ms of job time a frame) while the main thread sat in "wait for
// game/draw" on core 0 for 70 ms. A job this worker takes then cannot be
// stopped by the back end: the main thread only wakes from that wait when
// Game/Draw is done, and Game/Draw is only done once every front end job
// is. When the main thread is busy the worker does not fetch jobs at all.
#define VITA_JOB_THREAD_CORES { (core_t)7, (core_t)7, (core_t)7, CORE_0A } // VITA_CORE0_WORKER (3) last
bool Vita_Core3Available(); // vita_main.cpp
#else
#define NUM_JOB_THREADS "2"
#endif
#define JOB_THREAD_CORES { CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY, \
    CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY,                        \
    CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY,                        \
    CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY,                        \
    CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY,                        \
    CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY,                        \
    CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY,                        \
    CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY }

idCVar jobs_numThreads("jobs_numThreads", NUM_JOB_THREADS, CVAR_INTEGER | CVAR_NOCHEAT, "number of threads used to crunch through jobs", 0, MAX_JOB_THREADS);

class idParallelJobManagerLocal : public idParallelJobManager {
public:
    virtual ~idParallelJobManagerLocal() { }

    virtual void Init();
    virtual void Shutdown();

    virtual idParallelJobList* AllocJobList(jobListId_t id, jobListPriority_t priority, unsigned int maxJobs, unsigned int maxSyncs, const idColor* color);
    virtual void FreeJobList(idParallelJobList* jobList);

    virtual int GetNumJobLists() const;
    virtual int GetNumFreeJobLists() const;
    virtual idParallelJobList* GetJobList(int index);

    virtual int GetNumProcessingUnits();

    virtual void WaitForAllJobLists();

    void Submit(idParallelJobList_Threads* jobList, int parallelism);

private:
    idJobThread threads[MAX_JOB_THREADS];
    unsigned int maxThreads;
    int numPhysicalCpuCores;
    int numLogicalCpuCores;
    int numCpuPackages;
    idStaticList<idParallelJobList*, MAX_JOBLISTS> jobLists;
};

idParallelJobManagerLocal parallelJobManagerLocal;
idParallelJobManager* parallelJobManager = &parallelJobManagerLocal;

/*
========================
SubmitJobList
========================
*/
void SubmitJobList(idParallelJobList_Threads* jobList, int parallelism)
{
    parallelJobManagerLocal.Submit(jobList, parallelism);
}

/*
========================
idParallelJobManagerLocal::Init
========================
*/
void idParallelJobManagerLocal::Init()
{
    // on consoles this will have specific cores for the threads, but on PC they will all be CORE_ANY
#if defined(__vita__)
    core_t cores[] = VITA_JOB_THREAD_CORES;
    idLib::Printf("vita: %d job threads - three on cores 1-2, the fourth on core 0 taking jobs only while the main thread waits for game/draw; none on core 3: %s - it only runs work nothing waits for\n", jobs_numThreads.GetInteger(),
        Vita_Core3Available() ? "available through CapUnlocker or similar" : "kept by the system");
#else
    core_t cores[] = JOB_THREAD_CORES;
#endif
    assert(sizeof(cores) / sizeof(cores[0]) >= MAX_JOB_THREADS);

    for (int i = 0; i < MAX_JOB_THREADS; i++) {
        threads[i].Start(cores[i], i);
    }
    maxThreads = jobs_numThreads.GetInteger();

    Sys_CPUCount(numLogicalCpuCores, numPhysicalCpuCores, numCpuPackages);
}

/*
========================
idParallelJobManagerLocal::Shutdown
========================
*/
void idParallelJobManagerLocal::Shutdown()
{
    for (int i = 0; i < MAX_JOB_THREADS; i++) {
        threads[i].StopThread();
    }
}

/*
========================
idParallelJobManagerLocal::AllocJobList
========================
*/
idParallelJobList* idParallelJobManagerLocal::AllocJobList(jobListId_t id, jobListPriority_t priority, unsigned int maxJobs, unsigned int maxSyncs, const idColor* color)
{
    for (int i = 0; i < jobLists.Num(); i++) {
        if (jobLists[i]->GetId() == id) {
            // idStudio may cause job lists to be allocated multiple times
        }
    }
    idParallelJobList* jobList = new (TAG_JOBLIST) idParallelJobList(id, priority, maxJobs, maxSyncs, color);
    jobLists.Append(jobList);
    return jobList;
}

/*
========================
idParallelJobManagerLocal::FreeJobList
========================
*/
void idParallelJobManagerLocal::FreeJobList(idParallelJobList* jobList)
{
    if (jobList == NULL) {
        return;
    }
    // wait for all job threads to finish because job list deletion is not thread safe
    for (unsigned int i = 0; i < maxThreads; i++) {
        threads[i].WaitForThread();
    }
    int index = jobLists.FindIndex(jobList);
    assert(index >= 0 && jobLists[index] == jobList);
    jobLists[index]->Wait();
    delete jobLists[index];
    jobLists.RemoveIndexFast(index);
}

/*
========================
idParallelJobManagerLocal::GetNumJobLists
========================
*/
int idParallelJobManagerLocal::GetNumJobLists() const
{
    return jobLists.Num();
}

/*
========================
idParallelJobManagerLocal::GetNumFreeJobLists
========================
*/
int idParallelJobManagerLocal::GetNumFreeJobLists() const
{
    return MAX_JOBLISTS - jobLists.Num();
}

/*
========================
idParallelJobManagerLocal::GetJobList
========================
*/
idParallelJobList* idParallelJobManagerLocal::GetJobList(int index)
{
    return jobLists[index];
}

/*
========================
idParallelJobManagerLocal::GetNumProcessingUnits
========================
*/
int idParallelJobManagerLocal::GetNumProcessingUnits()
{
    return maxThreads;
}

/*
========================
idParallelJobManagerLocal::WaitForAllJobLists
========================
*/
void idParallelJobManagerLocal::WaitForAllJobLists()
{
    // wait for all job lists to complete
    for (int i = 0; i < jobLists.Num(); i++) {
        jobLists[i]->Wait();
    }
}

/*
========================
idParallelJobManagerLocal::Submit
========================
*/
void idParallelJobManagerLocal::Submit(idParallelJobList_Threads* jobList, int parallelism)
{
    if (jobs_numThreads.IsModified()) {
        maxThreads = idMath::ClampInt(0, MAX_JOB_THREADS, jobs_numThreads.GetInteger());
        jobs_numThreads.ClearModified();
    }
#if defined(__vita__)
    vitaJobWorkerCoresWanted = vita_jobWorkerCores.GetInteger(); // (the workers apply it, Run)
#endif

    // determine the number of threads to use
    int numThreads = maxThreads;
    if (parallelism == JOBLIST_PARALLELISM_DEFAULT) {
        numThreads = maxThreads;
    } else if (parallelism == JOBLIST_PARALLELISM_MAX_CORES) {
        numThreads = numLogicalCpuCores;
    } else if (parallelism == JOBLIST_PARALLELISM_MAX_THREADS) {
        numThreads = MAX_JOB_THREADS;
    } else if (parallelism > MAX_JOB_THREADS) {
        numThreads = MAX_JOB_THREADS;
    } else {
        numThreads = parallelism;
    }

    if (numThreads <= 0) {
        threadJobListState_t state(jobList->GetVersion());
        jobList->RunJobs(0, state, false);
        return;
    }

    for (int i = 0; i < numThreads; i++) {
        threads[i].AddJobList(jobList);
        threads[i].SignalWork();
    }
}
