// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// We recover the contents of the descriptor from the other files we find.
// 这里的恢复思路倒是值得参考. 以及将 repair 过程中出错的文件移到 lost 目录下也值得借鉴.
// (1) Any log files are first converted to tables
// (2) We scan every table to compute
//     (a) smallest/largest for the table
//     (b) large value refs from the table
//     (c) largest sequence number in the table
// (3) We generate descriptor contents:
//      - log number is set to zero
//      - next-file-number is set to 1 + largest file number we found
//      - last-sequence-number is set to largest sequence# found across
//        all tables (see 2c)
//      - compaction pointers are cleared
//      - every table file is added at level 0
//
// Possible optimization 1:
//   (a) Compute total size and use to pick appropriate max-level M
//   (b) Sort tables by largest sequence# in the table
//   (c) For each table: if it overlaps earlier table, place in level-0,
//       else place in level-M.
//   这个我倒觉得没啥必要, 就都放在 level0 让他慢慢 compact 呗, 感觉挺好玩的==
// Possible optimization 2:
//   Store per-table metadata (smallest, largest, largest-seq#,
//   large-value-refs, ...) in the table's meta section to speed up
//   ScanTable.
//
// Q: 如果在 leveldb 执行 BackgroundCompaction() 期间 kill leveldb, 那么可能会导致 (ukey, seq) 同时存在与
// (lvlN, fileno.sst), (lvlN+1, fileno.sst) 两个 sst 文件中. 如果这时候在执行 RepairDB(), 就会导致
// RepairDB() 之后的 leveldb 存在相同的 (ukey, seq). 此时会导致一些问题, 比如 MergingIterator 在元素可能会
// 重复的情况会出问题; 使用 DB::NewIterator() 遍历 leveldb 时可能会吐出相同的 ukey 等. 所以在 RepairDB() 之后
// 执行一次 Compaction 是不是会好点?
//

#include "db/builder.h"
#include "db/db_impl.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "db/write_batch_internal.h"
#include "include/comparator.h"
#include "include/db.h"
#include "include/env.h"

namespace leveldb {

namespace {

class Repairer {
 public:
  Repairer(const std::string& dbname, const Options& options)
      : dbname_(dbname),
        env_(options.env),
        icmp_(options.comparator),
        options_(SanitizeOptions(dbname, &icmp_, options)),
        owns_info_log_(options_.info_log != options.info_log),
        next_file_number_(1) {
    // TableCache can be small since we expect each table to be opened once.
    // 期望之中, 任何一个 table file 不会被打开两次以上, 所以 table cache 中的项几乎不会被命中, 因此这里将
    // cache 大小设置地很小.
    table_cache_ = new TableCache(dbname_, &options_, 10);
  }

  ~Repairer() {
    delete table_cache_;
    if (owns_info_log_) {
      delete options_.info_log;
    }
  }

  Status Run() {
    Status status = FindFiles();
    if (status.ok()) {
      // 这里的步骤与上面的 (1), (2), (3) 一一对应啊.
      ConvertLogFilesToTables();
      ExtractMetaData();
      status = WriteDescriptor();
    }
    if (status.ok()) {
      unsigned long long bytes = 0;
      for (int i = 0; i < tables_.size(); i++) {
        bytes += tables_[i].meta.file_size;
      }
      Log(env_, options_.info_log,
          "**** Repaired leveldb %s; "
          "recovered %d files; %llu bytes. "
          "Some data may have been lost. "
          "****",
          dbname_.c_str(),
          static_cast<int>(tables_.size()),
          bytes);
    }
    return status;
  }

 private:
  struct TableInfo {
    FileMetaData meta;
    SequenceNumber max_sequence;
  };

  std::string const dbname_;
  Env* const env_;
  InternalKeyComparator const icmp_;
  Options const options_;
  bool owns_info_log_;
  TableCache* table_cache_;
  VersionEdit edit_;  // 存放着 repair 的过程中, 所有对 server state 的变更.

  // 存放着在 FindFiles() 过程中发现的 descriptor 文件, 所有这些文件都会被移动 lost 目录下.
  std::vector<std::string> manifests_;
  std::vector<uint64_t> table_numbers_;
  std::vector<uint64_t> logs_;
  std::vector<TableInfo> tables_;
  uint64_t next_file_number_;

