/**
 * @file cheats.c
 * @brief Cheat Engine Implementation
 * @ingroup boot
 */

#include <libdragon.h>
#include "boot_io.h"
#include "cheats.h"
#include "vr4300_asm.h"

#define HIT_INVALIDATE_I ((4 << 2) | 0)
#define HIT_WRITE_BACK_D ((6 << 2) | 1)

#define D_CACHE_LINE_SIZE (16)

#define CAUSE_IRQ_PRE_NMI (1 << 12)
#define CAUSE_EXC_CODE_MASK (0x7C)
#define CAUSE_EXC_CODE_WATCH (0x5C)

#define WATCHLO_W (1 << 0)

#define RELOCATED_EXCEPTION_HANDLER_ADDRESS (0x80000120)
#define EXCEPTION_HANDLER_ADDRESS (0x80000180)
#define PATCHER_ADDRESS (0x80700000)
#define ENGINE_TEMPORARY_ADDRESS (PATCHER_ADDRESS + 0x10000)
#define DEFAULT_ENGINE_ADDRESS (0x807C5C00)

/* Written by the patcher, read by the engine. Below the default engine address rather than
 * inside the engine, so a 0xDE code that relocates the engine does not move it and the two
 * halves cannot disagree about where it is. Both accesses are cached and this is the only
 * writer, so no cache maintenance is owed. */
#define BEACON_STATE_ADDRESS (DEFAULT_ENGINE_ADDRESS - 16)

static bool beacon_enabled;

void cheats_set_beacon (bool enabled) {
    beacon_enabled = enabled;
}

/* Worst-case words a single cheat entry can emit. A 0x50 repeater carries an 8-bit count and
 * emits three instructions per iteration, so 255 * 3 = 765, plus the four-word tail this function
 * always appends. Checked BEFORE each entry rather than after each write, because the write is
 * `*engine_p++ =` in a dozen places and a check at each one is a dozen chances to miss one. */
#define ENGINE_WORST_ENTRY_WORDS (765 + 4)

/* The top halves of the first two instructions of libultra's __osExceptionPreamble --
 * `lui $k0, %hi(__osException)` and `addiu $k0, $k0, %lo(__osException)` -- followed by
 * `jr $k0` and a nop. Only the two address halves vary between games, which is what makes the
 * preamble findable by a masked scan of the loaded game image. See the long comment above the
 * scan emission in cheats_emit(). */
#define PREAMBLE_LUI_K0_HI16   (I_LUI(REG_K0, 0) >> 16)
#define PREAMBLE_ADDIU_K0_HI16 (I_ADDIU(REG_K0, REG_K0, 0) >> 16)

/* The patcher region runs from PATCHER_ADDRESS up to where the engine is staged. Nothing
 * enforced this before: a long enough list of boot-writes walked straight into the staging
 * buffer and corrupted the engine it was about to copy. */
#define PATCHER_MAX_WORDS ((ENGINE_TEMPORARY_ADDRESS - PATCHER_ADDRESS) / 4)


/** @brief Emit the four words that stamp @p colour into the beacon's state word. */
static io32_t *emit_beacon_stamp (io32_t *p, uint32_t colour) {
    *p++ = I_LUI(REG_K0, colour >> 16);
    *p++ = I_ORI(REG_K0, REG_K0, colour);
    *p++ = I_LUI(REG_K1, A_BASE(BEACON_STATE_ADDRESS));
    *p++ = I_SW(REG_K0, A_OFFSET(BEACON_STATE_ADDRESS), REG_K1);
    return p;
}

/** @brief Cheat structure */
typedef struct {
    uint8_t type; /**< Cheat type */
    uint32_t address; /**< Cheat address */
    uint16_t value; /**< Cheat value */
} cheat_t;

/** @brief Cheat entry structure */
typedef struct {
    cheat_t main; /**< Main cheat */
    cheat_t sub; /**< Sub cheat */
} cheat_entry_t;

/** @brief Special cheat types enumeration */
typedef enum {
    SPECIAL_CLEAR_MEMORY = 0x20, /**< Clear memory between 0x80000200-0x80000300 on boot */
    SPECIAL_SECONDARY_EXCEPTION_HANDLER = 0xCC, /**< Use alternate exception handler */
    SPECIAL_SET_ENTRYPOINT_ADDR = 0xDE, /**< Set boot entrypoint address */
    SPECIAL_DISABLE_EXPANSION_PAK = 0xEE, /**< Disable Expansion Pak */
    SPECIAL_WRITE_BYTE_ON_BOOT = 0xF0, /**< Write byte on boot */
    SPECIAL_WRITE_SHORT_ON_BOOT = 0xF1, /**< Write short on boot */
    SPECIAL_SET_STORE_LOCATION = 0xFF, /**< Set store location */
} cheat_type_special_t;

