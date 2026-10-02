#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#undef near
#undef far

#define TLOU_PLUGIN
#include "tlou_plugin_sdk.h"

#ifdef _MSC_VER
#define GUARD_TRY    __try
#define GUARD_EXCEPT __except (EXCEPTION_EXECUTE_HANDLER)
#else
#define GUARD_TRY    if (1)
#define GUARD_EXCEPT else
#endif

static const char* const modBaseName = "inventory_and_pickups_modifier";

/* ---------------------------------------------------------------- the log */

static char global_logPath[MAX_PATH];
static char global_iniPath[MAX_PATH];

static void writeLog(const char* fmt, ...) {
    FILE* f = nullptr;
    if (fopen_s(&f, global_logPath, "a") != 0 || !f) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u  ",
            now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* Formats into out. Answers 0 when the result does not fit in room, and out is
   then empty. */
static int formatInto(char* out, size_t room, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int written = _vsnprintf_s(out, room, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (written >= 0) return 1;
    out[0] = 0;
    return 0;
}

/* The log of the previous run, moved into the Archive folder beside it and
   named for the time it was last written. */
static void archiveLog() {
    WIN32_FILE_ATTRIBUTE_DATA about;
    if (!global_logPath[0]
        || !GetFileAttributesExA(global_logPath, GetFileExInfoStandard, &about))
        return;
    if (!about.nFileSizeLow && !about.nFileSizeHigh) return;

    /* The local time the log was written at, with that date's daylight
       saving offset. */
    SYSTEMTIME written, when;
    FileTimeToSystemTime(&about.ftLastWriteTime, &written);
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &written, &when)) {
        FILETIME local;
        FileTimeToLocalFileTime(&about.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &when);
    }

    char logs[MAX_PATH], folder[MAX_PATH], kept[MAX_PATH];
    formatInto(logs, sizeof(logs), "%s", global_logPath);
    char* slash = strrchr(logs, '\\');
    if (!slash) return;
    *slash = 0;
    if (!formatInto(folder, sizeof(folder), "%s\\Archive", logs)) return;
    CreateDirectoryA(folder, nullptr);
    if (!formatInto(kept, sizeof(kept), "%s\\%s_%04u%02u%02u_%02u%02u%02u.log",
                    folder, modBaseName, when.wYear, when.wMonth, when.wDay,
                    when.wHour, when.wMinute, when.wSecond))
        return;
    MoveFileA(global_logPath, kept);
}

/* ------------------------------------------------------- reading the game */

#define SMALLEST_POINTER 0x10000ull

/* The top of the user half of the address space. */
#define LARGEST_POINTER 0x00007FFFFFFFFFFFull

static int readQword(uint64_t address, uint64_t* out) {
    if (address < SMALLEST_POINTER || address > LARGEST_POINTER) return 0;
    int ok = 0;
    GUARD_TRY { *out = *(const uint64_t*)address; ok = 1; }
    GUARD_EXCEPT { ok = 0; }
    return ok;
}

static void copyText(char* out, size_t size, uint64_t address) {
    out[0] = 0;
    if (address < SMALLEST_POINTER || address > LARGEST_POINTER) return;
    GUARD_TRY {
        const char* s = (const char*)address;
        size_t i = 0;
        while (i + 1 < size && s[i]) {
            if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) {
                out[0] = 0;
                return;
            }
            out[i] = s[i];
            ++i;
        }
        out[i] = 0;
    } GUARD_EXCEPT { out[0] = 0; }
}

static uint64_t global_moduleBase = 0;
static uint64_t global_textBase = 0;
static uint64_t global_textSize = 0;

/* Fills the module and code addresses from the running game's own headers. */
static int findTheCode() {
    HMODULE game = GetModuleHandleA(nullptr);
    if (!game) return 0;
    global_moduleBase = (uint64_t)game;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)game;
    const IMAGE_NT_HEADERS64* nt =
        (const IMAGE_NT_HEADERS64*)((const uint8_t*)game + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(section[i].Name, ".text", 5) != 0) continue;
        global_textBase = global_moduleBase + section[i].VirtualAddress;
        global_textSize = section[i].Misc.VirtualSize;
        return 1;
    }
    return 0;
}

/* The one place in the game's code where these bytes appear, or 0. */
static uint64_t onlyPlaceOf(const uint8_t* pattern, size_t length) {
    if (!global_textBase || global_textSize < length) return 0;
    const uint8_t* text = (const uint8_t*)global_textBase;
    const uint64_t last = global_textSize - length;
    uint64_t found = 0;
    int count = 0;
    GUARD_TRY {
        for (uint64_t at = 0; at <= last; ++at) {
            if (text[at] != pattern[0]) continue;
            if (memcmp(text + at, pattern, length) != 0) continue;
            found = global_textBase + at;
            if (++count > 1) break;
        }
    } GUARD_EXCEPT { count = 0; }
    return count == 1 ? found : 0;
}

/* ------------------------------------------------------------- the items */

/* FNV-1a 64. Every id in the engine is this hash of a name. */
static uint64_t idOfName(const char* text) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (const unsigned char* at = (const unsigned char*)text; *at; ++at) {
        hash ^= (uint64_t)*at;
        hash *= 0x100000001B3ull;
    }
    return hash;
}


#define GROUP_CRAFTING_MATERIALS  0
#define GROUP_AMMO                1
#define GROUP_ITEMS               2
#define GROUP_PARTS               3
#define GROUP_COUNT               4

/* What a group's settings are called, after multiply-all- and the rest. */
static const char* const global_groupName[GROUP_COUNT] = {
    "crafting-materials", "ammo", "items", "parts-and-supplements"
};

/* 1 where a group's band sizes are written in whole items and counted in
   quarters, as its items are. */
static const int global_groupQuartered[GROUP_COUNT] = { 1, 0, 0, 0 };

struct Item {
    const char* plain;      /* what a setting and the menu call it */
    const char* gameName;   /* the game's own name, which hashes to its id */
    int         quartered;
    int         vanilla;    /* the game's own carry limit, in whole items */
    int         group;      /* GROUP_ above */
};

static const Item global_item[] = {
    { "rag",                 "t1x-rag",                         1,    3, GROUP_CRAFTING_MATERIALS },
    { "binding",             "t1x-binding",                     1,    3, GROUP_CRAFTING_MATERIALS },
    { "blade",               "t1x-blade",                       1,    3, GROUP_CRAFTING_MATERIALS },
    { "alcohol",             "t1x-alcohol",                     1,    3, GROUP_CRAFTING_MATERIALS },
    { "explosive",           "t1x-explosive",                   1,    3, GROUP_CRAFTING_MATERIALS },
    { "sugar",               "t1x-sugar",                       1,    3, GROUP_CRAFTING_MATERIALS },

    { "parts",               "t1x-parts",                       0, 9999, GROUP_PARTS },
    { "supplements",         "t1x-player-upgrade-supplements",  0, 9999, GROUP_PARTS },

    { "ammo-9mm",            "t1x-ammo-9mm",                    0,   14, GROUP_AMMO },
    { "ammo-shotgun",        "t1x-ammo-shotgun",                0,   10, GROUP_AMMO },
    { "ammo-rifle",          "t1x-ammo-rifle",                  0,    6, GROUP_AMMO },
    { "ammo-revolver",       "t1x-ammo-revolver",               0,   12, GROUP_AMMO },
    { "ammo-arrow",          "t1x-ammo-arrow",                  0,    8, GROUP_AMMO },
    { "ammo-flamethrower",   "t1x-ammo-flamethrower",           0,  100, GROUP_AMMO },
    { "ammo-assault-rifle",  "t1x-ammo-assault-rifle",          0,   60, GROUP_AMMO },
    { "ammo-shorty",         "t1x-ammo-pistol-shotgun",         0,    4, GROUP_AMMO },
    { "ammo-el-diablo",      "t1x-ammo-pistol-uber",            0,    6, GROUP_AMMO },

    { "health-kit",          "t1x-health-kit",                  0,    3, GROUP_ITEMS },
    { "molotov",             "t1x-molotov",                     0,    3, GROUP_ITEMS },
    { "nail-bomb",           "t1x-nail-bomb",                   0,    3, GROUP_ITEMS },
    { "smoke-bomb",          "t1x-smoke-bomb",                  0,    3, GROUP_ITEMS },
    { "shiv",                "t1x-shiv",                        0,    3, GROUP_ITEMS },
};

