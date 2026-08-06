/**
 * @file enginetest.c
 * @brief See enginetest.h for why the CPU is asked directly rather than through a cheat.
 * @ingroup menu
 */

#include <stdio.h>
#include <libdragon.h>

#include "library/cache.h"
#include "menu/enginetest.h"
#include "menu/paths.h"
#include "utils/fs.h"

/** The W bit of WatchLo: trap on stores. The same bit boot/cheats.c sets. */
#define WATCHLO_W       (1 << 0)

/** WatchLo holds bits 31..3 of a PHYSICAL address, so the target must be 8-byte aligned and the
 *  low three bits are the enable flags, not address. */
#define WATCHLO_PADDR   0xFFFFFFF8u

/** @brief WatchHi ($19). libdragon has no macro for this one, only for WatchLo. */
#define WRITE_WATCHHI(x) asm volatile("mtc0 %0,$19" :: "r"(x) : "memory")

/** @brief WatchLo, with a memory clobber, so the store under test cannot be moved across it. */
#define WRITE_WATCHLO(x) asm volatile("mtc0 %0,$18" :: "r"(x) : "memory")
#define READ_WATCHLO()   ({ uint32_t v; asm volatile("mfc0 %0,$18" : "=r"(v) :: "memory"); v; })

/** Left on the card while the test is in flight. See enginetest.h. */
#define BUSY_FILE   "watchtest.busy"

/**
 * @brief The address the watch is armed on, and the target of the store under test.
 *
 * Aligned to 8 because WatchLo compares bits 31..3: an unaligned target would arm the watch on the
 * doubleword containing it, which still works, but stating the alignment means the comparison
 * against the read-back value is exact rather than approximately right.
 *
 * `volatile` so the store is really emitted. It is a store to a variable nothing reads, which is
 * the first thing any optimiser removes.
 */
static volatile uint32_t watch_target __attribute__((aligned(8)));

static volatile bool watch_fired;
static volatile bool break_seen;
static exception_handler_t prev_handler;
static watchtest_t result;
static uint32_t readback;
static uint32_t armed;

/**
 * @brief Catch the watch exception, disarm, and return.
 *
 * Disarming comes first and nothing precedes it. Returning from here resumes at EPC, so the store
 * that trapped executes again -- and if the watch were still armed it would trap again, forever,
 * on a boot path. Everything else in this function is reporting.
 *
 * Any other exception is somebody else's: the previous handler gets it, or libdragon's default,
 * which is the crash screen. Swallowing an unrelated fault here would turn a diagnosable crash
 * into a hang.
 */
static void on_exception (exception_t *ex) {
    if (ex != NULL && ex->code == EXCEPTION_CODE_WATCH) {
        WRITE_WATCHLO(0);
        WRITE_WATCHHI(0);
        watch_fired = true;
        return;
    }
    /* The positive control. See probe_break(): stepping over the `break` is what proves this
     * function is reached at all, which is what makes "the watch did not fire" mean the CPU
     * rather than the harness. EPC points AT the faulting instruction for a synchronous
     * exception, so +4 resumes on the nop after it. */
    if (ex != NULL && ex->code == EXCEPTION_CODE_BREAKPOINT && ex->regs != NULL) {
        break_seen = true;
        ex->regs->epc += 4;
        return;
    }
    if (prev_handler != NULL) {
        prev_handler(ex);
        return;
    }
    exception_default_handler(ex);
}

