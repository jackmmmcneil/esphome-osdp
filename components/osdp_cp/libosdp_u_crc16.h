/* Vendored from LibOSDP v3.2.7 (Apache-2.0) by tools/vendor_libosdp.py */
#ifndef __BARE_METAL__
#define __BARE_METAL__
#endif
#include "libosdp_symbols.h"
#ifndef _UTILS_CRC16_H_
#define _UTILS_CRC16_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __ZEPHYR__

#include <zephyr/sys/crc.h>

#else

uint16_t crc16_itu_t(uint16_t seed, const uint8_t *src, size_t len);

#endif

#ifdef __cplusplus
}
#endif

#endif /* _UTILS_CRC16_H_ */