#define ITEM_COUNT ((int)(sizeof(global_item) / sizeof(global_item[0])))

/* The most bands a group's amounts can be divided into, and how many they are
   divided into where the settings do not say. */
#define MAX_BANDS          10
#define BAND_COUNT_DEFAULT 4

/*
    on     the item is multiplied at all
    band   the multiplier for each band; only as many as its group's count
           are used
    limit  carry limit in whole items
    id     the item's id, worked out at load
*/
struct ItemSetting {
    int      on;
    double   band[MAX_BANDS];
    int      limit;
    uint64_t id;
};

static ItemSetting global_setting[ITEM_COUNT];

/*
    How many bands each group's amounts are divided into, 1 to MAX_BANDS. At 1
    every amount takes band one's multiplier.
*/
static int global_bandCount[GROUP_COUNT] = {
    BAND_COUNT_DEFAULT, BAND_COUNT_DEFAULT, BAND_COUNT_DEFAULT, BAND_COUNT_DEFAULT
};

/*
    The largest amount each band of a group covers, in the group's own units:
    quarters where the group is quartered. The last band in use is everything
    above the band before it, so only the ends of the bands before it are
    read. Each is the band's size setting as it stands. A band whose end is
    not above the end of the band before it covers nothing.
*/
static int global_bandEnd[GROUP_COUNT][MAX_BANDS - 1];

/* --------------------------------------------------------- the settings */

static void readSettings() {
    char text[64], key[128];

    /* A band size the settings do not give, or give as less than one unit,
       is taken from here, in units. */
    static const int sizeFallback[MAX_BANDS - 1] = {
        1, 5, 10, 11, 12, 13, 14, 15, 16
    };
    for (int g = 0; g < GROUP_COUNT; ++g) {
        sprintf_s(key, "count-%s", global_groupName[g]);
        int count = GetPrivateProfileIntA("bands", key, BAND_COUNT_DEFAULT,
                                          global_iniPath);
        if (count < 1 || count > MAX_BANDS) count = BAND_COUNT_DEFAULT;

        const int unit = global_groupQuartered[g] ? 4 : 1;
        for (int b = 0; b < count - 1; ++b) {
            sprintf_s(key, "band%d-size-%s", b + 1, global_groupName[g]);
            GetPrivateProfileStringA("bands", key, "", text,
                                     (DWORD)sizeof(text), global_iniPath);
            const double size = text[0] ? atof(text) * unit : 0;
            global_bandEnd[g][b] = size >= 1 && size <= 100000
                                 ? (int)(size + 0.5) : sizeFallback[b];
        }
        /* Only once the ends it uses are in place. */
        global_bandCount[g] = count;
    }

    /*
        A group's own settings, which stand in for those of every item in it
        while the group is turned on. The menu writes these under the group's
        name in place of an item's.
    */
    int    groupOn[GROUP_COUNT], groupLimitOn[GROUP_COUNT];
    int    groupLimit[GROUP_COUNT];
    double groupBand[GROUP_COUNT][MAX_BANDS];
    for (int g = 0; g < GROUP_COUNT; ++g) {
        sprintf_s(key, "multiply-all-%s", global_groupName[g]);
        groupOn[g] = GetPrivateProfileIntA("multipliers", key, 0, global_iniPath);
        /* A band past the group's count is not read: it is not used. */
        for (int b = 0; b < MAX_BANDS; ++b) {
            groupBand[g][b] = 1;
            if (b >= global_bandCount[g]) continue;
            sprintf_s(key, "band%d-all-%s", b + 1, global_groupName[g]);
            GetPrivateProfileStringA("multipliers", key, "1", text,
                                     (DWORD)sizeof(text), global_iniPath);
            groupBand[g][b] = atof(text);
            if (groupBand[g][b] <= 0 || groupBand[g][b] > 1000)
                groupBand[g][b] = 1;
        }
        sprintf_s(key, "limit-all-%s", global_groupName[g]);
        groupLimitOn[g] = GetPrivateProfileIntA("carry limits", key, 0,
                                                global_iniPath);
        sprintf_s(key, "limit-amount-all-%s", global_groupName[g]);
        groupLimit[g] = GetPrivateProfileIntA("carry limits", key, 0,
                                              global_iniPath);
        if (groupLimit[g] < 1 || groupLimit[g] > 9999) groupLimitOn[g] = 0;
    }

    for (int i = 0; i < ITEM_COUNT; ++i) {
        ItemSetting* s = &global_setting[i];
        const int g = global_item[i].group;
        s->id = idOfName(global_item[i].gameName);

        if (groupOn[g]) {
            s->on = 1;
            for (int b = 0; b < MAX_BANDS; ++b) s->band[b] = groupBand[g][b];
        } else {
            sprintf_s(key, "multiply-%s", global_item[i].plain);
            s->on = GetPrivateProfileIntA("multipliers", key, 0, global_iniPath);

            for (int b = 0; b < MAX_BANDS; ++b) {
                double by = 1;
                if (b < global_bandCount[g]) {
                    sprintf_s(key, "band%d-%s", b + 1, global_item[i].plain);
                    GetPrivateProfileStringA("multipliers", key, "1", text,
                                             (DWORD)sizeof(text),
                                             global_iniPath);
                    by = atof(text);
                    if (by <= 0 || by > 1000) by = 1;
                }
                s->band[b] = by;
            }
        }

        if (groupLimitOn[g]) {
            s->limit = groupLimit[g];
        } else {
            sprintf_s(key, "limit-%s", global_item[i].plain);
            s->limit = GetPrivateProfileIntA("carry limits", key,
                                             global_item[i].vanilla,
                                             global_iniPath);
            if (s->limit < 1 || s->limit > 9999)
                s->limit = global_item[i].vanilla;
        }
    }
}

/*
    The settings are read only once the ini's last-write time has stayed put
    across a further look.
*/
#define SETTINGS_LOOK_EVERY_MS 250u

static FILETIME global_iniReadAt;      /* the write time the settings came from */
static FILETIME global_iniSettlingAt;  /* a newer write time, not yet settled */
static int      global_iniSettling = 0;
static DWORD    global_lastLookAt = 0;

/* 1 and the ini's last-write time in out, 0 where the file cannot be reached. */
static int iniWrittenAt(FILETIME* out) {
    WIN32_FILE_ATTRIBUTE_DATA about;
    if (!GetFileAttributesExA(global_iniPath, GetFileExInfoStandard, &about))
        return 0;
    *out = about.ftLastWriteTime;
    return 1;
}

static int sameTime(const FILETIME* a, const FILETIME* b) {
    return a->dwLowDateTime  == b->dwLowDateTime
        && a->dwHighDateTime == b->dwHighDateTime;
}

