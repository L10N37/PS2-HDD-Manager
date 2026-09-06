#include <tamtypes.h>
#include <kernel.h>
#include <delaythread.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <libcdvd.h>
#include <debug.h>
#include <stdio.h>
#include <string.h>

#define PORT 0
#define SLOT 0
#define OSD_CONFIG_SIZE 15
#define CANONICAL_ENABLE_LOW_BITS 0x02

static char padBuf[256] __attribute__((aligned(64)));
static u32 oldPad = 0;
static u8 osdConfig[OSD_CONFIG_SIZE];
static int readOkay = 0;
static char lastOperation[96] = "None - startup is read-only";

static int open_config_block(int mode, u32 *lastStatus)
{
    int tries;
    for (tries = 0; tries < 300; tries++) {
        u32 status = 0;
        int result = sceCdOpenConfig(0, mode, 1, &status);
        if (lastStatus != NULL) *lastStatus = status;
        if (result != 0 && (status & 9) == 0)
            return 0;
        DelayThread(10000);
    }
    return -1;
}

static int close_config_block(u32 *lastStatus)
{
    int tries;
    for (tries = 0; tries < 300; tries++) {
        u32 status = 0;
        int result = sceCdCloseConfig(&status);
        if (lastStatus != NULL) *lastStatus = status;
        if (result != 0 && (status & 9) == 0)
            return 0;
        DelayThread(10000);
    }
    return -1;
}

static int read_osd_config(u8 *buffer)
{
    int tries;
    u32 status = 0;
    memset(buffer, 0, OSD_CONFIG_SIZE);

    if (open_config_block(0, &status) != 0)
        return -1;

    for (tries = 0; tries < 300; tries++) {
        int result;
        status = 0;
        result = sceCdReadConfig(buffer, &status);
        if (result != 0 && (status & 9) == 0)
            break;
        DelayThread(10000);
    }
    if (tries == 300) {
        close_config_block(NULL);
        return -2;
    }

    if (close_config_block(&status) != 0)
        return -3;
    return 0;
}

static int write_osd_config(const u8 *buffer)
{
    int tries;
    u32 status = 0;

    if (open_config_block(1, &status) != 0)
        return -10;

    for (tries = 0; tries < 300; tries++) {
        int result;
        status = 0;
        result = sceCdWriteConfig((u8 *)buffer, &status);
        if (result != 0 && (status & 9) == 0)
            break;
        DelayThread(10000);
    }
    if (tries == 300) {
        close_config_block(NULL);
        return -11;
    }

    if (close_config_block(&status) != 0)
        return -12;
    return 0;
}

static int hdd_boot_enabled(const u8 *buffer)
{
    // This is the exact condition used by FMCB/KELFBinder: low config bits
    // must equal 2 for ATAD support + HDD MBR boot to be enabled.
    return (buffer[0] & 3) == CANONICAL_ENABLE_LOW_BITS;
}

static void reread_status(const char *reason)
{
    int result = read_osd_config(osdConfig);
    readOkay = (result == 0);
    if (readOkay)
        snprintf(lastOperation, sizeof(lastOperation), "%s - verified read OK", reason);
    else
        snprintf(lastOperation, sizeof(lastOperation), "%s - READ ERROR %d", reason, result);
}

static int apply_state(int enable)
{
    u8 before[OSD_CONFIG_SIZE];
    u8 desired[OSD_CONFIG_SIZE];
    u8 verify[OSD_CONFIG_SIZE];
    int result = read_osd_config(before);
    if (result != 0) {
        snprintf(lastOperation, sizeof(lastOperation), "READ ERROR %d - nothing written", result);
        readOkay = 0;
        return result;
    }

    if ((enable && hdd_boot_enabled(before) && ((before[0] & 3) == CANONICAL_ENABLE_LOW_BITS)) ||
        (!enable && !hdd_boot_enabled(before))) {
        memcpy(osdConfig, before, sizeof(osdConfig));
        readOkay = 1;
        snprintf(lastOperation, sizeof(lastOperation), "%s - already in requested state; no write",
                 enable ? "Enable" : "Disable");
        return 0;
    }

    memcpy(desired, before, sizeof(desired));
    if (enable)
        desired[0] = (desired[0] & ~3) | CANONICAL_ENABLE_LOW_BITS;
    else
        // Preserve the ATAD/support bit and set the low-bit state to 3. This
        // changes only bit 0 from the canonical enabled state (2 -> 3), making
        // the exact FMCB/KELFBinder enabled predicate false without clearing
        // unrelated OSD configuration bits.
        desired[0] = (desired[0] & ~3) | 3;

    result = write_osd_config(desired);
    if (result != 0) {
        snprintf(lastOperation, sizeof(lastOperation), "%s WRITE FAILED %d",
                 enable ? "Enable" : "Disable", result);
        memcpy(osdConfig, before, sizeof(osdConfig));
        readOkay = 1;
        return result;
    }

    result = read_osd_config(verify);
    if (result != 0) {
        snprintf(lastOperation, sizeof(lastOperation), "%s write completed, VERIFY READ FAILED %d",
                 enable ? "Enable" : "Disable", result);
        readOkay = 0;
        return result;
    }

    memcpy(osdConfig, verify, sizeof(osdConfig));
    readOkay = 1;
    if ((enable && !hdd_boot_enabled(verify)) || (!enable && hdd_boot_enabled(verify))) {
        snprintf(lastOperation, sizeof(lastOperation), "%s VERIFY FAILED (byte0=0x%02X)",
                 enable ? "Enable" : "Disable", verify[0]);
        return -20;
    }

    snprintf(lastOperation, sizeof(lastOperation), "%s complete - VERIFIED (0x%02X -> 0x%02X)",
             enable ? "Enable" : "Disable", before[0], verify[0]);
    return 0;
}

