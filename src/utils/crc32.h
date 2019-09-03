// Copyright (c) 2019 EPI-ONE Core Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef __EPIC_CRC32_H__
#define __EPIC_CRC32_H__

#include <cstddef>
#include <cstdint>

uint32_t crc32c(uint8_t* buf, size_t length);

#endif // __EPIC_CRC32_H__