/*
    Answers 1 on the look that settles a new write, having read the settings
    again. Meant to be called from the tick; it reaches the file no more often
    than SETTINGS_LOOK_EVERY_MS.
*/
static int settingsRereadIfChanged() {
    const DWORD now = GetTickCount();
    if (now - global_lastLookAt < SETTINGS_LOOK_EVERY_MS) return 0;
    global_lastLookAt = now;

    FILETIME written;
    if (!iniWrittenAt(&written)) return 0;
    if (sameTime(&written, &global_iniReadAt)) {
        global_iniSettling = 0;
        return 0;
    }
    if (!global_iniSettling || !sameTime(&written, &global_iniSettlingAt)) {
        global_iniSettlingAt = written;
        global_iniSettling = 1;
        return 0;
    }

    global_iniReadAt = written;
    global_iniSettling = 0;
    readSettings();
    return 1;
}

/* The item's place in the table, or -1. */
static int itemOfId(uint64_t id) {
    for (int i = 0; i < ITEM_COUNT; ++i)
        if (global_setting[i].id == id) return i;
    return -1;
}

/*
    The amount a pickup should hold, given the amount it was about to hold.
    Answers the amount unchanged for an item that is off, for an amount of
    zero or less, and where the band's multiplier is one. Rounded to the
    nearest whole number and never below one.

    bandOut receives the band that matched, counted from one, and byOut its
    multiplier. Both say 0 and 1 for an item that is off and for an amount of
    zero or less.
*/
static int amountWanted(uint64_t id, int amount, int* bandOut, double* byOut) {
    *bandOut = 0;
    *byOut = 1;
    if (amount <= 0) return amount;
    const int i = itemOfId(id);
    if (i < 0 || !global_setting[i].on) return amount;

    const int group = global_item[i].group;
    const int* end = global_bandEnd[group];
    const int count = global_bandCount[group];
    int band = count - 1;
    for (int b = 0; b < count - 1; ++b)
        if (amount <= end[b]) { band = b; break; }

    const double by = global_setting[i].band[band];
    *bandOut = band + 1;
    *byOut = by;
    if (by == 1) return amount;

    double made = (double)amount * by + 0.5;
    if (made < 1) made = 1;
    if (made > 1000000) made = 1000000;
    return (int)made;
}

/* --------------------------------------------- pickups already standing */

/*
    Where a pickup keeps what it holds, which item it is, and the flag that
    makes its prompt redraw, read out of two runs of bytes that each stand once
    in the game's code:

        41 89 8C 24 <amount>    mov [r12 + amount], ecx      in the setup
        48 8D 8C 24 60 02 00 00 lea rcx, [rsp + 0x260]
        4D 89 B4 24 <item>      mov [r12 + item], r14

        41 89 86 <amount>       mov [r14 + amount], eax      in the game's setter
        3B C8                   cmp ecx, eax
        74 07                   je past the next
        41 88 9E <flag>         mov byte ptr [r14 + flag], bl

    The flag must be cleared whenever the amount changes.
*/
static const uint8_t amountSetterAnchor[] = {
    0x41,0x89,0x86,0x50,0x07,0x00,0x00,
    0x3B,0xC8,
    0x74,0x07,
    0x41,0x88,0x9E,0xD0,0x06,0x00,0x00
};

static int global_amountAt = 0;     /* what a pickup holds */
static int global_itemAt = 0;       /* which item it is */
static int global_promptFlagAt = 0; /* the prompt flag; 0 where not known */

/*
    A pickup is a process, and the engine moves processes in memory while the
    game runs; the address a pickup is set up at does not stay that pickup.
    Each process is a block in a heap whose records do not move:

        process + PROCESS_RECORD   the process's heap record
        process + PROCESS_ID       the process's id, 32 bits
        record  + RECORD_OBJECT    where the process is now

    The game names a process by its record and its id together: a record is
    used again once its process is gone, and the id tells the new process from
    the old.

    All three offsets are checked against the game's code before anything is
    followed. Process::SpawnProcess writes the process into its record and the
    record into the process, once in the whole game:

        49 89 74 24 08          mov [r12 + RECORD_OBJECT], rsi
        4C 89 66 70             mov [rsi + PROCESS_RECORD], r12

    and the game's own handle resolve reads the id back out of the process:

        8B 51 08                mov edx, [rcx + 8]            the id
        85 D2                   test edx, edx
        0F 84 <offset>          je nothing
        49 8B 48 08             mov rcx, [r8 + RECORD_OBJECT]
        48 85 C9                test rcx, rcx
        0F 84 <offset>          je nothing
        39 91 98 00 00 00       cmp [rcx + PROCESS_ID], edx
        48 0F 44 C1             cmove rax, rcx
        C3                      ret

    The record and the id are both in place when the hook in the pickup's
    setup runs.
*/
#define PROCESS_RECORD  0x70
#define PROCESS_ID      0x98
#define RECORD_OBJECT   0x08

static const uint8_t spawnRecordAnchor[] = {
    0x49,0x89,0x74,0x24,0x08,
    0x4C,0x89,0x66,0x70
};
static const uint8_t handleResolveAnchor[] = {
    0x8B,0x51,0x08,
    0x85,0xD2,
    0x0F,0x84,0x1F,0x20,0x22,0xFF,
    0x49,0x8B,0x48,0x08,
    0x48,0x85,0xC9,
    0x0F,0x84,0x12,0x20,0x22,0xFF,
    0x39,0x91,0x98,0x00,0x00,0x00,
    0x48,0x0F,0x44,0xC1,
    0xC3
};

/* 1 once both anchors stood where expected, which is the only case a process
   is followed by its handle. */
static int global_handlesKnown = 0;

static int findHandleLayout() {
    const uint64_t spawn = onlyPlaceOf(spawnRecordAnchor,
                                       sizeof(spawnRecordAnchor));
    const uint64_t resolve = onlyPlaceOf(handleResolveAnchor,
                                         sizeof(handleResolveAnchor));
    global_handlesKnown = spawn && resolve;
    if (!global_handlesKnown)
        writeLog("PROCESS %s not found where expected, so no process is "
                 "followed once it is set up",
                 !spawn && !resolve
                     ? "the game's spawn and its handle resolve were"
                     : !spawn ? "the game's spawn was"
                              : "the game's handle resolve was");
    return global_handlesKnown;
}

/* The process a handle names now, or 0. A handle is a record and an id, as
   the game keeps them: the record at +0 and the id at +8. Reads only. */
static uint64_t processOf(uint64_t record, uint32_t id) {
    if (!global_handlesKnown || !id
        || record < SMALLEST_POINTER || record > LARGEST_POINTER) return 0;
    uint64_t object = 0;
    GUARD_TRY {
        object = *(volatile uint64_t*)(record + RECORD_OBJECT);
        if (object < SMALLEST_POINTER || object > LARGEST_POINTER
            || *(volatile uint32_t*)(object + PROCESS_ID) != id)
            object = 0;
    } GUARD_EXCEPT { object = 0; }
    return object;
}

/* The handle of the process at object. 1 where the handle names it. */
static int handleOf(uint64_t object, uint64_t* recordOut, uint32_t* idOut) {
    *recordOut = 0;
    *idOut = 0;
    if (!global_handlesKnown
        || object < SMALLEST_POINTER || object > LARGEST_POINTER) return 0;
    uint64_t record = 0;
    uint32_t id = 0;
    GUARD_TRY {
        record = *(volatile uint64_t*)(object + PROCESS_RECORD);
        id = *(volatile uint32_t*)(object + PROCESS_ID);
    } GUARD_EXCEPT { record = 0; id = 0; }
    if (processOf(record, id) != object) return 0;
    *recordOut = record;
    *idOut = id;
    return 1;
}