static void render_main(void)
{
    scr_clear();
    scr_setXY(5, 3);
    scr_printf("PS2 HDD Manager - FHDB HDD Boot Configuration\n\n");
    if (!readOkay) {
        scr_printf("Current status : READ ERROR / UNKNOWN\n");
        scr_printf("No EEPROM write is allowed until status can be read.\n\n");
    } else {
        scr_printf("Current status : %s\n", hdd_boot_enabled(osdConfig) ? "ENABLED" : "DISABLED");
        scr_printf("OSD config byte0: 0x%02X   low boot flags: 0x%02X\n",
                   osdConfig[0], osdConfig[0] & 3);
        if (hdd_boot_enabled(osdConfig) && ((osdConfig[0] & 3) != CANONICAL_ENABLE_LOW_BITS))
            scr_printf("Note: HDD MBR boot is enabled, but flags are non-canonical.\n");
        scr_printf("\n");
    }
    scr_printf("Last operation : %s\n\n", lastOperation);
    scr_printf("CROSS    Enable HDD/FHDB boot\n");
    scr_printf("SQUARE   Disable HDD/FHDB boot\n");
    scr_printf("TRIANGLE Re-read / Verify status\n");
    scr_printf("CIRCLE   Exit to PS2 Browser\n\n");
    scr_printf("Startup never writes EEPROM. Enable/Disable always\n");
    scr_printf("re-read first and verify immediately after writing.\n");
}

static int wait_button(void)
{
    struct padButtonStatus buttons;
    int state = padGetState(PORT, SLOT);
    if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1)
        return 0;
    if (padRead(PORT, SLOT, &buttons) == 0)
        return 0;
    u32 current = 0xffff ^ buttons.btns;
    u32 pressed = current & ~oldPad;
    oldPad = current;
    return (int)pressed;
}

static void wait_all_buttons_released(void)
{
    struct padButtonStatus buttons;
    for (;;) {
        int state = padGetState(PORT, SLOT);
        if ((state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1) &&
            padRead(PORT, SLOT, &buttons) != 0) {
            u32 current = 0xffff ^ buttons.btns;
            if (current == 0) {
                oldPad = 0;
                return;
            }
        }
        DelayThread(16000);
    }
}

static int confirm_action(const char *action, const char *description)
{
    // The button that opened this screen must be released before it can
    // confirm an EEPROM write. This makes confirmation a deliberate second press.
    wait_all_buttons_released();
    for (;;) {
        scr_clear();
        scr_setXY(5, 5);
        scr_printf("CONFIRM %s\n\n", action);
        scr_printf("%s\n\n", description);
        scr_printf("CROSS  Confirm and write EEPROM\n");
        scr_printf("CIRCLE Cancel - do not write\n");
        const int pressed = wait_button();
        if (pressed & PAD_CROSS) return 1;
        if (pressed & PAD_CIRCLE) return 0;
        DelayThread(16000);
    }
}

static void load_pad_modules(void)
{
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    padInit(0);
    padPortOpen(PORT, SLOT, padBuf);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sceSifInitRpc(0);
    init_scr();
    load_pad_modules();
    sceCdInit(SCECdINoD);
    reread_status("Startup");

    for (;;) {
        render_main();
        const int pressed = wait_button();
        if (pressed & PAD_TRIANGLE) {
            reread_status("Manual verify");
        } else if (pressed & PAD_CROSS) {
            if (confirm_action("ENABLE HDD BOOT",
                    "This normalizes the HDD boot flags to the canonical FMCB/FHDB enabled state."))
                apply_state(1);
            else
                snprintf(lastOperation, sizeof(lastOperation), "Enable cancelled - no write");
        } else if (pressed & PAD_SQUARE) {
            if (confirm_action("DISABLE HDD BOOT",
                    "The console will stop automatically booting the HDD MBR until re-enabled."))
                apply_state(0);
            else
                snprintf(lastOperation, sizeof(lastOperation), "Disable cancelled - no write");
        } else if (pressed & PAD_CIRCLE) {
            padPortClose(PORT, SLOT);
            padEnd();
            LoadExecPS2("rom0:OSDSYS", 0, NULL);
            SleepThread();
        }
        DelayThread(16000);
    }
    return 0;
}