void enginetest_run (const char *storage_prefix) {
    char busy[300];
    bool guarded = cache_writable();

    if (guarded) {
        menu_path(busy, sizeof(busy), storage_prefix, BUSY_FILE);
        if (file_exists(busy)) {
            /* The marker outlived the run that made it, so the last attempt did not come back.
             * Never try again: whatever it costs to not know, it is less than a console that
             * cannot boot. Deleting the file by hand re-arms it, which is documented nowhere
             * except here on purpose -- it is not a thing to invite people to do. */
            result = WATCH_SKIPPED;
            debugf("WATCHTEST refused: %s left over from a previous run\n", busy);
            return;
        }
        FILE *f = fopen(busy, "wb");
        if (f != NULL) {
            fputs("watch exception test in flight\n", f);
            fclose(f);
        }
    }

    /* Physical address. KSEG0 maps by clearing the top three bits, and this variable is in .data,
     * so no TLB is involved and the arithmetic is the whole translation. */
    uint32_t pa = ((uint32_t)(uintptr_t)&watch_target) & 0x1FFFFFFFu;
    uint32_t want = (pa & WATCHLO_PADDR) | WATCHLO_W;
    armed = want;

    prev_handler = register_exception_handler(on_exception);
    watch_fired = false;
    break_seen = false;

    /* Positive control, first, and the whole verdict rests on it.
     *
     * "The store did not trap" has two explanations that look identical from here: the CPU has no
     * watch exception, or on_exception() is never reached and no exception of any kind would be
     * seen. Reporting the first when the truth is the second sends somebody hunting a hardware
     * fault that is not there -- and this test exists precisely because the last two hardware runs
     * were confidently told something that turned out not to be the whole story.
     *
     * A `break` raises a synchronous exception every VR4300 and every emulator of one implements.
     * If it comes back through the handler, the plumbing works and a silent watch is the CPU's
     * answer, not ours. `.set noreorder` with an explicit nop so EPC+4 lands on a real
     * instruction rather than on whatever the assembler decided to put in a delay slot.
     *
     * If EPC writes did not take, this would loop forever -- which is what the interlock file
     * above is for, and which is verified under ares before this ships. */
    asm volatile(".set noreorder\n break\n nop\n .set reorder" ::: "memory");

    WRITE_WATCHHI(0);
    WRITE_WATCHLO(want);
    readback = READ_WATCHLO();

    if (!break_seen) {
        /* The control did not come back, so nothing this function reports about the watch means
         * anything. Say that, rather than the far more alarming and possibly wrong thing. */
        result = WATCH_NO_HANDLER;
    } else if (readback != want) {
        /* The register does not hold. Nothing further is worth trying, and the store is not
         * performed -- an armed-but-broken watch is exactly the state where a store might do
         * something unexpected. */
        result = WATCH_NO_REGISTER;
    } else {
        /* Cached first, through a normal KSEG0 pointer, because that is what the game does when
         * it writes its exception handler to 0x80000180. That is the case the engine needs. */
        watch_target = 0xC0FFEEu;

        if (watch_fired) {
            result = WATCH_WORKING;
        } else {
            /* Then uncached, and the distinction is not academic. A watch compares physical
             * addresses in the pipeline, so on real silicon a cache hit traps exactly like a
             * miss -- but an implementation that only checks the memory bus would trap the
             * second store and not the first. Telling those apart is the difference between
             * "this console cannot run cheats" and "this console traps, just not the way the
             * engine needs", and reporting the first when the truth is the second would send
             * somebody hunting a hardware fault that is not there. */
            volatile uint32_t *uncached =
                (volatile uint32_t *)(((uintptr_t)&watch_target) | 0x20000000u);
            *uncached = 0xC0FFEEu;
            result = watch_fired ? WATCH_UNCACHED_ONLY : WATCH_NO_EXCEPTION;

            /* The two views of that word have now been written separately. Drop the cached line
             * rather than let it write back over the uncached one at some arbitrary later point.
             * Nothing reads this variable, so this is hygiene, not correctness -- but a dirty
             * line aimed at a fixed address is not a thing to leave lying around. */
            data_cache_hit_writeback_invalidate((void *)&watch_target, sizeof(watch_target));
        }
    }

    WRITE_WATCHLO(0);
    WRITE_WATCHHI(0);
    register_exception_handler(prev_handler);

    if (guarded) {
        remove(busy);
    }

    debugf("WATCHTEST %s (target %p, wrote %08lx, read %08lx, break=%d, fired=%d)\n",
           enginetest_text(), (void *)&watch_target, (unsigned long)want,
           (unsigned long)readback, (int)break_seen, (int)watch_fired);
}

watchtest_t enginetest_watch (void) {
    return result;
}

const char *enginetest_text (void) {
    /* Kept short because it is right-aligned in a row that already carries a label: the first
     * version read "Store did not trap -- cheats cannot run", which at 12 px a glyph is wider
     * than the space left over and drew straight through the word "engine". The reasoning that
     * did not fit lives in enginetest.h, and the raw evidence goes to launch.log. */
    switch (result) {
        case WATCH_WORKING:       return "Working";
        case WATCH_NO_EXCEPTION:  return "Not supported by this console";
        case WATCH_UNCACHED_ONLY: return "Uncached stores only";
        case WATCH_NO_REGISTER:   return "No watch register";
        case WATCH_NO_HANDLER:    return "Inconclusive, control failed";
        case WATCH_SKIPPED:       return "Skipped after a bad attempt";
        default:                  return "Not tested";
    }
}

void enginetest_detail (char *out, size_t cap) {
    snprintf(out, cap, "wrote %08lx read %08lx control=%d trapped=%d",
             (unsigned long)armed, (unsigned long)readback,
             (int)break_seen, (int)watch_fired);
}