/*
    One pickup standing in the world.

    record, id  its handle, taken when it was set up; record is written last
                and is 0 while the entry is being filled
    item        which item it is
    vanilla     what the game set it up holding, which every multiplier applies
                to
    made        what this plugin last set it to
    taken       how much less it has held than this plugin made it, added up
                over each change to the settings, which is what the player
                took from it while it stood
*/
#define MAX_LIVE_PICKUPS 2048
struct LivePickup {
    volatile uint64_t record;
    uint32_t          id;
    uint64_t          item;
    int               vanilla;
    int               made;
    int               taken;
};
static LivePickup global_live[MAX_LIVE_PICKUPS];
static volatile LONG global_liveCount = 0;
static volatile LONG global_liveLock = 0;
static volatile LONG global_liveFull = 0;
static volatile LONG global_noHandle = 0;
static volatile LONG global_liveRefreshes = 0;

/* Where the pickup is now, or 0 where its handle no longer names it or the
   process it names is not that item. */
static uint64_t pickupNow(const LivePickup* one) {
    const uint64_t record = one->record;
    if (!record || !global_itemAt) return 0;
    const uint64_t object = processOf(record, one->id);
    if (!object) return 0;
    uint64_t item = 0;
    GUARD_TRY { item = *(volatile uint64_t*)(object + global_itemAt); }
    GUARD_EXCEPT { item = 0; }
    return item == one->item ? object : 0;
}

/* An entry whose pickup has left the level, or the next unused one. Nothing
   where the table is full. Called with the lock held. */
static LivePickup* freeLiveEntry() {
    const LONG count = global_liveCount;
    for (LONG i = 0; i < count; ++i)
        if (!pickupNow(&global_live[i])) return &global_live[i];
    if (count >= MAX_LIVE_PICKUPS) return nullptr;
    return &global_live[InterlockedIncrement(&global_liveCount) - 1];
}

/* Remembers a pickup just set up. Any thread; the game thread may be reading
   the table meanwhile. */
static void rememberPickup(uint64_t object, uint64_t item, int vanilla,
                           int made) {
    uint64_t record = 0;
    uint32_t id = 0;
    if (!handleOf(object, &record, &id)) {
        InterlockedIncrement(&global_noHandle);
        return;
    }
    /* Nothing here may hold a loading thread. */
    for (int spins = 0;
         InterlockedCompareExchange(&global_liveLock, 1, 0) != 0; ++spins) {
        if (spins > 100000) {
            InterlockedIncrement(&global_liveFull);
            return;
        }
        YieldProcessor();
    }
    LivePickup* one = freeLiveEntry();
    if (one) {
        InterlockedExchange64((volatile LONG64*)&one->record, 0);
        one->id = id;
        one->item = item;
        one->vanilla = vanilla;
        one->made = made;
        one->taken = 0;
        InterlockedExchange64((volatile LONG64*)&one->record, (LONG64)record);
    } else {
        InterlockedIncrement(&global_liveFull);
    }
    InterlockedExchange(&global_liveLock, 0);
}

/* What the pickup at object holds, or -1 where it cannot be read. */
static int pickupHolds(uint64_t object) {
    int holds = -1;
    GUARD_TRY { holds = *(volatile int*)(object + global_amountAt); }
    GUARD_EXCEPT { holds = -1; }
    return holds;
}

/* What the pickup at object holds, set to amount, with its prompt flag
   cleared where the number changed. */
static void setPickupAmount(uint64_t object, int amount) {
    GUARD_TRY {
        volatile int* holds = (volatile int*)(object + global_amountAt);
        if (*holds != amount) {
            *holds = amount;
            *(volatile uint8_t*)(object + global_promptFlagAt) = 0;
        }
    } GUARD_EXCEPT { }
}

/*
    Every pickup standing in the level, set to what the settings now say,
    counted from what it was set up holding, less whatever the player has
    already taken from it. One that the player has taken more from than the
    new settings would give keeps one. Game thread only; runs once for each
    change to the settings.
*/
static void refreshLivePickups() {
    if (!global_amountAt || !global_itemAt || !global_promptFlagAt
        || !global_handlesKnown) return;
    const LONG count = global_liveCount;
    for (LONG i = 0; i < count; ++i) {
        LivePickup* one = &global_live[i];
        if (!one->record) continue;
        const uint64_t object = pickupNow(one);
        if (!object) continue;
        const int holds = pickupHolds(object);
        if (holds < 0) continue;
        if (holds < one->made) one->taken += one->made - holds;
        int band = 0;
        double by = 1;
        int wants = amountWanted(one->item, one->vanilla, &band, &by)
                  - one->taken;
        if (wants < 1) wants = 1;
        setPickupAmount(object, wants);
        one->made = wants;
    }
    InterlockedIncrement(&global_liveRefreshes);
}

/* ------------------------------------------- the amount a pickup holds */

/*
    The three writes that settle a pickup, which appear once in the game's
    code:

        mov dword ptr [r12 + 0x750], ecx     the amount
        lea rcx, [rsp + 0x260]
        mov qword ptr [r12 + 0x748], r14     the item

    The hook sits on the first. At that instant rcx holds the amount about to
    be stored and r14 holds the item.
*/
static const uint8_t spawnAmountAnchor[] = {
    0x41,0x89,0x8C,0x24,0x50,0x07,0x00,0x00,
    0x48,0x8D,0x8C,0x24,0x60,0x02,0x00,0x00,
    0x4D,0x89,0xB4,0x24,0x48,0x07,0x00,0x00
};

/* Reads the three offsets out of the game's own instructions. 1 where the
   amount and item offsets were read. */
static int findAmountOffsets() {
    const uint64_t settle = onlyPlaceOf(spawnAmountAnchor,
                                        sizeof(spawnAmountAnchor));
    if (!settle) return 0;
    GUARD_TRY {
        const uint8_t* at = (const uint8_t*)settle;
        memcpy(&global_amountAt, at + 4, 4);
        memcpy(&global_itemAt, at + 20, 4);
    } GUARD_EXCEPT { global_amountAt = 0; global_itemAt = 0; }

    const uint64_t setter = onlyPlaceOf(amountSetterAnchor,
                                        sizeof(amountSetterAnchor));
    if (setter) {
        GUARD_TRY {
            const uint8_t* at = (const uint8_t*)setter;
            int amountAgain = 0;
            memcpy(&amountAgain, at + 3, 4);
            memcpy(&global_promptFlagAt, at + 14, 4);
            if (amountAgain != global_amountAt) {
                writeLog("PICKUP the two places the game writes what a pickup "
                         "holds do not agree, so a standing pickup is left "
                         "alone");
                global_promptFlagAt = 0;
            }
        } GUARD_EXCEPT { global_promptFlagAt = 0; }
    } else {
        writeLog("PICKUP the game's own way of setting a pickup's amount was "
                 "not found, so a standing pickup is left alone");
    }
    return global_amountAt != 0 && global_itemAt != 0;
}

static volatile LONG global_pickupsSeen = 0;

static const char* plainNameOfId(uint64_t id) {
    const int i = itemOfId(id);
    return i >= 0 ? global_item[i].plain : "an item";
}

/* Runs on a loading thread: nothing in it may write a file or hold the
   thread. */
static void pickupSpawnAmount(void* registers, void* user) {
    (void)user;
    GUARD_TRY {
        const uint64_t item = loaderHookRegister(registers, LOADER_REG_R14);
        const int vanilla = (int)(int32_t)(uint32_t)loaderHookRegister(
            registers, LOADER_REG_RCX);
        const uint64_t object = loaderHookRegister(registers, LOADER_REG_R12);

        int band = 0;
        double by = 1;
        const int became = amountWanted(item, vanilla, &band, &by);
        if (became != vanilla)
            loaderHookSetRegister(registers, LOADER_REG_RCX,
                                  (uint64_t)(uint32_t)(int32_t)became);
        InterlockedIncrement(&global_pickupsSeen);

        rememberPickup(object, item, vanilla, became);
    } GUARD_EXCEPT { }
}

