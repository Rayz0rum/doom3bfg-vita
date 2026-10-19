/*
 * path_oc - kernel plugin: lets a game run the CPU at 500 MHz when it asks
 * for it. Written for the DOOM 3 BFG port, meant for later ports too: a port
 * just calls scePowerSetArmClockFrequency(500) and checks
 * scePowerGetArmClockFrequency() (444 without the plugin).
 *
 * The stock scePowerSetArmClockFrequency() refuses anything above 444 MHz.
 * This hooks that user call (its kernel side, in ScePower) and answers a
 * request between 445 and 500 MHz the way PSVshell does: the stock kernel path
 * sets 444 MHz, then the CPU PLL multiplier is raised to 15 (500 MHz) and
 * ScePower's record of the clock is updated so scePowerGetArmClockFrequency()
 * reports it. Every other request goes through untouched - and since the stock
 * system refuses those values, only software written for an overclock plugin
 * (a port asking for 500 at startup) ever makes one. No title check:
 * it would need a kernel import whose NID changes between firmwares.
 *
 * The 500 MHz method (ScePervasiveForDriver_0xE9D95643 with the 2-byte patch
 * that lifts its limit, and the ScePower data at 0x41C8 / 0x41CC) is taken
 * from PSVshell by Electry - https://github.com/Electry/PSVshell (src/main.c,
 * src/oc.c). Works alongside PSVshell.
 *
 * Install: copy path_oc.skprx to ur0:tai/, add the line
 *     ur0:tai/path_oc.skprx
 * under *KERNEL in ur0:tai/config.txt, reboot.
 */
#include <vitasdkkern.h>
#include <taihen.h>
#include <string.h>

int module_get_offset(SceUID pid, SceUID modid, int segidx, size_t offset, uintptr_t* addr);
int module_get_export_func(SceUID pid, const char* modname, uint32_t libnid, uint32_t funcnid, uintptr_t* func);

static tai_hook_ref_t g_set_arm_ref;
static SceUID g_set_arm_hook = -1;
static SceUID g_inject = -1;
static SceUID g_mutex = -1;

static uint32_t* ScePower_41C8 = NULL; // the ARM clock ScePower reports (MHz)
static uint32_t* ScePower_41CC = NULL; // the multiplier it recorded with it
static int (*_kscePowerSetArmClockFrequency)(int freq) = NULL;
static int (*ScePervasiveForDriver_0xE9D95643)(int mul, int ndiv) = NULL;

static int set_arm_500(void)
{
    // the stock path as far as it goes
    int ret = _kscePowerSetArmClockFrequency(444);
    if (ret < 0) {
        return ret;
    }
    // CPU PLL multiplier 15, divider 16 - 0: 500 MHz (PSVshell's psvs_oc_holy_shit)
    ScePervasiveForDriver_0xE9D95643(15, 16 - 0);
    *ScePower_41C8 = 500;
    *ScePower_41CC = 15;
    return 0;
}

// the kernel side of the user call scePowerSetArmClockFrequency()
static int scePowerSetArmClockFrequency_patched(int freq)
{
    if (freq > 444 && freq <= 500
        && ScePower_41C8 != NULL && ScePower_41CC != NULL
        && _kscePowerSetArmClockFrequency != NULL && ScePervasiveForDriver_0xE9D95643 != NULL) {
        int ret = ksceKernelLockMutex(g_mutex, 1, NULL);
        if (ret < 0) {
            return ret;
        }
        ret = set_arm_500();
        ksceKernelUnlockMutex(g_mutex, 1);
        return ret;
    }
    return TAI_CONTINUE(int, g_set_arm_ref, freq);
}

int module_start(SceSize argc, const void* args);
int _start(SceSize argc, const void* args) __attribute__((weak, alias("module_start")));
int module_start(SceSize argc, const void* args)
{
    (void)argc;
    (void)args;

    tai_module_info_t info;
    info.size = sizeof(info);
    if (taiGetModuleInfoForKernel(KERNEL_PID, "ScePower", &info) < 0) {
        return SCE_KERNEL_START_SUCCESS; // nothing to do; stay loaded and harmless
    }
    module_get_offset(KERNEL_PID, info.modid, 1, 0x41C8, (uintptr_t*)&ScePower_41C8);
    module_get_offset(KERNEL_PID, info.modid, 1, 0x41CC, (uintptr_t*)&ScePower_41CC);

    // ScePowerForDriver: kscePowerSetArmClockFrequency
    module_get_export_func(KERNEL_PID, "ScePower", 0x1590166F, 0x74DB5AE5, (uintptr_t*)&_kscePowerSetArmClockFrequency);
    // ScePervasiveForDriver (in SceLowio): set the ARM PLL multiplier / divider
    module_get_export_func(KERNEL_PID, "SceLowio", 0xE692C727, 0xE9D95643, (uintptr_t*)&ScePervasiveForDriver_0xE9D95643);

    if (ScePervasiveForDriver_0xE9D95643 != NULL) {
        // lift the function's own limit on the multiplier (as PSVshell does)
        static const uint8_t nop[] = { 0x00, 0xBF };
        g_inject = taiInjectAbsForKernel(KERNEL_PID, (void*)((uintptr_t)ScePervasiveForDriver_0xE9D95643 + 0x1D), nop, sizeof(nop));
    }

    g_mutex = ksceKernelCreateMutex("path_oc_mutex", 0, 0, NULL);

    // ScePower (user library): scePowerSetArmClockFrequency
    g_set_arm_hook = taiHookFunctionExportForKernel(KERNEL_PID, &g_set_arm_ref,
        "ScePower", 0x1082DA7F, 0x74DB5AE5, scePowerSetArmClockFrequency_patched);

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void* args)
{
    (void)argc;
    (void)args;
    if (g_set_arm_hook >= 0) {
        taiHookReleaseForKernel(g_set_arm_hook, g_set_arm_ref);
    }
    if (g_inject >= 0) {
        taiInjectReleaseForKernel(g_inject);
    }
    if (g_mutex >= 0) {
        ksceKernelDeleteMutex(g_mutex);
    }
    return SCE_KERNEL_STOP_SUCCESS;
}
