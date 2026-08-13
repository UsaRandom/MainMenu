/**
 * @file hooktest.c
 * @brief See hooktest.h for what is being proven and why a megabyte of .bss is the test fixture.
 * @ingroup dev
 */

#include <libdragon.h>
#include <malloc.h>
#include <string.h>

#include "boot/cheats.h"
#include "boot/vr4300_asm.h"
#include "dev/hooktest.h"
#include "menu/memprofile.h"

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

/** @brief Is every pixel of row @p y the colour @p c? Read uncached, because the flash wrote
 *         through KSEG1 and this core's data cache has no reason to know. */
static bool row_is (volatile uint16_t *px, int y, uint16_t c) {
    for (int x = 0; x < BEACON_FLASH_W; x++) {
        if (px[y * BEACON_FLASH_W + x] != c) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Read row @p y back as the number the blocks are supposed to spell, and check the gaps.
 *
 * Every pixel is accounted for: inside a block it must be uniformly white or black, between
 * blocks and past the last one it must still be @p bg, the verdict colour the fill left there. A block loop that overran its
 * pitch, or a fill that the blocks failed to cover, both show up here rather than as a picture
 * that looks roughly right.
 */
static bool band_row_ok (volatile uint16_t *px, int y, uint32_t expect, uint16_t bg) {
    volatile uint16_t *row = px + y * BEACON_FLASH_W;
    uint32_t got = 0;

    for (int x = 0; x < BEACON_FLASH_W; x++) {
        int i = (x - BEACON_FLASH_BIT_X) / BEACON_FLASH_BIT_PITCH;
        int within = (x - BEACON_FLASH_BIT_X) % BEACON_FLASH_BIT_PITCH;
        bool in_block = (x >= BEACON_FLASH_BIT_X) && (i < BEACON_FLASH_BITS)
                        && (within < BEACON_FLASH_BIT_W);
        if (!in_block) {
            if (row[x] != bg) {
                return false;
            }
            continue;
        }
        uint16_t want = (expect >> (BEACON_FLASH_BITS - 1 - i)) & 1 ? 0xFFFF : 0x0000;
        if (row[x] != want) {
            return false;
        }
        if (within == 0) {
            got = (got << 1) | (want ? 1 : 0);
        }
    }

    return got == (expect & ((1u << BEACON_FLASH_BITS) - 1));
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
    /* The Datel engine is emitted to CHEATS_DEFAULT_ENGINE_ADDRESS, 0x807C5C00 -- 7.77 MB, which
     * only exists on a console with an Expansion Pak. On 4 MB this wrote into nothing and the run
     * stopped dead after "cheats: engine placed at 807c5c00" with no frames dumped, which reads
     * exactly like a hung ROM. The product already refuses cheats without a pak (see
     * build_cheat_list in screen_launch.c); the harness has to refuse for the same reason. */
    if (mem_small()) {
        debugf("HOOKTEST skipped: no Expansion Pak, so the engine's address does not exist\n");
        return;
    }

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

    /* Scenario 5: the handoff flash.
     *
     * The instrument the next hardware run depends on, so it gets the same treatment as the
     * mechanism it measures: the emitted code is executed and its output compared byte for byte --
     * the fill, the twenty-four blocks, the rows either side of the band, and the one VI register
     * it writes.
     *
     * A whole 640x480 frame does not fit alongside the arena, so it is malloc'd rather than carved
     * out of .bss. The two holds are a millisecond each rather than ten seconds each; the delay
     * loop is checked by the clock, not by the wall.
     */
    {
        volatile uint32_t *vi_origin = (volatile uint32_t *)VI_ORIGIN_ADDRESS;
        uint32_t origin_before = *vi_origin;

        /* memalign, not malloc: the cache maintenance either side of this wants a 16-byte-aligned
         * address and libdragon asserts on anything else. malloc's 8 happened to land on 16 until
         * two new files moved the heap, and then the harness died inside an assert rather than
         * reporting -- a test that cannot run is worse than one that fails. */
        void *fb = memalign(16, BEACON_FLASH_BYTES);
        check(fb != NULL, "a frame for the flash scenario");

        if (fb != NULL) {
            uint32_t fb_phys = ((uint32_t)(fb) & 0x1FFFFFFF);
            volatile uint16_t *px = (volatile uint16_t *)(0xA0000000u | fb_phys);

            memset(fb, 0, BEACON_FLASH_BYTES);
            /* The emitted code invalidates before painting uncached, which is exactly what makes
             * this memset invisible to it unless the dirty lines go out first. Without this the
             * scenario tests the flash against whatever survived, which is not a fixture. */
            data_cache_hit_writeback_invalidate(fb, BEACON_FLASH_BYTES);
            plant(I_JR(REG_K0));

            /* The blocks show where the scan stopped, relative to the game entry. The planted
             * preamble is arena[8], so the answer is 32 -- known exactly, which is what makes the
             * band checkable rather than merely present. */
            uint32_t expect_bits = 8 * 4;

            /* A quarter second, not a millisecond. At 1 ms the hold check passed with the delay
             * loop mutated to never loop, because run_patcher also contains the 53 ms scan and
             * that alone cleared the bar -- a harness measuring the wrong thing and reporting
             * green. The hold has to dominate the scan for the number to mean anything. */
            cheats_set_flash(true, fb_phys, 46875000u / 4u);
            patcher = cheats_emit(list);
            cheats_set_flash(false, 0, 0);
            check(patcher != NULL, "emit (flash scenario)");

            if (patcher != NULL) {
                uint32_t t0 = TICKS_READ();
                disable_interrupts();
                run_patcher(patcher, arena);
                uint32_t got_origin = *vi_origin;
                *vi_origin = origin_before;
                enable_interrupts();
                uint32_t held = TIMER_MICROS(TICKS_DISTANCE(t0, TICKS_READ()));

                check(got_origin == fb_phys, "the flash pointed the VI at its own frame");
                /* The hold is the only channel that survives a display which never comes back, so
                 * it is checked at both ends. Below 200 ms the delay loop fell through; above
                 * 400 ms the hit path is paying the miss path's second hold, which would collapse
                 * the two readings the whole timing channel is built on. */
                check(held >= 200000, "and held for the ticks it was given");
                check(held < 400000, "and held them once, not twice");

                /* Row 0 proves the fill ran and that nothing reached above the band; the row one
                 * past the band proves the cascade copy stopped. Whole rows, because a band one
                 * row too tall is exactly the off-by-one a spot check misses. */
                check(row_is(px, 0, BEACON_GREEN & 0xFFFF),
                      "the flash filled the frame with the verdict");
                check(row_is(px, BEACON_FLASH_BIT_ROW - 1, BEACON_GREEN & 0xFFFF),
                      "and left the row above the band alone");
                check(row_is(px, BEACON_FLASH_BIT_ROW + BEACON_FLASH_BIT_H, BEACON_GREEN & 0xFFFF),
                      "and the row below it");

                /* First and last rows of the band: the first is what the block loop wrote, the
                 * last is what the cascade copy carried down. Checking only the first would pass
                 * with the copy loop deleted. */
                check(band_row_ok(px, BEACON_FLASH_BIT_ROW, expect_bits, BEACON_GREEN & 0xFFFF),
                      "the blocks spell out where the scan stopped");
                check(band_row_ok(px, BEACON_FLASH_BIT_ROW + BEACON_FLASH_BIT_H - 1, expect_bits,
                                  BEACON_GREEN & 0xFFFF),
                      "and the band carries them down every row");
            }

            /* Uncached writes left the cache holding whatever was there before. Freeing without
             * this hands the allocator lines that will write stale bytes back over the next
             * owner's data at some unrelated moment. */
            data_cache_hit_invalidate(fb, BEACON_FLASH_BYTES);
            free(fb);
        }
    }

    /* Scenario 6: the miss path, which is where the timing channel earns its keep.
     *
     * The hit path is worth ten seconds on hardware and the miss path twenty, and that difference
     * is the one reading that survives a display which never comes back. It is also the only part
     * of the flash no other scenario reaches, because scenario 4 runs the miss path with the flash
     * off. Bogus target, so the scan walks the whole megabyte and comes up empty: red fill, blocks
     * reading a full 1 MB, and twice the hold.
     */
    {
        volatile uint32_t *vi_origin = (volatile uint32_t *)VI_ORIGIN_ADDRESS;
        uint32_t origin_before = *vi_origin;

        /* memalign, not malloc: the cache maintenance either side of this wants a 16-byte-aligned
         * address and libdragon asserts on anything else. malloc's 8 happened to land on 16 until
         * two new files moved the heap, and then the harness died inside an assert rather than
         * reporting -- a test that cannot run is worse than one that fails. */
        void *fb = memalign(16, BEACON_FLASH_BYTES);
        check(fb != NULL, "a frame for the miss-path scenario");

        if (fb != NULL) {
            uint32_t fb_phys = ((uint32_t)(fb) & 0x1FFFFFFF);
            volatile uint16_t *px = (volatile uint16_t *)(0xA0000000u | fb_phys);

            memset(fb, 0, BEACON_FLASH_BYTES);
            data_cache_hit_writeback_invalidate(fb, BEACON_FLASH_BYTES);
            plant_at(I_JR(REG_K0), 0x10007000);

            cheats_set_flash(true, fb_phys, 46875000u / 4u);   /* so the miss owes half a second */
            patcher = cheats_emit(list);
            cheats_set_flash(false, 0, 0);

            if (patcher != NULL) {
                uint32_t t0 = TICKS_READ();
                disable_interrupts();
                run_patcher(patcher, arena);
                uint32_t armed = READ_WATCHLO();
                WRITE_WATCHLO(0);
                WRITE_WATCHHI(0);
                *vi_origin = origin_before;
                vec[0] = vec0;
                vec[1] = vec1;
                data_cache_hit_writeback_invalidate((void *)(vec), 8);
                inst_cache_hit_invalidate((void *)(vec), 8);
                enable_interrupts();
                uint32_t held = TIMER_MICROS(TICKS_DISTANCE(t0, TICKS_READ()));

                check(armed == ((0x80000180 | 1) & 0xFFFF), "the miss path still armed the watch");
                check(row_is(px, 0, BEACON_RED & 0xFFFF), "and painted red rather than green");
                /* 1 MB: the scan pointer walked the entire window. Reading it off the screen is
                 * how a hardware miss announces itself as a miss and not as a wild match. */
                check(band_row_ok(px, BEACON_FLASH_BIT_ROW, 0x100000, BEACON_RED & 0xFFFF),
                      "and the blocks read a full megabyte");
                /* Half a second against the hit path's quarter, with the 53 ms scan too small to
                 * confuse them. This is the timing channel itself under test: it is what tells a
                 * hit from a miss on a console whose display never comes back. */
                check(held >= 450000, "and waited out both holds");
            }

            data_cache_hit_invalidate(fb, BEACON_FLASH_BYTES);
            free(fb);
        }
    }

    debugf("HOOKTEST %d/%d ok\n", checks - failures, checks);

    /* No sound_poll() on the way out, and no sound_gap_forget() either -- both existed for the
     * old call site, a second PAST the first mixer feed, where this function's blocked second
     * starved the music audibly and then hid the gap from the boot audio line. app_init() now
     * runs it before anything has fed the DAC: the same second passes in a silence that cannot
     * be heard, the first feed stays where the font-load margin was measured (app.c), and the
     * BOOT audio worst gap number reports only what a shipped build would do. */
}