/* ------------------------------------------------------- the carry limits */

/*
    The item map, as the engine keeps it:

        +0x00  how many items
        +0x08  a list of item ids, sorted
        +0x10  a list of pointers to the records, in the same order

    A record is two hundred bytes:

        +0x00  the item's own id, which repeats the key that found it
        +0x08  a pointer to the item's name, which hashes to that id
        +0x88  the carry limit, as a 32 bit count

    Every entry must check out, its id repeating the key and its name hashing
    to that id, before anything is written.
*/
#define ITEM_MAP_COUNT    0x00
#define ITEM_MAP_IDS      0x08
#define ITEM_MAP_RECORDS  0x10
#define ITEM_ID           0x00
#define ITEM_NAME_TEXT    0x08
#define ITEM_CARRY_LIMIT  0x88
#define MAX_ITEMS         4096

/* The symbol the map is bound to, and the module it is bound in. */
#define ITEM_MAP_SYMBOL   0xA26521A3BC083B01ull
#define ITEM_MAP_MODULE   "t1x-player-inventory"

typedef const void* (*LookupInModuleFn)(uint64_t symbol, uint64_t module);
static LookupInModuleFn global_lookupInModule = nullptr;

static int readItemRecord(uint64_t key, uint64_t record,
                          char* nameOut, size_t nameSize) {
    uint64_t idInRecord = 0, nameText = 0;
    nameOut[0] = 0;
    if (record < SMALLEST_POINTER) return 0;
    if (!readQword(record + ITEM_ID, &idInRecord) || idInRecord != key) return 0;
    if (!readQword(record + ITEM_NAME_TEXT, &nameText)) return 0;
    copyText(nameOut, nameSize, nameText);
    if (!nameOut[0]) return 0;
    return idOfName(nameOut) == key;
}

/* 1 when every entry checks out, which is the only case anything is written. */
static int readItemMap(uint64_t data, uint64_t* countOut,
                       uint64_t* idsOut, uint64_t* recordsOut) {
    uint64_t count = 0, ids = 0, records = 0;
    if (!readQword(data + ITEM_MAP_COUNT, &count)) return 0;
    if (!readQword(data + ITEM_MAP_IDS, &ids)) return 0;
    if (!readQword(data + ITEM_MAP_RECORDS, &records)) return 0;
    if (count == 0 || count > MAX_ITEMS) return 0;
    if (ids < SMALLEST_POINTER || records < SMALLEST_POINTER) return 0;

    for (uint64_t i = 0; i < count; ++i) {
        uint64_t key = 0, record = 0;
        char itemName[128];
        if (!readQword(ids + i * 8, &key)) return 0;
        if (!readQword(records + i * 8, &record)) return 0;
        if (!readItemRecord(key, record, itemName, sizeof(itemName))) return 0;
    }
    *countOut = count;
    *idsOut = ids;
    *recordsOut = records;
    return 1;
}

/*
    The map the game itself reads from.

    The game reaches the item map through a function that looks the symbol up
    once and keeps the answer in a variable of its own, returning that variable
    ever after. That function is the only place in the game's code where the
    symbol's eight bytes stand as an immediate, and the instruction after it
    loads the address of the variable:

        48 BA <symbol>       movabs rdx, the symbol
        48 8D 0D <offset>    lea rcx, the variable
        E8 <offset>          call the lookup, which writes through rcx

    The variable is at the end of that lea plus its offset. Reading it gives
    the pointer the game reads, and zero until the game has asked for the map
    once.
*/
static uint64_t global_gameMapVariable = 0;
static int      global_gameMapLooked = 0;
static int      global_wroteIntoGameMap = 0;
static DWORD    global_lastMapTry = 0;

static uint64_t gameItemMapVariable() {
    if (global_gameMapLooked) return global_gameMapVariable;
    global_gameMapLooked = 1;

    uint8_t pattern[10];
    pattern[0] = 0x48;
    pattern[1] = 0xBA;
    for (int i = 0; i < 8; ++i)
        pattern[2 + i] = (uint8_t)((ITEM_MAP_SYMBOL >> (i * 8)) & 0xFF);

    const uint64_t site = onlyPlaceOf(pattern, sizeof(pattern));
    if (!site) {
        writeLog("ITEMMAP the symbol does not stand exactly once in the game's "
                 "code, so the game's own item map variable was not found");
        return 0;
    }
    GUARD_TRY {
        const uint8_t* lea = (const uint8_t*)(site + 10);
        if (lea[0] == 0x48 && lea[1] == 0x8D && lea[2] == 0x0D) {
            int32_t offset = 0;
            memcpy(&offset, lea + 3, 4);
            global_gameMapVariable = (uint64_t)(lea + 7) + (int64_t)offset;
        }
    } GUARD_EXCEPT { global_gameMapVariable = 0; }

    if (!global_gameMapVariable)
        writeLog("ITEMMAP what follows the symbol at rva %08X is not the "
                 "instruction expected, so the game's own variable was not read",
                 (unsigned)(site - global_moduleBase));
    return global_gameMapVariable;
}

/* ------------------------------------------------- the player's inventory */

/*
    What the player holds, as the game keeps it:

        inventory + INVENTORY_COUNT   how many entries, 32 bits
        inventory + INVENTORY_LIST    the entries, ENTRY_SIZE bytes each

        entry + ENTRY_ITEM            the item's id
        entry + ENTRY_LIMIT           the carry limit, 32 bits

    An entry's carry limit is copied from the item's record when the entry is
    made, which is at the first pickup of an item and for every entry when a
    save is loaded, and the game reads the entry's copy, not the record, for
    any item the player already holds.

    Each of these constants is checked against the game's own code before
    anything is written. The game finds an entry here, once in its code:

        45 33 C0                xor r8d, r8d
        48 85 D2                test rdx, rdx
        74 21                   je none
        44 8B 09                mov r9d, [rcx + INVENTORY_COUNT]
        45 85 C9                test r9d, r9d
        74 19                   je none
        4C 8B 51 08             mov r10, [rcx + INVENTORY_LIST]
        41 8B C0                mov eax, r8d          the next entry
        48 6B C8 38             imul rcx, rax, ENTRY_SIZE
        4A 39 14 11             cmp [rcx + r10 + ENTRY_ITEM], rdx
        74 0C                   je found
        41 FF C0                inc r8d
        45 3B C1                cmp r8d, r9d
        72 EB                   jb the next entry
        83 C8 FF                or eax, -1            none
        C3                      ret
        41 8B C0                mov eax, r8d          found
        C3                      ret

    and reads an entry's limit here, also once:

        48 89 5C 24 08          mov [rsp + 8], rbx
        48 89 74 24 10          mov [rsp + 0x10], rsi
        57                      push rdi
        48 83 EC 20             sub rsp, 0x20
        8B 72 18                mov esi, [rdx + ENTRY_LIMIT]
        48 8B DA                mov rbx, rdx
        48 8B 12                mov rdx, [rdx + ENTRY_ITEM]
        48 8B F9                mov rdi, rcx

    The game reaches the inventory through the player's handle, which it keeps
    in a variable of its own, and reads it this way:

        48 8D 0D <offset>       lea rcx, the player's handle
        E8 <offset>             call the handle resolve
        48 8B C8                mov rcx, rax
        E8 <offset>             call the function that answers the inventory

    The resolve stands once in the game's code:

        48 83 EC 28             sub rsp, 0x28
        E8 13 00 00 00          call the part that reads the handle
        48 85 C0                test rax, rax
        75 05                   jne marked
        48 83 C4 28             add rsp, 0x28
        C3                      ret
        90 C6 40 42 01 90       nop, mov byte ptr [rax + 0x42], 1, nop
        EB F3                   jmp back

    and the function that answers the inventory stands twice, byte for byte:

        48 83 EC 28             sub rsp, 0x28
        48 8B 89 F8 2F 00 00    mov rcx, [rcx + 0x2FF8]
        33 C0                   xor eax, eax
        48 85 C9                test rcx, rcx
        74 09                   je nothing
        48 8B 01                mov rax, [rcx]
        FF 90 D0 01 00 00       call [rax + 0x1D0]
        48 83 C4 28             add rsp, 0x28
        C3                      ret

    Every place that fits all three must name the same variable. The handle is
    read the way the resolve reads it, without its mark; the function that
    answers the inventory is the game's own, called on the game thread.
*/
#define INVENTORY_COUNT  0x00
#define INVENTORY_LIST   0x08
#define ENTRY_SIZE       0x38
#define ENTRY_ITEM       0x00
#define ENTRY_LIMIT      0x18
#define MAX_ENTRIES      72     /* the game refuses a seventy-third */

