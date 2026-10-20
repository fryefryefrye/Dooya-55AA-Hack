/*
 * Dooya 55AA TTL protocol - CRC-16 reference implementation
 *
 * Parameters (reverse engineered from captures of the official DC1545W adapter):
 *
 *   width    16
 *   poly     0x8005
 *   init     0xFFFF
 *   refin    false      <- input bytes are NOT reflected (MSB first)
 *   refout   false      <- register is NOT reflected
 *   xorout   0x0000
 *   check    0xAEE7     <- CRC of "123456789"
 *
 * This is the parameter set known as CRC-16/CMS.
 *
 * It is NOT CRC-16/MODBUS. Same polynomial, but MODBUS reflects both the
 * input bytes and the register, and a Modbus RTU frame stores the CRC low
 * byte first. The 55AA frame stores it HIGH byte first. Using a Modbus CRC
 * routine here produces a frame the motor silently ignores.
 *
 * Covered bytes: from 0x55 up to and including the last data byte.
 */

#ifndef DOOYA_CRC_H
#define DOOYA_CRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CRC-16 over `len` bytes, MSB first, no reflection. */
uint16_t dooya_crc16(const uint8_t *data, size_t len);

/*
 * Finish a frame that is already in `buf`:
 *   buf[0 .. bodyLen-1]        frame body (0x55 0xAA ... last data byte)
 *   buf[bodyLen .. bodyLen+1]  two placeholder bytes, overwritten with the CRC
 *
 * Stores the CRC high byte first. Returns the total frame length
 * (bodyLen + 2), or 0 if the buffer is too small or a pointer is NULL.
 */
size_t dooya_frame_finish(uint8_t *buf, size_t bodyLen, size_t bufSize);

#ifdef __cplusplus
}
#endif

#endif /* DOOYA_CRC_H */
