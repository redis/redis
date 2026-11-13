/**
 * General-length CRC16/XMODEM selection behind the assembly public entry.
 * Public entry preserves exact 16B/64B fast paths; 8..15B seed=0 uses a
 * bounded-load polynomial path. Other inputs arrive here:
 * <16B scalar, 16..127B short, 128..4095B 4-way, >=4096B wide.
 * The short API delegates >=128B back to this selector, without recursion.
 */

#include <stdint.h>

extern uint16_t crc16_xmodem_scalar(uint16_t seed, const uint8_t *buf, uint64_t len);
extern uint16_t crc16_xmodem_pmull(uint16_t seed, const uint8_t *buf, uint64_t len);
extern uint16_t crc16_xmodem_short(uint16_t seed, const uint8_t *buf, uint64_t len);
extern uint16_t crc16_xmodem_pmull_8way(uint16_t seed, const uint8_t *buf, uint64_t len);
extern uint16_t crc16_xmodem_pmull_8way_neon(uint16_t seed, const uint8_t *buf, uint64_t len);
#ifdef REDIS_CRC16_SVE
extern int crc16_use_sve_wide;
#endif

uint16_t crc16_xmodem_generic(uint16_t seed, const uint8_t *buf, uint64_t len)
{
    if (len >= 128) {
        if (len >= 4096) {
#ifdef REDIS_CRC16_SVE
            /* The SVE kernel requires SVE2 + SVEPMULL and 256-bit VL.
             * Other vector lengths fall back inside the assembly entry. */
            if (len >= 16384 && crc16_use_sve_wide)
                return crc16_xmodem_pmull_8way(seed, buf, len);
#endif
            return crc16_xmodem_pmull_8way_neon(seed, buf, len);
        }
        return crc16_xmodem_pmull(seed, buf, len);
    }
    if (len >= 16)
        return crc16_xmodem_short(seed, buf, len);
    return crc16_xmodem_scalar(seed, buf, len);
}
