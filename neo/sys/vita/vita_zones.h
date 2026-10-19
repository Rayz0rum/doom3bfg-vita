/*
===========================================================================

PS Vita: named timing zones for the perf log.

    VITA_ZONE("player move");        // times the rest of the enclosing scope
    VITA_ZONE_COUNT("clip traces");  // only counts calls (for hot paths, where
                                     // reading the clock would cost more than
                                     // the work)
    VITA_ZONE_ADD("verts", n);       // counts n things, not calls
    VITA_ZONE_BEGIN(loop, "emitters"); // times a stretch inside a scope, up
    ...                                // to VITA_ZONE_END(loop) (no return
    VITA_ZONE_END(loop);               // in between)
    GD_SCOPE("game tics");           // a pass of the game/draw thread's tree
                                     // only (no zone)

Zones register themselves on first use; Vita_ZoneReport() (vita_main.cpp)
prints the costliest once per perf window and clears them. Times are
inclusive - a zone inside another is counted in both - and summed over every
thread that runs them (job workers included).

The game/draw thread's tree (VitaBench.cpp): every timed zone and GD_SCOPE
entered on the game/draw thread is also a node of a tree of passes, nested as
they ran, and each node collects its EXCLUSIVE time - what the thread spent
there and not in a pass inside it. So the tree adds up to the thread's whole
frame, and time nobody instrumented shows up as a parent's own time instead
of vanishing. The benchmark prints it per segment and per hitch. Zones run on
other threads do not touch it.

The PC test build (ID_VITA_PHYSCHECK) has them too: the benchmark prints the
zones of each segment (VitaBench.cpp), so a hot spot can be chased on the PC.

===========================================================================
*/
#ifndef __VITA_ZONES_H__
#define __VITA_ZONES_H__

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)

uint64 Sys_Microseconds(); // sys_public.h (some job files include less than that)

// A plain aggregate, constant-initialized: a zone is a function-local static,
// and one with a constructor would be built under a C++ initialization guard
// on first use. Zones run on job threads too, several at once, and the
// Vita's libstdc++ treated the program as single-threaded (it looks for
// pthread_cancel, which nothing linked): two threads entering a new zone
// together read as a recursive initialization and called std::terminate.
// So zones are registered on first use instead, lock-free (first one wins).
struct vitaZone_t {
    const char* name;
    volatile int us;
    volatile int calls;
    bool timed;
    volatile int registered;
    vitaZone_t* next;
};
void Vita_ZoneRegister(vitaZone_t* z); // vita_main.cpp (VitaBench.cpp on the PC)
static inline void Vita_ZoneUse(vitaZone_t& z)
{
    if (!z.registered) {
        Vita_ZoneRegister(&z);
    }
}

// the game/draw thread's tree of passes (VitaBench.cpp). Push returns the
// node to go back to, or -1 when not on the game/draw thread (Pop then does
// nothing); 'now' is a Sys_Microseconds() the caller already read.
int VitaGD_PushAt(const char* name, uint64 now);
void VitaGD_PopAt(int parent, uint64 now);
int VitaGD_Push(const char* name);
void VitaGD_Pop(int parent);

// Zone sums are atomic (job workers add to them at once) but need no
// ordering with anything else, so no barriers: __sync_fetch_and_add puts a
// full DMB on each side of the add, which on the Cortex-A9 is most of what a
// counted zone cost (thousands a frame in the model jobs).
#define VITA_ZONE_ATOMIC_ADD(var, n) __atomic_fetch_add(&(var), (n), __ATOMIC_RELAXED)

struct vitaZoneTimer_t {
    vitaZone_t& zone;
    uint64 start;
    int gd;
    vitaZoneTimer_t(vitaZone_t& z) : zone(z), start(Sys_Microseconds())
    {
        Vita_ZoneUse(z);
        gd = VitaGD_PushAt(z.name, start);
    }
    ~vitaZoneTimer_t()
    {
        const uint64 end = Sys_Microseconds();
        VITA_ZONE_ATOMIC_ADD(zone.us, (int)(end - start));
        VITA_ZONE_ATOMIC_ADD(zone.calls, 1);
        VitaGD_PopAt(gd, end);
    }
};

// times only the calls whose key picks them (one in 2^shift) and adds their
// time and count that many times over: for code too hot to clock every call
// (hundreds a frame of a few microseconds each). The key should change from
// frame to frame (an object's address mixed with the frame number), so every
// object is sampled in turn; no counter is shared between threads until a
// sampled call adds to the zone. No node in the game/draw thread's tree: the
// time stays in the parent pass's own there.
struct vitaZoneSampler_t {
    vitaZone_t& zone;
    uint64 start;
    int shift;
    vitaZoneSampler_t(vitaZone_t& z, unsigned int key, int shift_) : zone(z), start(0), shift(shift_)
    {
        if ((key & ((1u << shift_) - 1)) == 0) {
            Vita_ZoneUse(z);
            start = Sys_Microseconds();
        }
    }
    ~vitaZoneSampler_t()
    {
        if (start != 0) {
            VITA_ZONE_ATOMIC_ADD(zone.us, (int)((Sys_Microseconds() - start) << shift));
            VITA_ZONE_ATOMIC_ADD(zone.calls, 1 << shift);
        }
    }
};

