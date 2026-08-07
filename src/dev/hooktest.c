/**
 * @file hooktest.c
 * @brief See hooktest.h for what is being proven and why a megabyte of .bss is the test fixture.
 * @ingroup dev
 */

#include <libdragon.h>
#include <string.h>

#include "boot/cheats.h"
#include "boot/vr4300_asm.h"
#include "dev/hooktest.h"

/* Same three as enginetest.c: libdragon has macros for none of WatchLo's spellings but the C
 * one, and the fallback scenario arms a real watch on a live console that must be disarmed
 * before interrupts come back. */
#define READ_WATCHLO()   ({ uint32_t v_; asm volatile("mfc0 %0,$18" : "=r"(v_) :: "memory"); v_; })
#define WRITE_WATCHLO(x) asm volatile("mtc0 %0,$18" :: "r"(x) : "memory")
#define WRITE_WATCHHI(x) asm volatile("mtc0 %0,$19" :: "r"(x) : "memory")

/**
 * @brief The synthetic game image: a full megabyte, because the emitted scan covers a full
 *        megabyte from $t1 and every byte of the window must be memory this test controls.
 *
 * Word 0 is the fake game entry (the patcher ends `jr $t1`), word 4 the fake __osException,
 * word 8 the planted preamble. Everything else is .bss zero, which the scan reads and rejects
 * 262,000-odd times in the fallback scenario -- that full-length miss is as much the test as
 * the match is.
 */
static uint32_t arena[0x40000] __attribute__((aligned(16)));

/** Where the fake __osException records that control really arrived. It stores $k0, which at
 *  that point must hold the fake's own address -- delivered there by the engine tail replaying
 *  the two original preamble words. A flag that merely went non-zero would also pass if the
 *  engine jumped somewhere that happened to write memory; the exact value cannot. */
static volatile uint32_t osexc_hit;

static volatile uint8_t  target_byte;
static volatile uint16_t target_half;
static volatile uint8_t  target_cond;

static int checks;
static int failures;

static void check (bool ok, const char *what) {
    checks++;
    if (!ok) {
        failures++;
        debugf("HOOKTEST FAIL: %s\n", what);
    }
}

/**
 * @brief Plant the image: entry stub, fake __osException, and a preamble whose third word the
 *        caller chooses -- I_JR(REG_K0) makes it real, anything else makes it a near-miss the
 *        scan must reject.
 */
static void plant_at (uint32_t third_word, uint32_t osexc) {

    arena[0]  = I_JR(REG_RA);
    arena[1]  = I_NOP();

    arena[4]  = I_LUI(REG_K1, A_BASE((uint32_t)(&osexc_hit)));
    arena[5]  = I_SW(REG_K0, A_OFFSET((uint32_t)(&osexc_hit)), REG_K1);
    arena[6]  = I_JR(REG_RA);
    arena[7]  = I_NOP();

    arena[8]  = I_LUI(REG_K0, A_BASE(osexc));
    arena[9]  = I_ADDIU(REG_K0, REG_K0, A_OFFSET(osexc));
    arena[10] = third_word;
    arena[11] = I_NOP();

    data_cache_hit_writeback_invalidate(arena, 64);
    inst_cache_hit_invalidate(arena, 64);
}

/** @brief The ordinary fixture: a preamble whose target is the fake __osException next door. */
static void plant (uint32_t third_word) {
    plant_at(third_word, (uint32_t)(&arena[4]));
}

/**
 * @brief Jump into the emitted patcher exactly the way the hooked IPL3 would: $t1 carrying the
 *        game entry point. The patcher hands control to that entry when it finishes, and the
 *        planted entry is `jr $ra`, so the whole boot handoff round-trips back into C.
 *
 * The clobbers are the patcher's real register usage ($t3-$t6 plus $k0/$k1; the k-registers are
 * not listed because gcc never allocates them). Interrupts must be off across this: libdragon's
 * handler scribbles $k0/$k1 without saving them, and the patcher holds live state there.
 */
static void run_patcher (uint32_t *patcher, void *game_entry) {
    register uint32_t t1 asm ("t1") = (uint32_t)(game_entry);
    asm volatile (
        ".set noreorder\n"
        "jalr %1\n"
        "nop\n"
        ".set reorder\n"
        : "+r" (t1)
        : "r" (patcher)
        : "$11", "$12", "$13", "$14", "$31", "memory"
    );
}

/** @brief Execute four instructions as an exception vector would: jump at them, come back via
 *         whatever they chain to. Position-independent by construction, so calling the patched
 *         preamble in place is exactly what running its copy at 0x80000180 would be. */
