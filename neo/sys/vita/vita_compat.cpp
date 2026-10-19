/*
===========================================================================

Doom 3 BFG Edition GPL Source Code — PS Vita port compatibility shims.

The VitaSDK newlib provides POSIX headers for a number of interfaces it has no
implementation for. Rather than #ifdef out every call site in the shared POSIX
backend, this file supplies the handful of functions that are actually reached.

===========================================================================
*/

#include <string.h>
#include <errno.h>

extern "C" {

/*
================================================================================
Dynamic loading.

DOOM 3 BFG is a single monolithic executable — the game/d3xp DLL split of the
original DOOM 3 is gone. Sys_DLL_Load() is only reached by dead paths, so these
report a clean failure rather than pretending to succeed.
================================================================================
*/

static const char* dl_lastError = NULL;

void* dlopen(const char* file, int mode)
{
    (void)file;
    (void)mode;
    dl_lastError = "dynamic loading is not supported on the PS Vita";
    return NULL;
}

void* dlsym(void* handle, const char* name)
{
    (void)handle;
    (void)name;
    dl_lastError = "dynamic loading is not supported on the PS Vita";
    return NULL;
}

int dlclose(void* handle)
{
    (void)handle;
    dl_lastError = NULL;
    return 0;
}

char* dlerror(void)
{
    const char* e = dl_lastError;
    dl_lastError = NULL;
    return (char*)e;
}

/*
================================================================================
sigaction

Signals are never delivered to Vita homebrew, and Posix_InitSigs() is already
compiled out. Posix_ClearSigs() still calls this on shutdown, so accept and
ignore. Reporting success keeps it from spamming the console on every exit.
================================================================================
*/

struct sigaction;

int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact)
{
    (void)signum;
    (void)act;
    (void)oldact;
    return 0;
}

/*
================================================================================
fnmatch

Used by Sys_ListFiles() to filter directory entries against shell patterns such
as "*.pk4". Only the subset POSIX defines without FNM_PATHNAME / FNM_PERIOD is
needed, since the game never passes flags. Backslash escaping is honoured.
================================================================================
*/

#define FNM_NOMATCH 1

// Returns 1 if 'c' is matched by the bracket expression starting at *pp
// (which points just past the '['). Advances *pp past the closing ']'.
static int MatchBracket(const char** pp, char c)
{
    const char* p = *pp;
    int negate = 0;
    int matched = 0;

    if (*p == '!' || *p == '^') {
        negate = 1;
        p++;
    }

    // A ']' in the first position is a literal, not the terminator.
    int first = 1;
    while (*p != '\0' && (*p != ']' || first)) {
        first = 0;
        char lo = *p++;
        if (lo == '\\' && *p != '\0') {
            lo = *p++;
        }
        if (*p == '-' && p[1] != ']' && p[1] != '\0') {
            p++;
            char hi = *p++;
            if (hi == '\\' && *p != '\0') {
                hi = *p++;
            }
            if (c >= lo && c <= hi) {
                matched = 1;
            }
        } else if (c == lo) {
            matched = 1;
        }
    }

    if (*p == ']') {
        p++;
    }
    // else: unterminated '[' — treat what we consumed as the whole class.

    *pp = p;
    return negate ? !matched : matched;
}

// Recursive-on-'*' matcher. Patterns here are short filename globs, so the
// worst case is bounded and this stays far cheaper than a full NFA.
static int MatchHere(const char* pat, const char* str)
{
    while (*pat != '\0') {
        if (*pat == '*') {
            // Collapse runs of '*'.
            while (*pat == '*') {
                pat++;
            }
            if (*pat == '\0') {
                return 0; // trailing '*' matches the rest
            }
            for (; *str != '\0'; str++) {
                if (MatchHere(pat, str) == 0) {
                    return 0;
                }
            }
            return MatchHere(pat, str); // allow '*' to match empty at the end
        }

        if (*str == '\0') {
            return FNM_NOMATCH;
        }

        if (*pat == '?') {
            pat++;
            str++;
            continue;
        }

        if (*pat == '[') {
            pat++;
            if (!MatchBracket(&pat, *str)) {
                return FNM_NOMATCH;
            }
            str++;
            continue;
        }

        if (*pat == '\\' && pat[1] != '\0') {
            pat++;
        }

        if (*pat != *str) {
            return FNM_NOMATCH;
        }
        pat++;
        str++;
    }

    return (*str == '\0') ? 0 : FNM_NOMATCH;
}

int fnmatch(const char* pattern, const char* string, int flags)
{
    (void)flags;
    if (pattern == NULL || string == NULL) {
        return FNM_NOMATCH;
    }
    return MatchHere(pattern, string);
}

} // extern "C"

/*
================================================================================
GL entry point loading.

sdl_qgl.cpp is excluded from the Vita build: it resolves several hundred GL 1.x
function pointers at runtime, most of which do not exist in GLES2. The Vita links
vitaGL statically and reaches it through qgl_linked.h, so there is nothing to
load and nothing to unload. GL call logging goes with it.

These keep the same signatures as the desktop originals so the shared callers in
sdl_glimp.cpp and RenderSystem.cpp need no #ifdefs.
================================================================================
*/

bool QGL_Init(const char* dllname)
{
    (void)dllname;
    return true;
}

void QGL_Shutdown()
{
}

void GLimp_EnableLogging(bool enable)
{
    (void)enable;
}