struct vitaGDScope_t {
    int gd;
    vitaGDScope_t(const char* name) : gd(VitaGD_Push(name)) { }
    ~vitaGDScope_t()
    {
        VitaGD_Pop(gd);
    }
};

void Vita_ZoneReport(double frames);

#define VITA_ZONE_CAT2(a, b) a##b
#define VITA_ZONE_CAT(a, b) VITA_ZONE_CAT2(a, b)
#define VITA_ZONE(name) \
    static vitaZone_t VITA_ZONE_CAT(vitaZone_, __LINE__) = { name, 0, 0, true, 0, 0 }; \
    vitaZoneTimer_t VITA_ZONE_CAT(vitaZoneTimer_, __LINE__)(VITA_ZONE_CAT(vitaZone_, __LINE__))
#define VITA_ZONE_COUNT(name) \
    static vitaZone_t VITA_ZONE_CAT(vitaZone_, __LINE__) = { name, 0, 0, false, 0, 0 }; \
    Vita_ZoneUse(VITA_ZONE_CAT(vitaZone_, __LINE__)); \
    VITA_ZONE_ATOMIC_ADD(VITA_ZONE_CAT(vitaZone_, __LINE__).calls, 1)
// counts things (vertices, triangles...) instead of calls: adds n
#define VITA_ZONE_ADD(name, n) \
    static vitaZone_t VITA_ZONE_CAT(vitaZone_, __LINE__) = { name, 0, 0, false, 0, 0 }; \
    Vita_ZoneUse(VITA_ZONE_CAT(vitaZone_, __LINE__)); \
    VITA_ZONE_ATOMIC_ADD(VITA_ZONE_CAT(vitaZone_, __LINE__).calls, (int)(n))
#define VITA_ZONE_BEGIN(var, name) \
    static vitaZone_t var##_vitaZone = { name, 0, 0, true, 0, 0 }; \
    Vita_ZoneUse(var##_vitaZone); \
    const uint64 var##_vitaStart = Sys_Microseconds(); \
    const int var##_vitaGD = VitaGD_PushAt(name, var##_vitaStart)
#define VITA_ZONE_END(var) \
    do { \
        const uint64 var##_vitaEnd = Sys_Microseconds(); \
        VITA_ZONE_ATOMIC_ADD(var##_vitaZone.us, (int)(var##_vitaEnd - var##_vitaStart)); \
        VITA_ZONE_ATOMIC_ADD(var##_vitaZone.calls, 1); \
        VitaGD_PopAt(var##_vitaGD, var##_vitaEnd); \
    } while (0)
// one call in 2^shift timed, picked by key (see vitaZoneSampler_t); the
// report shows the scaled-up estimate - put "(sampled)" in the name
#define VITA_ZONE_SAMPLED(name, key, shift) \
    static vitaZone_t VITA_ZONE_CAT(vitaZone_, __LINE__) = { name, 0, 0, true, 0, 0 }; \
    vitaZoneSampler_t VITA_ZONE_CAT(vitaZoneSampler_, __LINE__)(VITA_ZONE_CAT(vitaZone_, __LINE__), (unsigned int)(key), shift)
#define GD_SCOPE(name) vitaGDScope_t VITA_ZONE_CAT(vitaGDScope_, __LINE__)(name)
// consecutive passes of one function: each ends where the next one starts
// (GD_PASS_END before any return in between)
#define GD_PASS_BEGIN(name) int gdPassNode = VitaGD_Push(name)
#define GD_PASS(name) do { VitaGD_Pop(gdPassNode); gdPassNode = VitaGD_Push(name); } while (0)
#define GD_PASS_END() do { VitaGD_Pop(gdPassNode); gdPassNode = -1; } while (0)

#else

#define VITA_ZONE(name)
#define VITA_ZONE_COUNT(name)
#define VITA_ZONE_ADD(name, n)
#define VITA_ZONE_BEGIN(var, name)
#define VITA_ZONE_END(var)
#define VITA_ZONE_SAMPLED(name, key, shift)
#define GD_SCOPE(name)
#define GD_PASS_BEGIN(name)
#define GD_PASS(name)
#define GD_PASS_END()

#endif

#endif /* !__VITA_ZONES_H__ */