static const uint8_t findEntryAnchor[] = {
    0x45,0x33,0xC0, 0x48,0x85,0xD2, 0x74,0x21,
    0x44,0x8B,0x09, 0x45,0x85,0xC9, 0x74,0x19,
    0x4C,0x8B,0x51,0x08, 0x41,0x8B,0xC0, 0x48,0x6B,0xC8,0x38,
    0x4A,0x39,0x14,0x11, 0x74,0x0C, 0x41,0xFF,0xC0, 0x45,0x3B,0xC1,
    0x72,0xEB, 0x83,0xC8,0xFF, 0xC3, 0x41,0x8B,0xC0, 0xC3
};
static const uint8_t entryLimitAnchor[] = {
    0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x74,0x24,0x10, 0x57,
    0x48,0x83,0xEC,0x20, 0x8B,0x72,0x18, 0x48,0x8B,0xDA,
    0x48,0x8B,0x12, 0x48,0x8B,0xF9
};
static const uint8_t outerResolveAnchor[] = {
    0x48,0x83,0xEC,0x28, 0xE8,0x13,0x00,0x00,0x00, 0x48,0x85,0xC0,
    0x75,0x05, 0x48,0x83,0xC4,0x28, 0xC3,
    0x90, 0xC6,0x40,0x42,0x01, 0x90, 0xEB,0xF3
};
static const uint8_t inventoryOfAnchor[] = {
    0x48,0x83,0xEC,0x28, 0x48,0x8B,0x89,0xF8,0x2F,0x00,0x00,
    0x33,0xC0, 0x48,0x85,0xC9, 0x74,0x09, 0x48,0x8B,0x01,
    0xFF,0x90,0xD0,0x01,0x00,0x00, 0x48,0x83,0xC4,0x28, 0xC3
};

typedef void* (*InventoryOfFn)(void* player);
static InventoryOfFn global_inventoryOf = nullptr;
static uint64_t      global_playerHandle = 0;   /* the game's own variable */

/* 1 where the player's inventory can be reached and its layout stood in the
   game's code as expected. */
static int findPlayerInventory() {
    if (!global_handlesKnown) return 0;
    const uint64_t findEntry = onlyPlaceOf(findEntryAnchor,
                                           sizeof(findEntryAnchor));
    const uint64_t entryLimit = onlyPlaceOf(entryLimitAnchor,
                                            sizeof(entryLimitAnchor));
    const uint64_t resolve = onlyPlaceOf(outerResolveAnchor,
                                         sizeof(outerResolveAnchor));
    if (!findEntry || !entryLimit || !resolve) {
        writeLog("INVENTORY the game's %s not found where expected, so a "
                 "changed carry limit reaches the items the player holds only "
                 "when a save is loaded",
                 !findEntry ? "search of the inventory was"
                 : !entryLimit ? "reading of an entry's carry limit was"
                               : "handle resolve was");
        return 0;
    }

    uint64_t handle = 0, inventoryOf = 0;
    int sites = 0, disagree = 0;
    if (global_textSize < 20) return 0;
    GUARD_TRY {
        const uint8_t* text = (const uint8_t*)global_textBase;
        const uint64_t last = global_textSize - 20;
        for (uint64_t at = 0; at <= last; ++at) {
            if (text[at] != 0x48 || text[at + 1] != 0x8D || text[at + 2] != 0x0D
                || text[at + 7] != 0xE8 || text[at + 12] != 0x48
                || text[at + 13] != 0x8B || text[at + 14] != 0xC8
                || text[at + 15] != 0xE8) continue;
            int32_t d1 = 0, d2 = 0, d3 = 0;
            memcpy(&d1, text + at + 3, 4);
            memcpy(&d2, text + at + 8, 4);
            memcpy(&d3, text + at + 16, 4);
            const uint64_t here = global_textBase + at;
            if (here + 12 + (int64_t)d2 != resolve) continue;
            const uint64_t called = here + 20 + (int64_t)d3;
            if (called < global_textBase
                || called + sizeof(inventoryOfAnchor)
                       > global_textBase + global_textSize
                || memcmp((const void*)called, inventoryOfAnchor,
                          sizeof(inventoryOfAnchor)) != 0) continue;
            const uint64_t variable = here + 7 + (int64_t)d1;
            if (!sites) { handle = variable; inventoryOf = called; }
            else if (variable != handle) ++disagree;
            ++sites;
        }
    } GUARD_EXCEPT { sites = 0; }

    if (!sites || disagree) {
        writeLog("INVENTORY %s, so a changed carry limit reaches the items "
                 "the player holds only when a save is loaded",
                 !sites ? "no place in the game's code reads the player's "
                          "handle the way expected"
                        : "the places that read the player's handle do not "
                          "agree on where it is");
        return 0;
    }
    global_playerHandle = handle;
    global_inventoryOf = (InventoryOfFn)inventoryOf;
    return 1;
}

/* ------------------------------------------------------ the count shown */

/*
    The HUD's current item shows how many the player holds.
    Hud2::HealthAndAmmo's update reads the count and hands it to
    Hud2::HealthAndAmmo::SetItemCount, which hides the number when it is zero;
    for the bottle, the brick and the gas can the update hands it zero instead.
    The hook stands on the instruction after that choice:

        45 33 D2                xor r10d, r10d        zero, for those three
        44 8A 4C 24 61          mov r9b, [rsp + 0x61] <- COUNT_SHOWN_HOOK
        45 8A C5                mov r8b, r13b
        41 8B D2                mov edx, r10d         the count shown
        48 8B CD                mov rcx, rbp
        E8 <offset>             call SetItemCount

    where rbx holds the item's id, r15 the player's inventory and r10 the
    count. For an item in the table other than a crafting material, the count
    shown is how many are held where the item's carry limit is above one, and
    zero, which hides it, where the limit is one.
*/
static const uint8_t countShownAnchor[] = {
    0x45,0x33,0xD2, 0x44,0x8A,0x4C,0x24,0x61, 0x45,0x8A,0xC5,
    0x41,0x8B,0xD2, 0x48,0x8B,0xCD, 0xE8
};
#define COUNT_SHOWN_HOOK  3
#define ENTRY_HELD        0x10

static const char* const setItemCountSignature =
    "void __cdecl Hud2::HealthAndAmmo::SetItemCount(int,bool,bool)";

/* The game's reading of an entry's carry limit, with what it adds to it. */
typedef int (*EntryLimitFn)(void* inventory, void* entry);
static EntryLimitFn global_entryLimit = nullptr;

