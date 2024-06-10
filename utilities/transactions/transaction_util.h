// Copyright (c) 2015, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#pragma once

#ifndef ROCKSDB_LITE

#include <string>
#include <unordered_map>

#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "rocksdb/types.h"

namespace rocksdb {

using TransactionKeyMap =
    std::unordered_map<uint32_t,
                       std::unordered_map<std::string, SequenceNumber>>;

class DBImpl;
struct SuperVersion;
class WriteBatchWithIndex;

class TransactionUtil {
 public:
  // Verifies there have been no writes to this key in the db since this
  // sequence number.
  //
  // If cache_only is true, then this function will not attempt to read any
  // SST files.  This will make it more likely this function will
  // return an error if it is unable to determine if there are any conflicts.
  //
  // Returns OK on success, BUSY if there is a conflicting write, or other error
  // status for any unexpected errors.
  static Status CheckKeyForConflicts(DBImpl* db_impl,
                                     ColumnFamilyHandle* column_family,
                                     const std::string& key,
                                     SequenceNumber key_seq, bool cache_only);

  // For each key,SequenceNumber pair in the TransactionKeyMap, this function
  // will verify there have been no writes to the key in the db since that
  // sequence number.
  //
  // Returns OK on success, BUSY if there is a conflicting write, or other error
  // status for any unexpected errors.
  //
  // REQUIRED: this function should only be called on the write thread or if the
  // mutex is held.
  static Status CheckKeysForConflicts(DBImpl* db_impl,
                                      const TransactionKeyMap& keys,
                                      bool cache_only);

 private:
  static Status CheckKey(DBImpl* db_impl, SuperVersion* sv,
                         SequenceNumber earliest_seq, SequenceNumber key_seq,
                         const std::string& key, bool cache_only);
};

}  // namespace rocksdb

#endif  // ROCKSDB_LITE
