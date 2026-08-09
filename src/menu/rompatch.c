/**
 * @file rompatch.c
 * @brief See rompatch.h for why the hook moved out of emitted code and into the cartridge.
 * @ingroup menu
 */

#include <libdragon.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "boot/vr4300_asm.h"
#include "flashcart/flashcart_utils.h"
#include "romcrc.h"
#include "rompatch.h"

/** Cartridge ROM as the CPU sees it over the PI. sc64.c:22 calls the same number ROM_ADDRESS. */
#define ROM_PI_BASE (0x10000000)

/** Where the engine lives, and the rule that decides it. See AUDIT 2y.
 *
 * The boot segment is what IPL3 DMAs into RDRAM -- ROM [0x1000, 0x101000) landing at the entry
 * address -- so code written into a gap there is placed in RDRAM by the same transfer that loads
 * the game. Survival needs nothing from reboot.S, the address is inside the region the cart maps
 * at runtime, and the code runs cached with no PI fetch. The three placements that failed on
 * hardware failed on exactly those three counts (AUDIT 2u), and this one is confirmed working on
 * an M64: a pre-patched Ocarina built this way boots and its cheats take effect.
 *
 * GUARD words of zero are left untouched either side. That is not only slack for a fencepost: when
 * a function's `jr` is followed by a `nop` delay slot the run swallows the delay slot, and the
 * guard is what stops the engine being written over an instruction the game still executes.
 *
 * What counts as a run at all is rompatch_run_is_padding(), over in rompatch_find.c because the
 * host suite has to be able to reach it. */
#define BOOTSEG_START      (0x1000u)
#define BOOTSEG_END        (0x101000u)
#define GAP_GUARD_WORDS    2

/** The engine is allowed to be discontiguous, but not endlessly so.
 *
 * One run holds about six cheats; Ocarina's two hold eight between them. Chaining costs two words
 * a hop and buys the multi-line groups, which a single run cannot hold -- "Infinite Big Key, Small
 * Keys, Compass & Map" alone is nineteen lines. The cap is a blast radius: every segment executes
 * on every exception forever, so each one is another run that has to be genuinely dead, and a
 * selection that needs more than this is refused rather than scattered. Ten segments is what the
 * unbounded version chose for Ocarina, and it black-screened. */
#define ENGINE_MAX_SEGMENTS 4
#define ENGINE_MAX_RUNS     16
#define ENGINE_MAX_ATOMS    48

typedef struct {
    uint32_t rom_off;   /**< Where the engine may be written, guard already skipped */
    uint32_t words;     /**< ...and how many words fit there */
} engine_seg_t;

/**
 * @brief romcrc's reader, over the cartridge.
 *
 * pi_dma_read_data rather than a volatile loop: 262,144 single-word PI reads would take the best
 * part of a minute, and this runs twice per launch.
 */
static bool read_cart (void *ctx, uint32_t offset, void *dst, size_t len) {
    (void)ctx;
    if ((offset % 4) || (len % 4)) {
        return false;
    }
    pi_dma_read_data((void *)(ROM_PI_BASE + offset), dst, len);
    return true;
}

/**
 * @brief Collect the runs of zeros in the boot segment that are really alignment padding.
 *
 * "A run of zeros bounded by non-zeros" is not enough, and the difference is not cosmetic. An
 * engine chained across ten runs that passed that test black-screened Ocarina; two runs that pass
 * this one boot and run it. The rejected runs were zero *fields* inside a pointer table, which
 * from the outside look exactly like padding.
 *
 * What separates them is the word before. Real padding follows a function's last instruction, so a
 * `jr`/`j` sits one or two words back -- two when the delay slot holds a real instruction, one
 * when it is a `nop` and the run therefore swallowed it. Measured over the fifteen-ROM reference
 * shelf: this keeps every run that has been booted successfully, on hardware or in ares, and drops
 * 37 of Ocarina's 39 candidates including all four the failing chain used.
 *
 * Lowest first, which is the other half of AUDIT 2w's correction: that scan took the *last* long
 * enough run on the reasoning that padding collects at the tail, and the tail of the window is
 * somebody's compressed data.
 *
 * @return how many runs were recorded, in ascending ROM order.
 */