static void countShown(void* registers, void* user) {
    (void)user;
    const int at = itemOfId(loaderHookRegister(registers, LOADER_REG_RBX));
    if (at < 0 || global_item[at].quartered || !global_entryLimit) return;
    const uint64_t item = global_setting[at].id;
    const uint64_t inventory = loaderHookRegister(registers, LOADER_REG_R15);
    if (inventory < SMALLEST_POINTER || inventory > LARGEST_POINTER) return;
    GUARD_TRY {
        const uint32_t count = *(volatile uint32_t*)(inventory + INVENTORY_COUNT);
        const uint64_t list = *(volatile uint64_t*)(inventory + INVENTORY_LIST);
        if (count > MAX_ENTRIES || list < SMALLEST_POINTER
            || list > LARGEST_POINTER) return;
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t entry = list + (uint64_t)i * ENTRY_SIZE;
            if (*(volatile uint64_t*)(entry + ENTRY_ITEM) != item) continue;
            const int limit = global_entryLimit((void*)inventory, (void*)entry);
            const uint32_t held = *(volatile uint32_t*)(entry + ENTRY_HELD);
            loaderHookSetRegister(registers, LOADER_REG_R10,
                                  limit > 1 ? (uint64_t)held : 0);
            return;
        }
    } GUARD_EXCEPT { }
}

static void showCountsByLimit() {
    const uint64_t anchor = onlyPlaceOf(countShownAnchor,
                                        sizeof(countShownAnchor));
    const uint64_t entryLimit = onlyPlaceOf(entryLimitAnchor,
                                            sizeof(entryLimitAnchor));
    const uint64_t setItemCount = loaderFindFunction(
        "Hud2::HealthAndAmmo::SetItemCount", setItemCountSignature);
    if (!anchor || !entryLimit || !setItemCount) {
        writeLog("COUNT the %s not found where expected, so the HUD shows "
                 "counts the way the game decides",
                 !anchor ? "place the HUD chooses the count shown was"
                 : !entryLimit ? "reading of an entry's carry limit was"
                               : "HUD's SetItemCount was");
        return;
    }
    int32_t offset = 0;
    memcpy(&offset, (const void*)(anchor + sizeof(countShownAnchor)), 4);
    const uint64_t called = anchor + sizeof(countShownAnchor) + 4
                          + (int64_t)offset;
    if (called != setItemCount) {
        writeLog("COUNT the call at rva %08X is not the HUD's SetItemCount, so "
                 "the HUD shows counts the way the game decides",
                 (unsigned int)(anchor - global_moduleBase));
        return;
    }
    global_entryLimit = (EntryLimitFn)entryLimit;
    if (!loaderHookAddress("hud_item_count", anchor + COUNT_SHOWN_HOOK,
                           countShown, nullptr))
        writeLog("COUNT the count the HUD shows could not be hooked");
}

/*
    Every entry in the player's inventory set to the carry limit the settings
    now ask for. Game thread only. Nothing is done where there is no player.
*/
static void writeHeldLimits() {
    if (!global_inventoryOf || !global_playerHandle) return;
    uint64_t record = 0, idWord = 0;
    if (!readQword(global_playerHandle, &record)
        || !readQword(global_playerHandle + 8, &idWord)) return;
    const uint64_t player = processOf(record, (uint32_t)idWord);
    if (!player) return;

    uint64_t inventory = 0;
    GUARD_TRY { inventory = (uint64_t)global_inventoryOf((void*)player); }
    GUARD_EXCEPT { inventory = 0; }
    if (inventory < SMALLEST_POINTER || inventory > LARGEST_POINTER) return;

    uint32_t count = 0;
    uint64_t list = 0;
    GUARD_TRY {
        count = *(volatile uint32_t*)(inventory + INVENTORY_COUNT);
        list = *(volatile uint64_t*)(inventory + INVENTORY_LIST);
    } GUARD_EXCEPT { count = 0; list = 0; }
    if (!count || count > MAX_ENTRIES
        || list < SMALLEST_POINTER || list > LARGEST_POINTER) return;

    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t entry = list + (uint64_t)i * ENTRY_SIZE;
        GUARD_TRY {
            const uint64_t id = *(volatile uint64_t*)(entry + ENTRY_ITEM);
            const int at = itemOfId(id);
            if (at >= 0) {
                const int whole = global_setting[at].limit;
                const uint32_t give = global_item[at].quartered
                                    ? (uint32_t)(whole * 4) : (uint32_t)whole;
                volatile uint32_t* where =
                    (volatile uint32_t*)(entry + ENTRY_LIMIT);
                if (*where != give) *where = give;
            }
        } GUARD_EXCEPT {
            writeLog("LIMIT entry %u of the player's inventory could not be "
                     "written", i);
            break;
        }
    }
}

/* 1 once the limits have been written from the settings as they stand.
   Clearing it has them written again. */
static int global_limitsDone = 0;

/* 1 once the map has been read and every limit that was asked for is written. */
static int setCarryLimits() {
    if (global_limitsDone) return 1;
    if (!global_lookupInModule) return 0;

    uint64_t fromGame = 0;
    const uint64_t variable = gameItemMapVariable();
    if (variable) readQword(variable, &fromGame);

    const void* value = nullptr;
    GUARD_TRY {
        value = global_lookupInModule(ITEM_MAP_SYMBOL,
                                      idOfName(ITEM_MAP_MODULE));
    } GUARD_EXCEPT { value = nullptr; }
    uint64_t fromSymbol = 0;
    if (value) readQword((uint64_t)value, &fromSymbol);

    const uint64_t data = fromGame ? fromGame : fromSymbol;
    if (data < SMALLEST_POINTER) return 0;

    uint64_t count = 0, ids = 0, records = 0;
    if (!readItemMap(data, &count, &ids, &records)) return 0;

    for (uint64_t i = 0; i < count; ++i) {
        uint64_t key = 0, record = 0;
        if (!readQword(ids + i * 8, &key)) break;
        if (!readQword(records + i * 8, &record)) break;
        const int at = itemOfId(key);
        if (at < 0) continue;

        const int whole = global_setting[at].limit;
        const uint32_t give = global_item[at].quartered
                            ? (uint32_t)(whole * 4) : (uint32_t)whole;
        GUARD_TRY {
            uint32_t* where = (uint32_t*)(record + ITEM_CARRY_LIMIT);
            DWORD was = 0;
            if (VirtualProtect(where, 4, PAGE_READWRITE, &was)) {
                if (*where != give) *where = give;
                VirtualProtect(where, 4, was, &was);
            }
        } GUARD_EXCEPT {
            writeLog("LIMIT %s could not be written", global_item[at].plain);
        }
    }
    writeHeldLimits();
    if (data == fromGame && fromGame) global_wroteIntoGameMap = 1;
    global_limitsDone = 1;
    return 1;
}

/* ------------------------------------------------------------- installing */

static volatile LONG global_installed = 0;

/* The pickups that could not be followed, written when their count moves,
   and a full table, written once. Not a game thread. */
static DWORD WINAPI reporting(LPVOID) {
    LONG noHandleReported = 0;
    int fullReported = 0;
    for (;;) {
        Sleep(1000);
        if (global_noHandle != noHandleReported) {
            noHandleReported = global_noHandle;
            writeLog("PICKUP %ld pickup(s) had no handle when set up, so a "
                     "change to the multipliers reaches them only when the "
                     "level is loaded again", noHandleReported);
        }
        if (global_liveFull && !fullReported) {
            fullReported = 1;
            writeLog("PICKUP there is room to remember %d standing pickup(s) "
                     "and the level has more, so the ones past that keep what "
                     "they were given until the level is loaded again",
                     MAX_LIVE_PICKUPS);
        }
    }
}