#define IS_WIDTH_16(t) ((t) & (1 << 0))
#define IS_CONDITION_NOT_EQUAL(t) ((t) & (1 << 1))
#define IS_CONDITION_GS_BUTTON(t) ((t) & (1 << 3))

#define IS_TYPE_REPEATER(t) ((t) == 0x50)
#define IS_TYPE_WRITE(t) ((((t)&0xF0) == 0x80) || (((t)&0xF0) == 0xA0))
#define IS_TYPE_CONDITIONAL(t) (((t)&0xF0) == 0xD0)

#define IS_DOUBLE_ENTRY(t) (IS_TYPE_CONDITIONAL(t) || IS_TYPE_REPEATER(t))

#define X106_XOR_CONSTANT (0x0260BCD5)
#define X106_ENC_START (0x13C)

/**
 * @brief Get the XOR value for a given offset in the CIC x106 encrypted area.
 *
 * Calls to this function ought to always be reduced to constants.
 *
 * @param seed The IPL3 checksum seed (should always be 0x85 for x106; see cic_get_seed()).
 * @param offset The offset in the encrypted area to calculate for.
 * @return the calculated XOR value.
 */
__attribute__((always_inline))
static inline uint32_t cheats_calc_x106_xor(uint8_t seed, uint8_t offset) {
    uint32_t val = X106_XOR_CONSTANT * seed + 1;
    #pragma GCC unroll 256
    for (uint8_t i = 0; i < offset; i++) {
        val *= X106_XOR_CONSTANT;
    }
    return val;
}

/**
 * @brief Patch the IPL3 with the cheat engine.
 * 
 * @param cic_type The CIC type.
 * @param target The target address.
 * @return true if successful, false otherwise.
 */
int cheats_ipl3_patch_offset (cic_type_t cic_type) {
    switch (cic_type) {
    case CIC_5101: return 476;
    case CIC_6101:
    case CIC_7102: return 466;
    case CIC_x102: return 475;
    case CIC_x103: return 472;
    case CIC_x105: return 499;
    case CIC_x106: return 488;
    default: return -1;
    }
}

bool cheats_ipl3_layout_ok (cic_type_t cic_type, uint32_t word_at_offset) {
    int patch_offset = cheats_ipl3_patch_offset(cic_type);
    if (patch_offset < 0) {
        return false;
    }
    if (cic_type == CIC_x106) {
        // NOTE: CIC x106 IPL3 is partially scrambled
        word_at_offset ^= cheats_calc_x106_xor(cic_get_seed(cic_type),
                                               (uint32_t)patch_offset - X106_ENC_START);
    }
    return word_at_offset == I_JR(REG_T1);
}

static bool cheats_patch_ipl3 (cic_type_t cic_type, io32_t *target) {
    uint32_t j_instruction = I_J((uint32_t)(target));

    io32_t *ipl3 = SP_MEM->DMEM;

    int offset = cheats_ipl3_patch_offset(cic_type);
    if (offset < 0) {
        return true;
    }
    uint32_t patch_offset = (uint32_t)offset;

    /* NOTE: Check for "jr $t1" instruction
     *       Libdragon IPL3 could be brute-force signed with any retail
     *       CIC seed and checksum, and we support only retail libultra IPL3
     *
     * This returned FALSE here, which this function's callers read as success -- so an IPL3 we
     * had just failed to recognise was reported as patched, the jump below was never written,
     * and cheats_install() went on to build the whole engine, return true, and have boot.c set
     * skip_rdram_reset on the strength of it. The engine was assembled, never hooked, and the
     * game booted with cheats silently doing nothing. Of the 23 retail ROMs measured with
     * tools/hosttest/test_cheatinstall.c, one -- Star Fox 64, CIC 6101 -- takes this path. */
    if (!cheats_ipl3_layout_ok(cic_type, cpu_io_read(&ipl3[patch_offset]))) {
        return true;
    }

    switch (cic_type) {
    case CIC_x105:
        // NOTE: This disables game code checksum verification
        cpu_io_write(&ipl3[486], I_NOP());
        break;

    case CIC_x106:
        // NOTE: CIC x106 IPL3 is partially scrambled
        j_instruction ^= cheats_calc_x106_xor(cic_get_seed(cic_type), patch_offset - X106_ENC_START);
        break;

    default: break;
    }

    cpu_io_write(&ipl3[patch_offset], j_instruction);

    return false;
}

/**
 * @brief Get the next cheat entry from the cheat list.
 * 
 * @param cheat_list Pointer to the cheat list.
 * @param cheat Pointer to the cheat entry structure.
 * @return true if successful, false otherwise.
 */