static int find_padding_runs (engine_seg_t *out, int max) {
    enum { CHUNK = 4096 };
    uint32_t *buf = memalign(16, CHUNK * sizeof(uint32_t));
    if (buf == NULL) {
        return 0;
    }

    int n = 0;
    uint32_t prev1 = 0, prev2 = 0;          /* the two words behind the cursor */
    uint32_t before1 = 0, before2 = 0;      /* ...as they stood when this run began */
    uint32_t run_start = 0, run_len = 0;

    for (uint32_t at = BOOTSEG_START; at < BOOTSEG_END && n < max; at += CHUNK * 4) {
        uint32_t words = CHUNK;
        if (at + words * 4 > BOOTSEG_END) {
            words = (BOOTSEG_END - at) / 4;
        }
        if (!read_cart(NULL, at, buf, words * 4)) {
            break;
        }
        for (uint32_t i = 0; i < words && n < max; i++) {
            uint32_t w = buf[i];
            if (w == 0) {
                if (run_len == 0) {
                    run_start = at + i * 4;
                    before1 = prev1;
                    before2 = prev2;
                }
                run_len++;
            } else {
                if (rompatch_run_is_padding(before2, before1, run_len * 4)) {
                    out[n].rom_off = run_start + GAP_GUARD_WORDS * 4;
                    out[n].words = run_len - 2 * GAP_GUARD_WORDS;
                    n++;
                }
                run_len = 0;
            }
            prev2 = prev1;
            prev1 = w;
        }
    }

    free(buf);
    return n;
}

/**
 * @brief Lay the engine across as many runs as it takes, lowest first, without splitting an atom.
 *
 * An atom is a run of words that must land contiguously. A plain write is three and could be cut
 * anywhere; a conditional is four words of test whose branch is PC-relative and jumps a fixed
 * three instructions, so the test and the write it guards are seven words that cannot be
 * separated. A segment boundary between them would branch into whatever follows in an unrelated
 * part of the game's boot code.
 *
 * Every segment but the last ends with `j` into the next and its delay slot, so two words are
 * reserved for the hop unless the rest of the engine fits here. A run too small for the next atom
 * is skipped rather than half-filled. Lowest-first means the runs nearest the entry point -- the
 * ones inside the code that boots the game and therefore stays resident -- are spent first, and a
 * one-cheat selection never uses more than one.
 *
 * @return the number of segments used, or 0 if the image has nowhere to put this.
 */
static int place_engine (const engine_seg_t *runs, int n_runs,
                         const uint8_t *atom, int n_atoms,
                         engine_seg_t *segs, int max_segs) {
    int n = 0, a = 0;

    for (int i = 0; i < n_runs && a < n_atoms && n < max_segs; i++) {
        uint32_t rest = 0;
        for (int k = a; k < n_atoms; k++) {
            rest += atom[k];
        }

        /* The hop costs two words, and is not owed if everything left fits here. */
        uint32_t budget = (rest <= runs[i].words) ? runs[i].words
                        : (runs[i].words >= 3 ? runs[i].words - 2 : 0);

        uint32_t used = 0;
        while (a < n_atoms && used + atom[a] <= budget) {
            used += atom[a];
            a++;
        }
        if (used == 0) {
            continue;                       /* too small for the next atom; leave it alone */
        }
        segs[n].rom_off = runs[i].rom_off;
        segs[n].words = used;
        n++;
    }

    return (a == n_atoms) ? n : 0;          /* short: write nothing, leave the cartridge alone */
}