/* The game functions called here read fiber-local storage and can only be
   called from a game thread. */
static void carryLimitsOnGameThread(void* registers, void* user) {
    (void)registers; (void)user;
    if (settingsRereadIfChanged()) {
        global_limitsDone = 0;
        refreshLivePickups();
    }
    if (!global_wroteIntoGameMap
        && (DWORD)(GetTickCount() - global_lastMapTry) >= 250u) {
        global_lastMapTry = GetTickCount();
        global_limitsDone = 0;
    }
    setCarryLimits();
}

static DWORD WINAPI install(LPVOID) {
    if (InterlockedCompareExchange(&global_installed, 1, 0) != 0) return 0;

    /* Must be taken before the settings are read. */
    iniWrittenAt(&global_iniReadAt);
    readSettings();

    if (!findTheCode()) {
        writeLog("the game's code could not be found; nothing is installed");
        return 0;
    }

    findHandleLayout();
    findPlayerInventory();
    showCountsByLimit();

    {
        const uint64_t site = onlyPlaceOf(spawnAmountAnchor,
                                          sizeof(spawnAmountAnchor));
        if (!site) {
            writeLog("the place a pickup's amount is settled was not found; "
                     "amounts are left alone");
        } else {
            findAmountOffsets();
            if (!loaderHookAddress("pickup_spawn_amount", site,
                                   pickupSpawnAmount, nullptr))
                writeLog("the amount could not be hooked");
        }
    }

    {
        static const char* const lookupSignature =
            "const struct ScriptValue *__cdecl ScriptManager::LookupInModule"
            "(class StringId64,class StringId64)";
        const uint64_t found = loaderFindFunction(
            "ScriptManager::LookupInModule", lookupSignature);
        if (found)
            global_lookupInModule = (LookupInModuleFn)found;
        else
            writeLog("the symbol lookup was not found; carry limits are left "
                     "alone");
    }

    {
        static const char* const listenerSignature =
            "void __cdecl ScriptManager::ListenerUpdate(void)";
        if (!loaderHookFunction("ScriptManager::ListenerUpdate",
                                listenerSignature, carryLimitsOnGameThread,
                                nullptr))
            writeLog("the script update could not be hooked, so the carry "
                     "limits are left as the game has them");
    }

    CloseHandle(CreateThread(nullptr, 0, reporting, nullptr, 0, nullptr));
    return 0;
}

/* ------------------------------------------------------------------ plugin */

/*
    What this plugin is holding for one pickup it remembers, for anything that
    wants to ask.

    at    which pickup, counted from zero, up to what PickupsRemembered
          answers. An entry is used again once its pickup has left the level.
    out   receives one line of plain text: which item, the heap record and id
          the pickup is followed by, where that puts it now, what it was born
          holding, what this plugin made of it, what it holds now, and how
          much was taken from it before the last change to the settings. A
          pickup no longer in the level says so.
    room  how much room out has. A line too long for it is cut.

    Answers 1 where there is such an entry, 0 where there is not.
*/
extern "C" TLOU_PLUGIN_EXPORT int DescribePickup(int at, char* out,
                                                 unsigned room) {
    if (!out || !room) return 0;
    out[0] = 0;
    if (at < 0 || at >= (int)global_liveCount) return 0;
    const LivePickup* one = &global_live[at];
    const uint64_t record = one->record;

    char line[512];
    if (!record) {
        sprintf_s(line, "%d: nothing remembered here", at);
    } else {
        const uint64_t object = pickupNow(one);
        int holds = 0, readable = 0;
        if (object) {
            GUARD_TRY {
                holds = *(volatile int*)(object + global_amountAt);
                readable = 1;
            } GUARD_EXCEPT { readable = 0; }
        }
        if (!object)
            sprintf_s(line, "%d: %s, heap record %016llX id %u, is no longer "
                            "in the level; born holding %d, made %d", at,
                      plainNameOfId(one->item), (unsigned long long)record,
                      one->id, one->vanilla, one->made);
        else if (!readable)
            sprintf_s(line, "%d: %s, heap record %016llX id %u, is at %016llX "
                            "and cannot be read", at,
                      plainNameOfId(one->item), (unsigned long long)record,
                      one->id, (unsigned long long)object);
        else
            sprintf_s(line, "%d: %s, heap record %016llX id %u, is at %016llX; "
                            "born holding %d, made %d, holds %d, %d taken "
                            "before the last change to the settings", at,
                      plainNameOfId(one->item), (unsigned long long)record,
                      one->id, (unsigned long long)object, one->vanilla,
                      one->made, holds, one->taken);
    }
    unsigned n = 0;
    while (line[n] && n + 1 < room) { out[n] = line[n]; ++n; }
    out[n] = 0;
    return 1;
}

/* How many entries there are to ask about. */
extern "C" TLOU_PLUGIN_EXPORT int PickupsRemembered(void) {
    return (int)global_liveCount;
}

/* Counts up every time a pickup is set up or the standing ones are set
   again. */
extern "C" TLOU_PLUGIN_EXPORT unsigned PickupChanges(void) {
    return (unsigned)global_pickupsSeen + (unsigned)global_liveRefreshes;
}

/* How many pickups could not be followed from their setup. */
extern "C" TLOU_PLUGIN_EXPORT unsigned PickupsWithoutHandle(void) {
    return (unsigned)global_noHandle;
}

extern "C" TLOU_PLUGIN_EXPORT const char* PluginName(void)
    { return "Inventory and Pickups Modifier"; }
extern "C" TLOU_PLUGIN_EXPORT const char* PluginVersion(void) { return "v1.0.1"; }
extern "C" TLOU_PLUGIN_EXPORT const char* PluginAuthor(void)  { return "mwith77"; }

extern "C" TLOU_PLUGIN_EXPORT int SupportsGame(LoaderHandle) { return 1; }

static volatile LONG global_initCalled = 0;

static DWORD WINAPI standalone(LPVOID) {
    Sleep(2000);
    if (!global_initCalled)
        writeLog("no loader present; nothing can be hooked");
    return 0;
}

static void settingsPathFrom(const char* modFolder) {
    formatInto(global_iniPath, sizeof(global_iniPath), "%s\\%s.ini", modFolder,
               modBaseName);
}

extern "C" TLOU_PLUGIN_EXPORT int Init(LoaderHandle loader) {
    InterlockedExchange(&global_initCalled, 1);
    loaderBind(loader);
    /* Must come before anything else after loaderBind. */
    loaderWatchBadArguments(PluginName());

    const char* folder = loaderModFolder();
    const char* logs   = loaderLogsFolder();
    if (folder && *folder) settingsPathFrom(folder);
    if (logs && *logs) {
        formatInto(global_logPath, sizeof(global_logPath), "%s\\%s.log", logs,
                   modBaseName);
        archiveLog();
    }

    CloseHandle(CreateThread(nullptr, 0, install, nullptr, 0, nullptr));
    return 1;
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);

    char folder[MAX_PATH], logs[MAX_PATH];
    const DWORD length = GetModuleFileNameA(self, folder, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return TRUE;
    char* slash = strrchr(folder, '\\');
    if (slash) *slash = 0;
    settingsPathFrom(folder);

    if (formatInto(logs, sizeof(logs), "%s\\Logs", folder)) {
        CreateDirectoryA(logs, nullptr);
        formatInto(global_logPath, sizeof(global_logPath), "%s\\%s.log", logs,
                   modBaseName);
        archiveLog();
    }

    CloseHandle(CreateThread(nullptr, 0, standalone, nullptr, 0, nullptr));
    return TRUE;
}
