// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Endian-neutral encoding:
// * Fixed-length numbers are encoded with least-significant byte first. 即小端模式.
//
// Q: 为啥不采用网络字节序? 这样在 EncodeFixed32/64() 时还可以使用库函数 ntohl() 等能提高点效率.
//
// * In addition we support variable length "varint" encoding. 编码规则类似 utf-8, 具体参见代码.
// 这里针对 uint32 的 varint encoding 与 uint64 的 varint encoding 兼容, 即总是可以使用 EncodeVarint64()
// 来替代 EncodeVarint32(), 总是可以使用 GetVarint64() 来替代 GetVarint32().
// 这里的 fixed-length encoding 与 varint encoding 与 golang encoding/binary 兼容.
//
//
// * Strings are encoded prefixed by their length in varint format

#ifndef STORAGE_LEVELDB_UTIL_CODING_H_
#define STORAGE_LEVELDB_UTIL_CODING_H_

#include <stdint.h>
#include <string.h>
#include <string>
#include "include/slice.h"
#include "port/port.h"

namespace leveldb {

// Standard Put... routines append to a string
extern void PutFixed32(std::string* dst, uint32_t value);
extern void PutFixed64(std::string* dst, uint64_t value);
extern void PutVarint32(std::string* dst, uint32_t value);
extern void PutVarint64(std::string* dst, uint64_t value);
extern void PutLengthPrefixedSlice(std::string* dst, const Slice& value);

// Standard Get... routines parse a value from the beginning of a Slice
// and advance the slice past the parsed value.
extern bool GetVarint32(Slice* input, uint32_t* value);
extern bool GetVarint64(Slice* input, uint64_t* value);
extern bool GetLengthPrefixedSlice(Slice* input, Slice* result);

// Pointer-based variants of GetVarint...  These either store a value
// in *v and return a pointer just past the parsed value, or return
// NULL on error.  These routines only look at bytes in the range
// [p..limit-1]
extern const char* GetVarint32Ptr(const char* p,const char* limit, uint32_t* v);
extern const char* GetVarint64Ptr(const char* p,const char* limit, uint64_t* v);

// Returns the length of the varint32 or varint64 encoding of "v"
extern int VarintLength(uint64_t v);

// Lower-level versions of Put... that write directly into a character buffer
// REQUIRES: dst has enough space for the value being written
extern void EncodeFixed32(char* dst, uint32_t value);
extern void EncodeFixed64(char* dst, uint64_t value);

// Lower-level versions of Put... that write directly into a character buffer
// and return a pointer just past the last byte written.
// REQUIRES: dst has enough space for the value being written
extern char* EncodeVarint32(char* dst, uint32_t value);
extern char* EncodeVarint64(char* dst, uint64_t value);

// Lower-level versions of Get... that read directly from a character buffer
// without any bounds checking.

inline uint32_t DecodeFixed32(const char* ptr) {
  if (port::kLittleEndian) {
    // Load the raw bytes
    uint32_t result;
    memcpy(&result, ptr, sizeof(result));  // gcc optimizes this to a plain load
    return result;
  } else {
    return ((static_cast<uint32_t>(ptr[0]))
            | (static_cast<uint32_t>(ptr[1]) << 8)
            | (static_cast<uint32_t>(ptr[2]) << 16)
            | (static_cast<uint32_t>(ptr[3]) << 24));
  }
  // 按我理解这里可以优化如下:
  // memcpy(&result, ...)
  // if (!port::kLittleEndian) {
  //    bswap result // 即使用 cpu 指令来交换字节序.
  // }
  // 根据 gcc optimizes memcpy to a plain load 的说法, 这里大概需要 3 条指令左右的样子.
}

inline uint64_t DecodeFixed64(const char* ptr) {
  if (port::kLittleEndian) {
    // Load the raw bytes
    uint64_t result;
    memcpy(&result, ptr, sizeof(result));  // gcc optimizes this to a plain load
    return result;
  } else {
    uint64_t lo = DecodeFixed32(ptr);
    uint64_t hi = DecodeFixed32(ptr + 4);
    return (hi << 32) | lo;
  }
}

// Internal routine for use by fallback path of GetVarint32Ptr
extern const char* GetVarint32PtrFallback(const char* p,
                                          const char* limit,
                                          uint32_t* value);
inline const char* GetVarint32Ptr(const char* p,
                                  const char* limit,
                                  uint32_t* value) {
  // 直接 return GetVarint32PtrFallback(p, limit, value); 是可以的.
  // 这里加这么多戏是图啥呢? 按我理解直接把 GetVarint32PtrFallback() 的实现放在这里就行了.
  if (p < limit) {
    uint32_t result = *(reinterpret_cast<const unsigned char*>(p));
    if ((result & 128) == 0) {
      *value = result;
      return p + 1;
    }
  }
  return GetVarint32PtrFallback(p, limit, value);
}

}

#endif  // STORAGE_LEVELDB_UTIL_CODING_H_