static bool cheats_get_next (uint32_t **cheat_list, cheat_entry_t *cheat) {
    cheat_t *c = &cheat->main;
    cheat->sub.type = 0;

    for (int i = 0; i < 2; i++) {
        uint32_t raw[2] = {(*cheat_list)[0], (*cheat_list)[1]};

        (*cheat_list) += 2;

        if ((raw[0] == 0) && (raw[1] == 0)) {
            return false;
        }

        c->type = ((raw[0] >> 24) & 0xFF);
        c->address = (raw[0] & 0xA07FFFFF);
        c->value = (raw[1] & 0xFFFF);

        if (!IS_DOUBLE_ENTRY(c->type)) {
            break;
        }

        c = &cheat->sub;
    }

    return true;
}

/**
 * @brief Get the engine address from the cheat list.
 * 
 * @param cheat_list Pointer to the cheat list.
 * @return io32_t* The engine address.
 */
static io32_t *cheats_get_engine_address (uint32_t *cheat_list) {
    cheat_entry_t cheat;
    while (cheats_get_next(&cheat_list, &cheat)) {
        if (cheat.main.type == SPECIAL_SET_STORE_LOCATION) {
            return (io32_t *)(cheat.main.address & 0x807FFFFF);
        }
    }
    return (io32_t *)(DEFAULT_ENGINE_ADDRESS);
}

/**
 * @brief Update the cache for the specified memory range.
 * 
 * @param start The start address.
 * @param end The end address.
 */
static void cheats_update_cache (volatile void *start, volatile void *end) {
    data_cache_hit_writeback(start, (end - start));
    inst_cache_hit_invalidate(start, (end - start));
}

