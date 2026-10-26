/*
 * Copyright 2022 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "xmcd.h"

/* Component ID definition, used by tools. */
#ifndef FSL_COMPONENT_ID
#define FSL_COMPONENT_ID "platform.xmcd"
#endif

#if defined(XIP_BOOT_HEADER_ENABLE) && (XIP_BOOT_HEADER_ENABLE == 1)

#if defined(XIP_BOOT_HEADER_XMCD_ENABLE) && (XIP_BOOT_HEADER_XMCD_ENABLE == 1)

#if defined(__CC_ARM) || defined(__ARMCC_VERSION) || defined(__GNUC__)
__attribute__((section(".boot_hdr.xmcd_data"), used))
#elif defined(__ICCARM__)
#pragma location = ".boot_hdr.xmcd_data"
#endif

const uint32_t xmcd_data[] = {
    /* Tag = 0xC, Version = 0, Memory Interface: SEMC */
    0xC010000D,
    /* Magic_number = 0xA1, Version = 1 166MHz */
    0xA60001A1,
    /* SDRAM CS0 size: 16MBytes */
    0x00004000,
    /* Port_size: 16-bit */
    0x01};

#endif /* XIP_BOOT_HEADER_XMCD_ENABLE */
#endif /* XIP_BOOT_HEADER_ENABLE */