static void run_vector (void *vector) {
    asm volatile (
        ".set noreorder\n"
        "jalr %0\n"
        "nop\n"
        ".set reorder\n"
        :
        : "r" (vector)
        : "$31", "memory"
    );
}

/**
 * @brief Pin the four words tools/preamblescan.py carries as literals to what this build emits.
 *
 * The tool exists because the console answers one ROM per launch and only after the menu is gone;
 * it scans a whole shelf on a PC in seconds. It cannot include vr4300_asm.h, so the pattern is
 * written down twice, and the failure mode of a drift is silent and wrong in both directions: a
 * match reported for a ROM the patcher will not hook, or a clean bill for a ROM it will corrupt.
 *
 * Here rather than in tools/hosttest because vr4300_asm.h assembles through a bitfield union, and
 * bitfield allocation order follows the target's endianness -- on a little-endian host
 * I_JR(REG_K0) is 0x20000680, not 0x03400008. The macros only mean anything compiled for MIPS.
 * A _Static_assert cannot do it either: a compound literal is not a constant expression.
 */
static void check_preamble_pattern (void) {
    check((I_LUI(REG_K0, 0) >> 16) == 0x3C1A, "preamblescan.py's lui $k0 word still matches");
    check((I_ADDIU(REG_K0, REG_K0, 0) >> 16) == 0x275A,
          "preamblescan.py's addiu $k0 word still matches");
    check(I_JR(REG_K0) == 0x03400008, "preamblescan.py's jr $k0 word still matches");
    check(I_NOP() == 0x00000000, "preamblescan.py's nop word still matches");
    /* The address check the scan gained after the tool found two ROMs matching data whose target
     * is not RDRAM. Conker's %hi is 0x1000 and GoldenEye's is 0x7001; neither may pass. */
    check((I_LUI(REG_K0, 0x8000) & 0xFF00) == 0x8000, "a KSEG0 %hi passes the address check");
    check((I_LUI(REG_K0, 0x1000) & 0xFF00) != 0x8000, "Conker's %hi does not");
    check((I_LUI(REG_K0, 0x7001) & 0xFF00) != 0x8000, "GoldenEye's %hi does not");
}