/**
 * @brief Assemble the engine, and record where it may be cut.
 *
 * $k0 and $k1 only. At exception entry those two are the kernel's scratch registers and libultra's
 * `__osException` clobbers $k0 in its own first instruction, so nothing downstream can observe
 * either and nothing has to be saved. Every cheat address is KSEG0, which is unmapped, so no store
 * here can take a TLB miss inside an exception -- the one way this could turn a working game into
 * a reset loop.
 *
 * A plain write is three instructions. A conditional is four more in front of them, and the shape
 * is lifted from src/boot/cheats.c rather than invented:
 *
 *     lui   $k0, %hi(addr)
 *     lbu   $k0, %lo(addr)($k0)      # lhu for the 16-bit types
 *     ori   $k1, $zero, value
 *     bne   $k0, $k1, +3             # beq for the NOT-EQUAL types
 *     lui   $k0, %hi(target)         # the guarded write's own first word, in the delay slot
 *     ori   $k1, $zero, value
 *     sh    $k1, %lo(target)($k0)
 *
 * The branch has no `nop`: its delay slot is the write's `lui`, which always executes and is
 * harmless because it only loads a register the store is being skipped anyway. That is why the
 * offset is 3 rather than 4, and it is worth stating because writing the obvious `nop` here would
 * make the branch land one instruction short -- on the store, executing exactly what the
 * conditional exists to prevent.
 *
 * Types, from the Datel encoding: bit 0 is 16-bit, bit 1 is NOT-EQUAL, bit 3 is the GS button.
 * A conditional guards the single line after it, which must be a write -- a dangling one is
 * refused by rompatch_cheats_fit() before anything is written.
 *
 * The tail is the preamble's own two words, verbatim, then `jr $k0`. `__osException` is entered
 * with exactly the register state it had before -- its own address in $k0. The inline stub of
 * AUDIT 2x reached it with `j` instead, having spent $k0 as a store base, and black-screened.
 *
 * @param atom     Filled with the word count of each indivisible block; see place_engine().
 * @return words emitted, or 0 if the selection does not fit in @p cap.
 */
static uint32_t emit_engine (const uint32_t *list, uint32_t word0, uint32_t word1,
                             uint32_t *out, uint32_t cap,
                             uint8_t *atom, int *n_atoms, int max_atoms, int *codes_out) {
    uint32_t n = 0;
    int codes = 0, atoms = 0;

    for (size_t i = 0; list != NULL && !(list[i] == 0 && list[i + 1] == 0); ) {
        uint32_t type = (list[i] >> 24) & 0xFFu;
        uint32_t start = n;

        if ((type & 0xF0u) == 0xD0u) {
            /* Four words of test, then the write it guards, consumed together. */
            uint32_t caddr = list[i] & 0xA07FFFFFu;
            bool c16 = (type & 0x01u) != 0;
            uint16_t cval = (uint16_t)(list[i + 1] & (c16 ? 0xFFFFu : 0x00FFu));

            if (n + 7 + 4 > cap || atoms >= max_atoms) {
                return 0;
            }
            out[n++] = I_LUI(REG_K0, A_BASE(caddr));
            out[n++] = c16 ? I_LHU(REG_K0, A_OFFSET(caddr), REG_K0)
                           : I_LBU(REG_K0, A_OFFSET(caddr), REG_K0);
            out[n++] = I_ORI(REG_K1, REG_ZERO, cval);
            out[n++] = (type & 0x02u) ? I_BEQ(REG_K0, REG_K1, 3) : I_BNE(REG_K0, REG_K1, 3);
            i += 2;

            /* A dangling conditional is refused by rompatch_cheats_fit(), which is this
             * function's documented precondition -- but getting here anyway would read the
             * {0,0} terminator as a write and emit a store to address 0. That is KUSEG, which
             * is TLB-mapped, so it would take a TLB miss inside an exception handler and put
             * the console in a reset loop. Two lines against a catastrophic failure mode. */
            if (list[i] == 0 && list[i + 1] == 0) {
                return 0;
            }
        } else if (n + 3 + 4 > cap || atoms >= max_atoms) {
            return 0;
        }

        uint32_t wtype = (list[i] >> 24) & 0xFFu;
        uint32_t address = list[i] & 0xA07FFFFFu;
        bool w16 = (wtype & 0x01u) != 0;
        uint16_t value = (uint16_t)(list[i + 1] & (w16 ? 0xFFFFu : 0x00FFu));

        out[n++] = I_LUI(REG_K0, A_BASE(address));
        out[n++] = I_ORI(REG_K1, REG_ZERO, value);
        out[n++] = w16 ? I_SH(REG_K1, A_OFFSET(address), REG_K0)
                       : I_SB(REG_K1, A_OFFSET(address), REG_K0);
        i += 2;

        atom[atoms++] = (uint8_t)(n - start);
        codes++;
    }

    if (n + 4 > cap || atoms >= max_atoms) {
        return 0;
    }
    out[n++] = word0;
    out[n++] = word1;
    out[n++] = I_JR(REG_K0);
    out[n++] = I_NOP();
    atom[atoms++] = 4;

    if (codes_out != NULL) {
        *codes_out = codes;
    }
    if (n_atoms != NULL) {
        *n_atoms = atoms;
    }
    return n;
}