uint32_t *cheats_emit (uint32_t *cheat_list) {
    if (!cheat_list) {
        return NULL;
    }

    io32_t *engine_start = (io32_t *)(ENGINE_TEMPORARY_ADDRESS);
    io32_t *engine_p = engine_start;

    io32_t *patcher_start = (io32_t *)(PATCHER_ADDRESS);
    io32_t *patcher_p = patcher_start;

    io32_t *final_engine_address = cheats_get_engine_address(cheat_list);

    // Original watch exception handler code written by Jay Oster 'Parasyte'
    // https://github.com/parasyte/alt64/blob/master/utils.c#L1024-L1054

    uint32_t ori_placeholder_instruction = I_ORI(REG_ZERO, REG_K0, A_OFFSET(RELOCATED_EXCEPTION_HANDLER_ADDRESS));
    uint32_t ori_placeholder_address = (uint32_t)(final_engine_address + 20);

    // Load cause register
    *engine_p++ = I_MFC0(REG_K0, C0_REG_CAUSE);

    // Disable watch exception when reset button is pressed
    *engine_p++ = I_ANDI(REG_K1, REG_K0, CAUSE_IRQ_PRE_NMI);
    *engine_p++ = I_BNEL(REG_K1, REG_ZERO, 1);
    *engine_p++ = I_MTC0(REG_ZERO, C0_REG_WATCH_LO);

    // Check if watch exception occurred, if yes then proceed to relocate the game exception handler
    *engine_p++ = I_ANDI(REG_K0, REG_K0, CAUSE_EXC_CODE_MASK);
    *engine_p++ = I_ORI(REG_K1, REG_ZERO, CAUSE_EXC_CODE_WATCH);
    *engine_p++ = I_BNE(REG_K0, REG_K1, 15); // Skips to after the 'eret' instruction

    // Extract base register number from the store instruction
    *engine_p++ = I_MFC0(REG_K1, C0_REG_EPC);
    *engine_p++ = I_LW(REG_K1, 0, REG_K1);
    *engine_p++ = I_LUI(REG_K0, 0x03E0);
    *engine_p++ = I_AND(REG_K1, REG_K0, REG_K1);
    *engine_p++ = I_SRL(REG_K1, REG_K1, 5);

    // Update create final instruction and update its target register number
    *engine_p++ = I_LUI(REG_K0, ori_placeholder_instruction >> 16);
    *engine_p++ = I_ORI(REG_K0, REG_K0, ori_placeholder_instruction);
    *engine_p++ = I_OR(REG_K0, REG_K0, REG_K1);

    // Write created instruction into placeholder
    *engine_p++ = I_LUI(REG_K1, A_BASE(ori_placeholder_address));
    *engine_p++ = I_SW(REG_K0, A_OFFSET(ori_placeholder_address), REG_K1);

    // Force write and instruction cache invalidation
    *engine_p++ = I_CACHE(HIT_WRITE_BACK_D, A_OFFSET(ori_placeholder_address), REG_K1);
    *engine_p++ = I_CACHE(HIT_INVALIDATE_I, A_OFFSET(ori_placeholder_address), REG_K1);

    // Load address base and execute created instruction
    *engine_p++ = I_LUI(REG_K0, A_BASE(RELOCATED_EXCEPTION_HANDLER_ADDRESS));
    *engine_p++ = I_NOP();

    // Return from the exception
    *engine_p++ = I_ERET();

    /* Everything below here runs on EVERY exception the engine sees -- the `bne` above skips the
     * watch-relocation block and lands exactly on this instruction. Which is why the beacon goes
     * here and not at the top: reaching this point is the definition of "the engine ran". */
    if (beacon_enabled) {
        io32_t *beacon_start = engine_p;

        *engine_p++ = I_LUI(REG_K0, A_BASE(VI_ORIGIN_ADDRESS));
        *engine_p++ = I_LW(REG_K1, A_OFFSET(VI_ORIGIN_ADDRESS), REG_K0);

        /* Nothing below a megabyte is a framebuffer. $k0 is scratch either way. */
        *engine_p++ = I_SRL(REG_K0, REG_K1, BEACON_MIN_ORIGIN_SHIFT);
        io32_t *skip = engine_p;
        *engine_p++ = I_NOP();              /* back-patched to beq $k0, $zero, past the stores */
        *engine_p++ = I_NOP();              /* branch delay slot */

        /* Uncached, so the bar reaches the screen without depending on a writeback the game has
         * no reason to perform. The OR covers all three ways VI_ORIGIN is written -- physical,
         * KSEG0 or KSEG1 -- because every one of them ORs to the same KSEG1 address. */
        *engine_p++ = I_LUI(REG_K0, 0xA000);
        *engine_p++ = I_OR(REG_K1, REG_K1, REG_K0);

        *engine_p++ = I_LUI(REG_K0, A_BASE(BEACON_STATE_ADDRESS));
        *engine_p++ = I_LW(REG_K0, A_OFFSET(BEACON_STATE_ADDRESS), REG_K0);

        /* Unrolled rather than looped, because a loop needs a third register to hold its limit
         * and the engine's contract with the game is that it touches $k0 and $k1 and nothing
         * else. 256 stores cost 256 words of an engine that has hundreds of kilobytes. */
        for (int i = 0; i < BEACON_WORDS; i++) {
            *engine_p++ = I_SW(REG_K0, i * 4, REG_K1);
        }

        *skip = I_BEQ(REG_K0, REG_ZERO, (int)(engine_p - (skip + 1)));
        debugf("cheats: beacon armed, %u words\n", (unsigned)(engine_p - beacon_start));
    }

    cheat_entry_t cheat;

    /* Where the engine will END UP, not where it is staged, is what bounds it: the staging
     * buffer has 743 KB of slack while the final location has whatever sits between it and the
     * top of RDRAM. get_memory_size() rather than a hardcoded 8 MB, so this stays correct if the
     * engine address is overridden by a 0xDE code to somewhere unexpected.
     *
     * Before this, `*engine_p++` had no bound at all. AUDIT.md 2.3: a 0x50 repeater emits three
     * instructions per iteration with a count of up to 255, so a handful of them ran off the end
     * of the buffer and wrote through whatever followed. */
    uint32_t rdram_top = 0x80000000u + (uint32_t)get_memory_size();
    uint32_t engine_final = (uint32_t)final_engine_address;
    size_t engine_max_words = (engine_final < rdram_top) ? (rdram_top - engine_final) / 4 : 0;

    while (cheats_get_next(&cheat_list, &cheat)) {
        cheat_t *c = &cheat.main;

        if ((size_t)(engine_p - engine_start) + ENGINE_WORST_ENTRY_WORDS > engine_max_words) {
            debugf("cheats: engine full at %u words (max %u), refusing to install\n",
                   (unsigned)(engine_p - engine_start), (unsigned)engine_max_words);
            return NULL;
        }
        if ((size_t)(patcher_p - patcher_start) + ENGINE_WORST_ENTRY_WORDS > PATCHER_MAX_WORDS) {
            debugf("cheats: patcher full at %u words, refusing to install\n",
                   (unsigned)(patcher_p - patcher_start));
            return NULL;
        }

        switch (c->type) {
            case SPECIAL_WRITE_BYTE_ON_BOOT:
            case SPECIAL_WRITE_SHORT_ON_BOOT: {
                *patcher_p++ = I_LUI(REG_K0, A_BASE(c->address));
                *patcher_p++ = I_ORI(REG_K1, REG_ZERO, c->value);
                *patcher_p++ = IS_WIDTH_16(c->type) ? I_SH(REG_K1, A_OFFSET(c->address), REG_K0)
                                                    : I_SB(REG_K1, A_OFFSET(c->address), REG_K0);
                break;
            }
            case SPECIAL_CLEAR_MEMORY: {
                *patcher_p++ = I_LUI(REG_K0, 0xA000);
                *patcher_p++ = I_ORI(REG_K1, REG_K0, (0x300 - 0x200) - 4);
                *patcher_p++ = I_SW(REG_ZERO, 0x0200, REG_K0);
                *patcher_p++ = I_BNE(REG_K0, REG_K1, -2); // could be BNEL
                *patcher_p++ = I_ADDIU(REG_K0, REG_K0, 4);
                break;
            }
            // N/A
            case SPECIAL_SECONDARY_EXCEPTION_HANDLER:
            // not needed with N64FlashcartMenu's boot method
            case SPECIAL_SET_ENTRYPOINT_ADDR:
            // already handled
            case SPECIAL_SET_STORE_LOCATION: {
                // do nothing
                break;
            }
            case SPECIAL_DISABLE_EXPANSION_PAK: {
                *patcher_p++ = I_LUI(REG_K0, 0xA000);
                *patcher_p++ = I_LUI(REG_K1, 0x0040);
                *patcher_p++ = I_SW(REG_K1, 0x318, REG_K0);
                *patcher_p++ = I_SW(REG_K1, 0x3F0, REG_K0);
                break;
            }
            default: {
                if (IS_TYPE_REPEATER(c->type)) {
                    if ((!IS_TYPE_WRITE(cheat.sub.type)) || IS_CONDITION_GS_BUTTON(cheat.sub.type)) {
                        continue;
                    }

                    int count = ((c->address >> 8) & 0xFF);
                    int step = (c->address & 0xFF);
                    int16_t increment = (int16_t)(c->value);

                    c = &cheat.sub;

                    for (int i = 0; i < count; i++) {
                        *engine_p++ = I_LUI(REG_K0, A_BASE(c->address));
                        *engine_p++ = I_ORI(REG_K1, REG_ZERO, c->value);
                        *engine_p++ = IS_WIDTH_16(c->type) ? I_SH(REG_K1, A_OFFSET(c->address), REG_K0)
                                                        : I_SB(REG_K1, A_OFFSET(c->address), REG_K0);

                        c->address += step;
                        c->value += increment;
                    }

                    continue;
                }

                if (IS_TYPE_CONDITIONAL(c->type)) {
                    if ((!IS_TYPE_WRITE(cheat.sub.type)) || IS_CONDITION_GS_BUTTON(cheat.sub.type)) {
                        continue;
                    }

                    *engine_p++ = I_LUI(REG_K0, A_BASE(c->address));
                    *engine_p++ = IS_WIDTH_16(c->type) ? I_LHU(REG_K0, A_OFFSET(c->address), REG_K0)
                                                    : I_LBU(REG_K0, A_OFFSET(c->address), REG_K0);
                    *engine_p++ = I_ORI(REG_K1, REG_ZERO, c->value & (IS_WIDTH_16(c->type) ? 0xFFFF : 0xFF));
                    *engine_p++ = IS_CONDITION_NOT_EQUAL(c->type) ? I_BEQ(REG_K0, REG_K1, 3) : I_BNE(REG_K0, REG_K1, 3);

                    c = &cheat.sub;
                }

                if (IS_TYPE_WRITE(c->type)) {
                    if (IS_CONDITION_GS_BUTTON(c->type)) {
                        continue;
                    }

                    *engine_p++ = I_LUI(REG_K0, A_BASE(c->address));
                    *engine_p++ = I_ORI(REG_K1, REG_ZERO, c->value);
                    *engine_p++ = IS_WIDTH_16(c->type) ? I_SH(REG_K1, A_OFFSET(c->address), REG_K0)
                                                    : I_SB(REG_K1, A_OFFSET(c->address), REG_K0);

                    continue;
                }
            }
        }
    }

    /* The tail is four words and both hooks share it:
     *
     *     lui   $k0, hi        \ default: 0x80000120, the relocated game handler,
     *     addiu $k0, $k0, lo   / which is where the watch hook parks the game's own code
     *     jr    $k0
     *     nop
     *
     * In preamble mode the patcher overwrites the first two words with the two words it found at
     * the top of the game's __osExceptionPreamble -- which are themselves a lui/addiu pair
     * computing __osException into $k0 -- so the tail becomes a verbatim replay of the handler
     * the game believes it installed. No address arithmetic happens at runtime, and no register
     * beyond $k0 is touched, which is the same contract the preamble itself honours. */
    uint32_t tail_offset_words = (uint32_t)(engine_p - engine_start);
    *engine_p++ = I_LUI(REG_K0, A_BASE(RELOCATED_EXCEPTION_HANDLER_ADDRESS));
    *engine_p++ = I_ADDIU(REG_K0, REG_K0, A_OFFSET(RELOCATED_EXCEPTION_HANDLER_ADDRESS));
    *engine_p++ = I_JR(REG_K0);
    *engine_p++ = I_NOP();

    uint32_t j_engine_from_handler = I_J((uint32_t)(final_engine_address));

    // Copy engine to the final location
    *patcher_p++ = I_LUI(REG_T3, A_BASE((uint32_t)(engine_start)));
    *patcher_p++ = I_ADDIU(REG_T3, REG_T3, A_OFFSET((uint32_t)(engine_start)));

    *patcher_p++ = I_LUI(REG_T4, A_BASE((uint32_t)(engine_p)));
    *patcher_p++ = I_ADDIU(REG_T4, REG_T4, A_OFFSET((uint32_t)(engine_p)));

    *patcher_p++ = I_LUI(REG_T5, A_BASE((uint32_t)(final_engine_address)));
    *patcher_p++ = I_ADDIU(REG_T5, REG_T5, A_OFFSET((uint32_t)(final_engine_address)));

    *patcher_p++ = I_ORI(REG_T6, REG_ZERO, 0);

    *patcher_p++ = I_LW(REG_K1, 0, REG_T3);
    *patcher_p++ = I_SW(REG_K1, 0, REG_T5);
    *patcher_p++ = I_ADDIU(REG_T3, REG_T3, 4);
    *patcher_p++ = I_ADDIU(REG_T5, REG_T5, 4);
    *patcher_p++ = I_BNE(REG_T3, REG_T4, -5);
    *patcher_p++ = I_ADDIU(REG_T6, REG_T6, 4);

    // Force write and invalidate instruction cache
    *patcher_p++ = I_LUI(REG_T5, A_BASE((uint32_t)(final_engine_address)));
    *patcher_p++ = I_ADDIU(REG_T5, REG_T5, A_OFFSET((uint32_t)(final_engine_address)));

    *patcher_p++ = I_CACHE(HIT_WRITE_BACK_D, 0, REG_T5);
    *patcher_p++ = I_CACHE(HIT_INVALIDATE_I, 0, REG_T5);
    *patcher_p++ = I_ADDIU(REG_T6, REG_T6, -D_CACHE_LINE_SIZE);
    *patcher_p++ = I_BGTZ(REG_T6, -4);
    *patcher_p++ = I_ADDIU(REG_T5, REG_T5, D_CACHE_LINE_SIZE);

    /* Two ways to get the engine into the game's exception path, tried in this order:
     *
     * 1. Rewrite the game's own handler template. At this point in the patcher the game's first
     *    1 MB is already in RDRAM -- copying it there is the one thing IPL3 does -- and somewhere
     *    in it sits libultra's __osExceptionPreamble, the four instructions osInitialize will
     *    copy onto every exception vector. Rewrite its lui/addiu to compute the engine's address
     *    instead, and the game installs the hook itself; the engine's tail replays the original
     *    two words to reach the real __osException. Nothing here needs the watch exception, which
     *    is the point: two machines out of two available to this project (ares, and the ModRetro
     *    M64 per AUDIT.md 1af) hold the watch register but never deliver the trap, so the Datel
     *    hook below has never once run.
     *
     * 2. The Datel watch hook, kept as the fallback for the scan finding nothing -- a game not
     *    built on libultra, or one whose kernel lives outside the first megabyte.
     *
     * The scan covers [$t1, $t1 + 1 MB): $t1 is where IPL3 was about to jump, which is where it
     * loaded the game. A false match would hijack sixteen bytes that merely look like the
     * preamble -- accepted as unlikely rather than impossible, since the pattern fixes eight of
     * the sixteen bytes and half the rest, and real preambles sit in libultra .text near the
     * front of the image, ahead of any data that could imitate them. */

    io32_t *tail0 = final_engine_address + tail_offset_words;
    io32_t *tail1 = tail0 + 1;

    /* What the top of the preamble becomes: the same two-instruction shape, aimed at the engine. */
    uint32_t hook_w0 = I_LUI(REG_K0, A_BASE((uint32_t)final_engine_address));
    uint32_t hook_w1 = I_ADDIU(REG_K0, REG_K0, A_OFFSET((uint32_t)final_engine_address));

    /* Forward branches are emitted as placeholders and filled in below once their targets
     * exist -- a branch offset counts instructions from its own delay slot, and the distance is
     * not known until the target is emitted. Backpatching beats hand-counted offsets because a
     * hand count silently rots the first time an instruction is added between branch and target. */
    io32_t *to_next[3];
    io32_t *to_found;
    io32_t *to_notfound;
    io32_t *to_join;

    *patcher_p++ = I_OR(REG_T3, REG_T1, REG_ZERO);
    *patcher_p++ = I_LUI(REG_T4, 0x0004); // 0x40000 words = the 1 MB IPL3 just loaded

    io32_t *scan_loop = patcher_p;

    *patcher_p++ = I_LW(REG_K0, 0, REG_T3);
    *patcher_p++ = I_SRL(REG_K0, REG_K0, 16);
    *patcher_p++ = I_ORI(REG_K1, REG_ZERO, PREAMBLE_LUI_K0_HI16);
    to_next[0] = patcher_p;
    *patcher_p++ = I_NOP(); // bne $k0, $k1, next
    *patcher_p++ = I_NOP();

    *patcher_p++ = I_LW(REG_K0, 4, REG_T3);
    *patcher_p++ = I_SRL(REG_K0, REG_K0, 16);
    *patcher_p++ = I_ORI(REG_K1, REG_ZERO, PREAMBLE_ADDIU_K0_HI16);
    to_next[1] = patcher_p;
    *patcher_p++ = I_NOP(); // bne $k0, $k1, next
    *patcher_p++ = I_NOP();

    *patcher_p++ = I_LW(REG_K0, 8, REG_T3);
    *patcher_p++ = I_LUI(REG_K1, I_JR(REG_K0) >> 16);
    *patcher_p++ = I_ORI(REG_K1, REG_K1, I_JR(REG_K0));
    to_next[2] = patcher_p;
    *patcher_p++ = I_NOP(); // bne $k0, $k1, next
    *patcher_p++ = I_NOP();

    *patcher_p++ = I_LW(REG_K0, 12, REG_T3);
    to_found = patcher_p;
    *patcher_p++ = I_NOP(); // beq $k0, $zero, found -- the nop that ends the preamble
    *patcher_p++ = I_NOP();

    io32_t *next_label = patcher_p;
    *patcher_p++ = I_ADDIU(REG_T4, REG_T4, -1);
    io32_t *loop_branch = patcher_p;
    *patcher_p++ = I_BNE(REG_T4, REG_ZERO, scan_loop - (loop_branch + 1));
    *patcher_p++ = I_ADDIU(REG_T3, REG_T3, 4); // delay slot: advances on both paths, harmlessly

    to_notfound = patcher_p;
    *patcher_p++ = I_NOP(); // beq $zero, $zero, notfound -- the scan came up empty
    *patcher_p++ = I_NOP();

    /* found: $t3 holds the game's preamble. Save its first two words into the engine tail (they
     * compute __osException, which is where the engine must hand off), then aim the preamble at
     * the engine. The tail was flushed with the rest of the engine by the loop above, so the two
     * stores here need their own writeback -- and the preamble gets one too, not because anything
     * executes it in place but because sixteen dirty bytes aimed at code the game is about to
     * copy are not worth reasoning about. */
    io32_t *found_label = patcher_p;

    *patcher_p++ = I_LW(REG_K0, 0, REG_T3);
    *patcher_p++ = I_LUI(REG_K1, A_BASE((uint32_t)tail0));
    *patcher_p++ = I_SW(REG_K0, A_OFFSET((uint32_t)tail0), REG_K1);
    *patcher_p++ = I_CACHE(HIT_WRITE_BACK_D, A_OFFSET((uint32_t)tail0), REG_K1);
    *patcher_p++ = I_CACHE(HIT_INVALIDATE_I, A_OFFSET((uint32_t)tail0), REG_K1);

    *patcher_p++ = I_LW(REG_K0, 4, REG_T3);
    *patcher_p++ = I_LUI(REG_K1, A_BASE((uint32_t)tail1));
    *patcher_p++ = I_SW(REG_K0, A_OFFSET((uint32_t)tail1), REG_K1);
    *patcher_p++ = I_CACHE(HIT_WRITE_BACK_D, A_OFFSET((uint32_t)tail1), REG_K1);
    *patcher_p++ = I_CACHE(HIT_INVALIDATE_I, A_OFFSET((uint32_t)tail1), REG_K1);

    *patcher_p++ = I_LUI(REG_K0, hook_w0 >> 16);
    *patcher_p++ = I_ORI(REG_K0, REG_K0, hook_w0);
    *patcher_p++ = I_SW(REG_K0, 0, REG_T3);
    *patcher_p++ = I_LUI(REG_K0, hook_w1 >> 16);
    *patcher_p++ = I_ORI(REG_K0, REG_K0, hook_w1);
    *patcher_p++ = I_SW(REG_K0, 4, REG_T3);
    *patcher_p++ = I_CACHE(HIT_WRITE_BACK_D, 0, REG_T3);
    *patcher_p++ = I_CACHE(HIT_INVALIDATE_I, 0, REG_T3);
    *patcher_p++ = I_CACHE(HIT_WRITE_BACK_D, 4, REG_T3);
    *patcher_p++ = I_CACHE(HIT_INVALIDATE_I, 4, REG_T3);

    /* Green: the game will install our hook for us. The engine paints this if it ever runs. */
    if (beacon_enabled) {
        patcher_p = emit_beacon_stamp(patcher_p, BEACON_GREEN);
    }

    to_join = patcher_p;
    *patcher_p++ = I_NOP(); // beq $zero, $zero, join -- found path arms no watch
    *patcher_p++ = I_NOP();

    /* notfound: the original Datel hook. Write `j engine` over the general exception vector and
     * arm a watch on it, so the game's attempt to install its own handler is what springs the
     * trap. Requires a CPU that actually delivers the watch exception. */
    io32_t *notfound_label = patcher_p;

    *patcher_p++ = I_LUI(REG_K0, A_BASE(EXCEPTION_HANDLER_ADDRESS));
    *patcher_p++ = I_ADDIU(REG_K0, REG_K0, A_OFFSET(EXCEPTION_HANDLER_ADDRESS));

    *patcher_p++ = I_LUI(REG_K1, j_engine_from_handler >> 16);
    *patcher_p++ = I_ORI(REG_K1, REG_K1, j_engine_from_handler);
    *patcher_p++ = I_SW(REG_K1, 0, REG_K0);
    *patcher_p++ = I_SW(REG_ZERO, 4, REG_K0);

    *patcher_p++ = I_CACHE(HIT_WRITE_BACK_D, 0, REG_K0);
    *patcher_p++ = I_CACHE(HIT_INVALIDATE_I, 0, REG_K0);

    // Set watch exception on address 0x80000180
    *patcher_p++ = I_ORI(REG_K1, REG_ZERO, EXCEPTION_HANDLER_ADDRESS | WATCHLO_W);
    *patcher_p++ = I_MTC0(REG_K1, C0_REG_WATCH_LO);
    *patcher_p++ = I_MTC0(REG_ZERO, C0_REG_WATCH_HI);

    /* Red, and it is the more interesting of the two. This path only reaches the engine if the
     * watch exception fires, which is the thing 1af measured as absent on both machines we can
     * ask -- so a red bar is not a fallback working as designed, it is the watch working after
     * all, and it would change what we believe about this console. */
    if (beacon_enabled) {
        patcher_p = emit_beacon_stamp(patcher_p, BEACON_RED);
    }

    // Jump back to the game code
    io32_t *join_label = patcher_p;
    *patcher_p++ = I_JR(REG_T1);
    *patcher_p++ = I_NOP();

    *to_next[0]  = I_BNE(REG_K0, REG_K1, next_label - (to_next[0] + 1));
    *to_next[1]  = I_BNE(REG_K0, REG_K1, next_label - (to_next[1] + 1));
    *to_next[2]  = I_BNE(REG_K0, REG_K1, next_label - (to_next[2] + 1));
    *to_found    = I_BEQ(REG_K0, REG_ZERO, found_label - (to_found + 1));
    *to_notfound = I_BEQ(REG_ZERO, REG_ZERO, notfound_label - (to_notfound + 1));
    *to_join     = I_BEQ(REG_ZERO, REG_ZERO, join_label - (to_join + 1));

    cheats_update_cache(engine_start, engine_p);
    cheats_update_cache(patcher_start, patcher_p);

    return (uint32_t *)(patcher_start);
}

bool cheats_install (cic_type_t cic_type, uint32_t *cheat_list) {
    if (!cheat_list) {
        return false;
    }

    /* Emit first, patch the IPL3 last. The old order patched first, which left a live trap: a
     * cheat list that overflowed the engine bound returned false AFTER the IPL3 in DMEM had been
     * pointed at a half-built patcher, and boot.c's false meant the RDRAM holding that patcher
     * was about to be wiped -- an IPL3 jumping into zeroed memory instead of a game booting
     * without cheats. Emission touches nothing the boot depends on, so failing out of it first
     * costs nothing. */
    if (cheats_emit(cheat_list) == NULL) {
        return false;
    }

    return !cheats_patch_ipl3(cic_type, (io32_t *)(PATCHER_ADDRESS));
}