void hooktest_run (void) {
    check_preamble_pattern();

    volatile uint32_t *vec = (volatile uint32_t *)(0x80000180);
    uint32_t vec0 = vec[0];
    uint32_t vec1 = vec[1];
    uint32_t watch_before = READ_WATCHLO();

    uint32_t list[10];
    uint32_t a_byte = (uint32_t)(&target_byte);
    uint32_t a_half = (uint32_t)(&target_half);
    uint32_t a_cond = (uint32_t)(&target_cond);

    list[0] = 0x80000000u | (a_byte & 0xFFFFFF); // write 0x5A, 8-bit
    list[1] = 0x5A;
    list[2] = 0x81000000u | (a_half & 0xFFFFFF); // write 0xBEEF, 16-bit
    list[3] = 0xBEEF;
    list[4] = 0xD0000000u | (a_byte & 0xFFFFFF); // if the byte above landed...
    list[5] = 0x5A;
    list[6] = 0x80000000u | (a_cond & 0xFFFFFF); // ...write 0x77
    list[7] = 0x77;
    list[8] = 0;
    list[9] = 0;

    /* Scenario 1: a well-formed preamble at arena[8]. */
    plant(I_JR(REG_K0));
    uint32_t planted_w0 = arena[8];

    target_byte = 0;
    target_half = 0;
    target_cond = 0;
    osexc_hit = 0;

    uint32_t *patcher = cheats_emit(list);
    check(patcher != NULL, "emit (found scenario)");

    if (patcher != NULL) {
        disable_interrupts();
        run_patcher(patcher, arena);

        /* Capture, then restore unconditionally, BEFORE executing anything or letting
         * interrupts back in. On the found path this restores what was never touched and costs
         * nothing. But if the scan wrongly misses, the patcher has just aimed libdragon's live
         * exception vector at an engine whose tail leads to garbage -- and the first version of
         * this test found that out as a hang instead of a red check, which is exactly the
         * mutation run this comment is quoting. A test whose failure mode is "the console
         * wedges" reports nothing. */
        uint32_t got0 = vec[0];
        uint32_t got1 = vec[1];
        uint32_t armed = READ_WATCHLO();
        WRITE_WATCHLO(0);
        WRITE_WATCHHI(0);
        vec[0] = vec0;
        vec[1] = vec1;
        data_cache_hit_writeback_invalidate((void *)(vec), 8);
        inst_cache_hit_invalidate((void *)(vec), 8);

        run_vector(&arena[8]);
        enable_interrupts();

        check(arena[8] != planted_w0, "preamble was rewritten");
        check(target_byte == 0x5A, "8-bit write landed");
        check(target_half == 0xBEEF, "16-bit write landed");
        check(target_cond == 0x77, "conditional write landed");
        check(osexc_hit == (uint32_t)(&arena[4]), "control reached __osException with $k0 exact");
        check(got0 == vec0 && got1 == vec1, "0x80000180 untouched on the found path");
        check(armed == watch_before, "watch not armed on the found path");
    }

    /* Scenario 2: one word wrong -- `jr $k1` where the preamble says `jr $k0`. The scan must
     * reject it 262,144 times and fall back to the Datel hook. Timed, because a full-window
     * miss is the worst case every non-libultra game will pay at boot. */
    plant(I_JR(REG_K1));

    target_byte = 0;
    target_half = 0;
    target_cond = 0;

    patcher = cheats_emit(list);
    check(patcher != NULL, "emit (fallback scenario)");

    if (patcher != NULL) {
        disable_interrupts();
        uint32_t t0 = TICKS_READ();
        run_patcher(patcher, arena);
        uint32_t scan_ticks = TICKS_READ() - t0;

        uint32_t armed = READ_WATCHLO();
        uint32_t got0 = vec[0];
        uint32_t got1 = vec[1];

        /* Undo before interrupts return: the vector is libdragon's and the watch is live. The
         * watch goes first -- with it armed, the restoring stores to 0x180 are themselves the
         * trigger on any CPU that delivers the trap. */
        WRITE_WATCHLO(0);
        WRITE_WATCHHI(0);
        vec[0] = vec0;
        vec[1] = vec1;
        data_cache_hit_writeback_invalidate((void *)(vec), 8);
        inst_cache_hit_invalidate((void *)(vec), 8);
        enable_interrupts();

        check((got0 >> 26) == OP_J, "fallback wrote j-engine over 0x80000180");
        check(got1 == 0, "fallback wrote the delay-slot nop");
        check(armed == ((0x80000180 | 1) & 0xFFFF), "fallback armed the watch");
        check(arena[8] == planted_w0, "near-miss preamble left alone");
        check(target_byte == 0 && target_half == 0 && target_cond == 0,
              "engine did not run in the fallback scenario");

        debugf("HOOKTEST fallback full-window scan: %lu us\n",
               (unsigned long)(TIMER_MICROS(scan_ticks)));
    }

    /* Scenario 3: the beacon. It is the instrument the next hardware run depends on, so it gets
     * the same treatment as the mechanism it measures -- executed, and checked against exact
     * bytes rather than against "something happened".
     *
     * VI_ORIGIN is pointed at the arena for the duration instead of at the real framebuffer, so
     * the engine paints into memory this test owns and the check can be exact. The VI scans out
     * the arena for the frame or two this takes; that is a dev build drawing garbage briefly, and
     * it is worth it for a check that cannot pass by accident. */
    {
        volatile uint32_t *vi_origin = (volatile uint32_t *)VI_ORIGIN_ADDRESS;
        uint32_t origin_before = *vi_origin;

        /* The pretend framebuffer starts half a megabyte into the arena, not at the top of it.
         * The fake game entry, the fake __osException and the planted preamble live in arena[0..8],
         * and a beacon aimed at offset zero would paint straight over them -- which is what the
         * first attempt at this did: the mutation that put the offset back to 0 destroyed the
         * fixture and hung the console instead of turning the overscan check red. A test whose
         * failure mode is a wedge reports nothing, so the two regions are now disjoint and the
         * mutation reports. */
        uint32_t arena_phys = ((uint32_t)(arena) & 0x1FFFFFFF) + 0x80000;

        /* The engine refuses to paint below BEACON_MIN_ORIGIN_SHIFT, on the reasoning that
         * VI_ORIGIN that low is VI_ORIGIN unset. If .bss ever moves under that line the beacon
         * silently does nothing and this scenario would report it as a failure to paint -- so the
         * precondition is a check of its own, and it names itself. It has already earned its
         * keep: the floor was a megabyte and this is what said so. */
        check((arena_phys >> BEACON_MIN_ORIGIN_SHIFT) != 0,
              "the arena is above the beacon's origin floor");

        /* Clear the band the beacon aims at, plus a word past its end, so "it painted" and "it
         * stopped where it should" are both claims about memory this test zeroed. arena[8] holds
         * the planted preamble and BEACON_OFFSET_BYTES is 64,000 in, so the two never overlap. */
        memset((uint8_t *)arena + 0x80000, 0, BEACON_OFFSET_BYTES + BEACON_WORDS * 4 + 64);
        plant(I_JR(REG_K0));

        cheats_set_beacon(true);
        patcher = cheats_emit(list);
        cheats_set_beacon(false);
        check(patcher != NULL, "emit (beacon scenario)");

        if (patcher != NULL && (arena_phys >> BEACON_MIN_ORIGIN_SHIFT) != 0) {
            disable_interrupts();
            run_patcher(patcher, arena);

            uint32_t got0 = vec[0], got1 = vec[1];
            uint32_t armed = READ_WATCHLO();
            WRITE_WATCHLO(0);
            WRITE_WATCHHI(0);
            vec[0] = vec0;
            vec[1] = vec1;
            data_cache_hit_writeback_invalidate((void *)(vec), 8);
            inst_cache_hit_invalidate((void *)(vec), 8);

            *vi_origin = arena_phys;
            run_vector(&arena[8]);
            *vi_origin = origin_before;
            enable_interrupts();

            /* Read uncached: the engine wrote through KSEG1 and this core's data cache has no
             * reason to know. Reading it cached would compare against whatever was there before
             * and fail for the wrong reason. */
            volatile uint32_t *painted =
                (volatile uint32_t *)(0xA0000000u | (arena_phys + BEACON_OFFSET_BYTES));
            bool all_green = true;
            for (int i = 0; i < BEACON_WORDS; i++) {
                if (painted[i] != BEACON_GREEN) {
                    all_green = false;
                }
            }
            check(all_green, "the beacon painted every word of its bar");
            check(painted[BEACON_WORDS] == 0, "and stopped at the end of it");
            /* The offset is the whole reason the first hardware run was unreadable, so it is
             * checked rather than assumed: nothing may land at the top of the buffer. */
            check(*(volatile uint32_t *)(0xA0000000u | arena_phys) != BEACON_GREEN,
                  "and nothing landed at the top of the buffer, where overscan hides it");
            check(got0 == vec0 && got1 == vec1, "the beacon did not disturb 0x80000180");
            check(armed == watch_before, "the beacon did not arm the watch");
            check(osexc_hit == (uint32_t)(&arena[4]),
                  "control still reached __osException with the beacon in the way");
        }
    }

    /* Scenario 4: a preamble-shaped run whose target is not an address.
     *
     * Not hypothetical. tools/preamblescan.py ran this exact pattern over the 24 N64 ROMs on the
     * reference card, and Conker's Bad Fur Day and GoldenEye 007 both match a run of data whose
     * reconstructed target is 0x100071e0 and 0x700101a0 -- neither of which is RDRAM. The patcher
     * takes the FIRST match and rewrites two words of live game code at it, so on those two ROMs
     * it was about to corrupt something arbitrary. One in twelve.
     *
     * 0x10007000 is Conker's, near enough. The scan must walk straight past it and land on the
     * watch fallback, leaving the fake preamble exactly as planted. */
    plant_at(I_JR(REG_K0), 0x10007000);
    uint32_t bogus_w0 = arena[8];
    uint32_t bogus_w1 = arena[9];

    target_byte = 0;
    target_half = 0;
    target_cond = 0;

    patcher = cheats_emit(list);
    check(patcher != NULL, "emit (bogus-target scenario)");

    if (patcher != NULL) {
        disable_interrupts();
        run_patcher(patcher, arena);

        uint32_t armed = READ_WATCHLO();
        uint32_t got0 = vec[0];
        WRITE_WATCHLO(0);
        WRITE_WATCHHI(0);
        vec[0] = vec0;
        vec[1] = vec1;
        data_cache_hit_writeback_invalidate((void *)(vec), 8);
        inst_cache_hit_invalidate((void *)(vec), 8);
        enable_interrupts();

        check(arena[8] == bogus_w0 && arena[9] == bogus_w1,
              "a preamble whose target is not an address is left alone");
        check((got0 >> 26) == OP_J && armed == ((0x80000180 | 1) & 0xFFFF),
              "and the scan falls through to the watch instead of hooking it");
    }

    debugf("HOOKTEST %d/%d ok\n", checks - failures, checks);
}