  Status FindFiles() {
    std::vector<std::string> filenames;
    Status status = env_->GetChildren(dbname_, &filenames);
    if (!status.ok()) {
      return status;
    }
    if (filenames.empty()) {
      return Status::IOError(dbname_, "repair found no files");
    }

    uint64_t number;
    LargeValueRef large_ref;
    FileType type;
    for (int i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &large_ref, &type)) {
        if (type == kLargeValueFile) {
          // Will be picked up when we process a Table that points to it
        } else if (type == kDescriptorFile) {
          manifests_.push_back(filenames[i]);
        } else {
          if (number + 1 > next_file_number_) {
            next_file_number_ = number + 1;
          }
          if (type == kLogFile) {
            logs_.push_back(number);
          } else if (type == kTableFile) {
            table_numbers_.push_back(number);
          } else {
            // Ignore other files
          }
        }
      }
    }
    return status;
  }

  void ConvertLogFilesToTables() {
    for (int i = 0; i < logs_.size(); i++) {
      std::string logname = LogFileName(dbname_, logs_[i]);
      Status status = ConvertLogToTable(logs_[i]);
      if (!status.ok()) {
        Log(env_, options_.info_log, "Log #%llu: ignoring conversion error: %s",
            (unsigned long long) logs_[i],
            status.ToString().c_str());
      }
      ArchiveFile(logname);  // 真谨慎.
    }
  }

  Status ConvertLogToTable(uint64_t log) {
    struct LogReporter : public log::Reader::Reporter {
      Env* env;
      WritableFile* info_log;
      uint64_t lognum;
      virtual void Corruption(size_t bytes, const Status& s) {
        // We print error messages for corruption, but continue repairing.
        Log(env, info_log, "Log #%llu: dropping %d bytes; %s",
            (unsigned long long) lognum,
            static_cast<int>(bytes),
            s.ToString().c_str());
      }
    };

    // Open the log file
    std::string logname = LogFileName(dbname_, log);
    SequentialFile* lfile;
    Status status = env_->NewSequentialFile(logname, &lfile);
    if (!status.ok()) {
      return status;
    }

    // Create the log reader.
    LogReporter reporter;
    reporter.env = env_;
    reporter.info_log = options_.info_log;
    reporter.lognum = log;
    // We intentially make log::Reader do checksumming so that
    // corruptions cause entire commits to be skipped instead of
    // propagating bad information (like overly large sequence
    // numbers).
    log::Reader reader(lfile, &reporter, false/*do not checksum*/);

    // Read all the records and add to a memtable
    std::string scratch;
    Slice record;
    WriteBatch batch;
    MemTable mem(icmp_);
    int counter = 0;
    while (reader.ReadRecord(&record, &scratch)) {
      if (record.size() < 12) {
        reporter.Corruption(
            record.size(), Status::Corruption("log record too small"));
        continue;
      }
      WriteBatchInternal::SetContents(&batch, record);
      status = WriteBatchInternal::InsertInto(&batch, &mem);
      if (status.ok()) {
        counter += WriteBatchInternal::Count(&batch);
      } else {
        Log(env_, options_.info_log, "Log #%llu: ignoring %s",
            (unsigned long long) log,
            status.ToString().c_str());
        status = Status::OK();  // Keep going with rest of file
      }
    }
    delete lfile;

    // We ignore any version edits generated by the conversion to a Table
    // since ExtractMetaData() will also generate edits.
    VersionEdit skipped;
    FileMetaData meta;
    meta.number = next_file_number_++;
    Iterator* iter = mem.NewIterator();
    status = BuildTable(dbname_, env_, options_, table_cache_, iter,
                        &meta, &skipped);
    delete iter;
    if (status.ok()) {
      if (meta.file_size > 0) {
        table_numbers_.push_back(meta.number);
      }
    }
    Log(env_, options_.info_log, "Log #%llu: %d ops saved to Table #%llu %s",
        (unsigned long long) log,
        counter,
        (unsigned long long) meta.number,
        status.ToString().c_str());
    return status;
  }

  void ExtractMetaData() {
    std::vector<TableInfo> kept;
    for (int i = 0; i < table_numbers_.size(); i++) {
      TableInfo t;
      t.meta.number = table_numbers_[i];
      Status status = ScanTable(&t);
      if (!status.ok()) {
        std::string fname = TableFileName(dbname_, table_numbers_[i]);
        Log(env_, options_.info_log, "Table #%llu: ignoring %s",
            (unsigned long long) table_numbers_[i],
            status.ToString().c_str());
        ArchiveFile(fname);
      } else {
        tables_.push_back(t);
      }
    }
  }

