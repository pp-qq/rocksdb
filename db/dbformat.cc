// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <stdio.h>
#include "db/dbformat.h"
#include "port/port.h"
#include "util/coding.h"

namespace leveldb {

static uint64_t PackSequenceAndType(uint64_t seq, ValueType t) {
  assert(seq <= kMaxSequenceNumber);
  assert(t <= kValueTypeForSeek);
  return (seq << 8) | t;
}

void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key.data(), key.user_key.size());
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

std::string ParsedInternalKey::DebugString() const {
  char buf[50];
  snprintf(buf, sizeof(buf), "' @ %llu : %d",
           (unsigned long long) sequence,
           int(type));
  std::string result = "'";
  result += user_key.ToString();
  result += buf;
  return result;
}

const char* InternalKeyComparator::Name() const {
  return "leveldb.InternalKeyComparator";
}

int InternalKeyComparator::Compare(const Slice& akey, const Slice& bkey) const {
  // Order by:
  //    increasing user key (according to user-supplied comparator)
  //    decreasing sequence number
  //    decreasing type (though sequence# should be enough to disambiguate)
  int r = user_comparator_->Compare(ExtractUserKey(akey), ExtractUserKey(bkey));
  if (r == 0) {
    const uint64_t anum = DecodeFixed64(akey.data() + akey.size() - 8);
    const uint64_t bnum = DecodeFixed64(bkey.data() + bkey.size() - 8);
    // QA: 为啥降序排序呢? 按我理解 sequence 越大表明越新, 倒序排序可以让越新的 key 记录摆在前面.
    // A: 参见 DoCompactionWork(), 基本上就是我理解的那样.
    if (anum > bnum) {
      r = -1;
    } else if (anum < bnum) {
      r = +1;
    }
  }
  return r;
}

void InternalKeyComparator::FindShortestSeparator(
      std::string* start,
      const Slice& limit) const {
  // Attempt to shorten the user portion of the key
  Slice user_start = ExtractUserKey(*start);
  Slice user_limit = ExtractUserKey(limit);
  std::string tmp(user_start.data(), user_start.size());
  user_comparator_->FindShortestSeparator(&tmp, user_limit);
  if (user_comparator_->Compare(*start, tmp) < 0) {
    // 这里应该是 Compare(*user_start, tmp); 最新 rocksdb 已经修复了这个 bug.

    // 按我理解这里 tmp 属于 (user_start, user_limit) 之间, 所以我觉得这里并不一定非得是 kMaxSequenceNumber,
    // kValueTypeForSeek. 因为这里任何 user_key 与 tmp 都不会相同, 即在 InternalKeyComparator::Compare()
    // 时不会试图比较 sequence#.

    // User key has become larger.  Tack on the earliest possible
    // number to the shortened user key.
    PutFixed64(&tmp, PackSequenceAndType(kMaxSequenceNumber,kValueTypeForSeek));
    assert(this->Compare(*start, tmp) < 0);
    assert(this->Compare(tmp, limit) < 0);
    start->swap(tmp);
  }
}

void InternalKeyComparator::FindShortSuccessor(std::string* key) const {
  Slice user_key = ExtractUserKey(*key);
  std::string tmp(user_key.data(), user_key.size());
  user_comparator_->FindShortSuccessor(&tmp);
  if (user_comparator_->Compare(user_key, tmp) < 0) {
    // User key has become larger.  Tack on the earliest possible
    // number to the shortened user key.
    PutFixed64(&tmp, PackSequenceAndType(kMaxSequenceNumber,kValueTypeForSeek));
    assert(this->Compare(*key, tmp) < 0);
    key->swap(tmp);
  }
}

LargeValueRef LargeValueRef::Make(const Slice& value, CompressionType ctype) {
  LargeValueRef result;
  port::SHA1_Hash(value.data(), value.size(), &result.data[0]);
  EncodeFixed64(&result.data[20], value.size());
  result.data[28] = static_cast<unsigned char>(ctype);
  return result;
}

std::string LargeValueRefToFilenameString(const LargeValueRef& h) {
  assert(sizeof(h.data) == LargeValueRef::ByteSize());
  assert(sizeof(h.data) == 29); // So we can hardcode the array size of buf
  static const char tohex[] = "0123456789abcdef";
  char buf[20*2];
  for (int i = 0; i < 20; i++) {
    buf[2*i] = tohex[(h.data[i] >> 4) & 0xf];
    buf[2*i+1] = tohex[h.data[i] & 0xf];
  }
  std::string result = std::string(buf, sizeof(buf));
  result += "-";
  result += NumberToString(h.ValueSize());
  result += "-";
  result += NumberToString(static_cast<uint64_t>(h.compression_type()));
  return result;
}

static uint32_t hexvalue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  } else if (c >= 'A' && c <= 'F') {
    return 10 + c - 'A';
  } else {
    assert(c >= 'a' && c <= 'f');
    return 10 + c - 'a';
  }
}

bool FilenameStringToLargeValueRef(const Slice& s, LargeValueRef* h) {
  Slice in = s;
  if (in.size() < 40) {
    return false;
  }
  for (int i = 0; i < 20; i++) {
    if (!isxdigit(in[i*2]) || !isxdigit(in[i*2+1])) {
      return false;
    }
    unsigned char c = (hexvalue(in[i*2])<<4) | hexvalue(in[i*2+1]);
    h->data[i] = c;
  }
  in.remove_prefix(40);
  uint64_t value_size, ctype;

  if (ConsumeChar(&in, '-') &&
      ConsumeDecimalNumber(&in, &value_size) &&
      ConsumeChar(&in, '-') &&
      ConsumeDecimalNumber(&in, &ctype) &&
      in.empty() &&
      // 如果以后新增一种压缩类型, 导致 kLightweightCompression 不是最大的了, 那么不知道要修改多少地方呢?
      // 这里应该使用类似 IsValidCompressionType() 来判断的.
      (ctype <= kLightweightCompression)) {
    EncodeFixed64(&h->data[20], value_size);
    h->data[28] = static_cast<unsigned char>(ctype);
    return true;
  } else {
    return false;
  }
}

}