/**
 * @brief Everything both entry points share: find, gate, write two words, fix the checksum, read
 *        back. Only the two words differ.
 */
static bool install (cic_type_t cic_type, uint32_t header_entry,
                     const uint32_t *cheat_list, rompatch_result_t *out) {
    engine_seg_t segs[ENGINE_MAX_SEGMENTS];
    uint32_t engine[ENGINE_MAX_WORDS];
    uint8_t atom[ENGINE_MAX_ATOMS];
    uint32_t engine_len = 0;
    int n_atoms = 0, n_segs = 0;

    memset(out, 0, sizeof(*out));
    out->attempted = true;
    out->entry = rompatch_entry_for(cic_type, header_entry);

    /* The gate, and the reason any of this is safe to ship. If our checksum does not already
     * describe this cartridge then either this code does not understand this ROM or the ROM was
     * broken before we saw it, and in both cases the honest move is to change nothing. Measured:
     * 23 of the 24 N64 ROMs on the reference card pass, the exception being Pokemon Stadium,
     * whose header disagreed with its own contents already. */
    out->crc_ok = romcrc_verify(read_cart, NULL, cic_type, &out->old_crc1, &out->old_crc2);
    if (!out->crc_ok) {
        return false;
    }

    if (!rompatch_find(read_cart, NULL, out->entry,
                       0x80000000u + (uint32_t)get_memory_size(), out)) {
        return false;
    }

    /* Two instructions, computing the engine's address into $k0 for the `jr $k0` that already
     * follows them. The engine's tail replays word0 and word1 to reach the game's real
     * __osException, so nothing the preamble did is lost. */
    uint32_t hook[2];
    const int hook_words = 2;
    {
        /* The engine, into the game's own boot segment, and the preamble aimed at its RAM address.
         * Everything is inside the checksum window, so the recompute below covers all of it.
         *
         * Assembled and placed before a single word is written: emit_engine() fails if the
         * selection is too big, place_engine() fails if the image has nowhere for it, and either
         * one leaves the cartridge exactly as it was. A half-written engine reachable from a
         * patched preamble is the one state this must never produce. */
        engine_seg_t runs[ENGINE_MAX_RUNS];
        int n_runs = find_padding_runs(runs, ENGINE_MAX_RUNS);

        engine_len = emit_engine(cheat_list, out->word0, out->word1,
                                 engine, ENGINE_MAX_WORDS,
                                 atom, &n_atoms, ENGINE_MAX_ATOMS, &out->engine_codes);
        if (engine_len == 0) {
            return false;
        }
        n_segs = place_engine(runs, n_runs, atom, n_atoms, segs, ENGINE_MAX_SEGMENTS);
        if (n_segs == 0) {
            return false;
        }

        out->engine_rom_off = segs[0].rom_off;
        out->engine_ram = out->entry + (segs[0].rom_off - BOOTSEG_START);
        out->engine_run_words = engine_len;
        out->engine_segments = n_segs;

        uint32_t k = 0;
        for (int s = 0; s < n_segs; s++) {
            uint32_t at = segs[s].rom_off;
            for (uint32_t i = 0; i < segs[s].words; i++) {
                io_write(ROM_PI_BASE + at + i * 4, engine[k++]);
            }
            if (s + 1 < n_segs) {
                uint32_t next = out->entry + (segs[s + 1].rom_off - BOOTSEG_START);
                io_write(ROM_PI_BASE + at + segs[s].words * 4, I_J(next));
                io_write(ROM_PI_BASE + at + segs[s].words * 4 + 4, I_NOP());
            }
        }

        hook[0] = I_LUI(REG_K0, A_BASE(out->engine_ram));
        hook[1] = I_ADDIU(REG_K0, REG_K0, A_OFFSET(out->engine_ram));
    }
    for (int i = 0; i < hook_words; i++) {
        io_write(ROM_PI_BASE + out->rom_offset + (uint32_t)(i * 4), hook[i]);
        out->patch[i] = hook[i];
    }
    out->patch_words = hook_words;

    /* Recompute over the patched image, then write the header. In this order and never the other:
     * the checksum has to describe what is actually in the cartridge, and if anything goes wrong
     * before this point the image and its header still agree and the game boots unmodified. */
    if (!romcrc_compute_ex(read_cart, NULL, cic_type, false, &out->new_crc1, &out->new_crc2)) {
        /* Cannot happen -- romcrc_verify already succeeded with this CIC -- but putting the words
         * back is two stores and leaves a cartridge that boots rather than one that does not. */
        io_write(ROM_PI_BASE + out->rom_offset, out->word0);
        io_write(ROM_PI_BASE + out->rom_offset + 4, out->word1);
        return false;
    }
        io_write(ROM_PI_BASE + 0x10, out->new_crc1);
        io_write(ROM_PI_BASE + 0x14, out->new_crc2);
    out->written = true;

    /* Read back what the cartridge now holds, rather than what we believe we wrote. This is the
     * entire point of moving the patch here: the old emitted version could not check itself, and
     * three hardware runs went by without anyone being able to say whether the bytes had changed. */
    uint32_t back[4], crc_back[2];
    if (!read_cart(NULL, out->rom_offset, back, (size_t)hook_words * 4)) {
        return false;
    }
    if (!read_cart(NULL, 0x10, crc_back, sizeof(crc_back))) {
        return false;
    }
    out->verified = (crc_back[0] == out->new_crc1) && (crc_back[1] == out->new_crc2);
    for (int i = 0; i < hook_words; i++) {
        if (back[i] != hook[i]) {
            out->verified = false;
        }
    }

    /* And the engine itself, every word of every segment. The hook is two words that point at it;
     * if those landed and the engine did not, the preamble aims the exception vector at whatever
     * happens to be there, which is a black screen with a perfectly clean hook read-back. That is
     * precisely the state the older placements kept reporting as success. */
    if (n_segs > 0) {
        uint32_t k = 0;
        for (int s = 0; s < n_segs && out->verified; s++) {
            uint32_t back[ENGINE_MAX_WORDS];
            if (!read_cart(NULL, segs[s].rom_off, back, segs[s].words * 4)) {
                out->verified = false;
                break;
            }
            for (uint32_t i = 0; i < segs[s].words; i++) {
                if (back[i] != engine[k + i]) {
                    out->verified = false;
                }
            }
            k += segs[s].words;
        }
    }

    /* And once more from the top: does the patched image now agree with the checksum we just gave
     * it? A read-back proves the four words we wrote are in the cartridge; this proves the whole
     * megabyte and its header are consistent by our own arithmetic, which is the claim IPL3 is
     * about to test. It costs one more pass over a megabyte, which is milliseconds. */
    uint32_t again1, again2, hdr[2];
    out->reverified = romcrc_compute_ex(read_cart, NULL, cic_type, false, &again1, &again2)
                   && read_cart(NULL, 0x10, hdr, sizeof(hdr))
                   && again1 == hdr[0] && again2 == hdr[1];

    return out->verified && out->reverified;
}

bool rompatch_install_engine (cic_type_t cic_type, uint32_t header_entry,
                              const uint32_t *cheat_list, rompatch_result_t *out) {
    return install(cic_type, header_entry, cheat_list, out);
}