  Status ScanTable(TableInfo* t) {
    std::string fname = TableFileName(dbname_, t->meta.number);
    int counter = 0;
    Status status = env_->GetFileSize(fname, &t->meta.file_size);
    if (status.ok()) {
      Iterator* iter = table_cache_->NewIterator(
          ReadOptions(), t->meta.number);
      bool empty = true;  // 通过 counter 也可以判断出来啊~
      ParsedInternalKey parsed;
      t->max_sequence = 0;
      for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
        Slice key = iter->key();
        if (!ParseInternalKey(key, &parsed)) {
          Log(env_, options_.info_log, "Table #%llu: unparsable key %s",
              (unsigned long long) t->meta.number,
              EscapeString(key).c_str());
          continue;
        }

        counter++;
        if (empty) {
          empty = false;
          t->meta.smallest.DecodeFrom(key);
        }
        t->meta.largest.DecodeFrom(key);
        if (parsed.sequence > t->max_sequence) {
          t->max_sequence = parsed.sequence;
        }

        if (ExtractValueType(key) == kTypeLargeValueRef) {
          if (iter->value().size() != LargeValueRef::ByteSize()) {
            Log(env_, options_.info_log, "Table #%llu: bad large value ref",
                (unsigned long long) t->meta.number);
          } else {
            edit_.AddLargeValueRef(LargeValueRef::FromRef(iter->value()),
                                   t->meta.number,
                                   key);
          }
        }
      }
      if (!iter->status().ok()) {
        status = iter->status();
      }
      delete iter;
    }
    Log(env_, options_.info_log, "Table #%llu: %d entries %s",
        (unsigned long long) t->meta.number,
        counter,
        status.ToString().c_str());
    return status;
  }

  Status WriteDescriptor() {
    std::string tmp = TempFileName(dbname_, 1);
    WritableFile* file;
    Status status = env_->NewWritableFile(tmp, &file);
    if (!status.ok()) {
      return status;
    }

    SequenceNumber max_sequence = 0;
    for (int i = 0; i < tables_.size(); i++) {
      if (max_sequence < tables_[i].max_sequence) {
        max_sequence = tables_[i].max_sequence;
      }
    }

    edit_.SetComparatorName(icmp_.user_comparator()->Name());
    edit_.SetLogNumber(0);
    edit_.SetNextFile(next_file_number_);
    edit_.SetLastSequence(max_sequence);

    for (int i = 0; i < tables_.size(); i++) {
      // TODO(opt): separate out into multiple levels
      const TableInfo& t = tables_[i];
      edit_.AddFile(0, t.meta.number, t.meta.file_size,
                    t.meta.smallest, t.meta.largest);
    }

    //fprintf(stderr, "NewDescriptor:\n%s\n", edit_.DebugString().c_str());
    {
      log::Writer log(file);
      std::string record;
      edit_.EncodeTo(&record);
      status = log.AddRecord(record);
    }
    if (status.ok()) {
      status = file->Close();
    }
    delete file;
    file = NULL;

    if (!status.ok()) {
      env_->DeleteFile(tmp);
    } else {
      // Discard older manifests
      for (int i = 0; i < manifests_.size(); i++) {
        ArchiveFile(dbname_ + "/" + manifests_[i]);
      }

      // Install new manifest
      status = env_->RenameFile(tmp, DescriptorFileName(dbname_, 1));
      if (status.ok()) {
        status = SetCurrentFile(env_, dbname_, 1);
      } else {
        env_->DeleteFile(tmp);
      }
    }
    return status;
  }

  void ArchiveFile(const std::string& fname) {
    // Move into another directory.  E.g., for
    //    dir/foo
    // rename to
    //    dir/lost/foo
    const char* slash = strrchr(fname.c_str(), '/');
    std::string new_dir;
    if (slash != NULL) {
      new_dir.assign(fname.data(), slash - fname.data());
    }
    new_dir.append("/lost");
    env_->CreateDir(new_dir);  // Ignore error
    std::string new_file = new_dir;
    new_file.append("/");
    new_file.append((slash == NULL) ? fname.c_str() : slash + 1);
    Status s = env_->RenameFile(fname, new_file);
    Log(env_, options_.info_log, "Archiving %s: %s\n",
        fname.c_str(), s.ToString().c_str());
  }
};
}

Status RepairDB(const std::string& dbname, const Options& options) {
  Repairer repairer(dbname, options);
  return repairer.Run();
}

}
