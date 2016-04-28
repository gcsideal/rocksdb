//  Copyright (c) 2015, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include "rocksdb/sst_file_writer.h"

#include <vector>
#include "db/dbformat.h"
#include "rocksdb/table.h"
#include "table/block_based_table_builder.h"
#include "util/file_reader_writer.h"
#include "util/string_util.h"

namespace rocksdb {

const std::string ExternalSstFilePropertyNames::kVersion =
    "rocksdb.external_sst_file.version";

// PropertiesCollector used to add properties specific to tables
// generated by SstFileWriter
class SstFileWriter::SstFileWriterPropertiesCollector
    : public IntTblPropCollector {
 public:
  explicit SstFileWriterPropertiesCollector(int32_t version)
      : version_(version) {}

  virtual Status InternalAdd(const Slice& key, const Slice& value,
                             uint64_t file_size) override {
    // Intentionally left blank. Have no interest in collecting stats for
    // individual key/value pairs.
    return Status::OK();
  }

  virtual Status Finish(UserCollectedProperties* properties) override {
    std::string version_val;
    PutFixed32(&version_val, static_cast<int32_t>(version_));
    properties->insert({ExternalSstFilePropertyNames::kVersion, version_val});
    return Status::OK();
  }

  virtual const char* Name() const override {
    return "SstFileWriterPropertiesCollector";
  }

  virtual UserCollectedProperties GetReadableProperties() const override {
    return {{ExternalSstFilePropertyNames::kVersion, ToString(version_)}};
  }

 private:
  int32_t version_;
};

class SstFileWriter::SstFileWriterPropertiesCollectorFactory
    : public IntTblPropCollectorFactory {
 public:
  explicit SstFileWriterPropertiesCollectorFactory(int32_t version)
      : version_(version) {}

  virtual IntTblPropCollector* CreateIntTblPropCollector(
      uint32_t column_family_id) override {
    return new SstFileWriterPropertiesCollector(version_);
  }

  virtual const char* Name() const override {
    return "SstFileWriterPropertiesCollector";
  }

 private:
  int32_t version_;
};

struct SstFileWriter::Rep {
  Rep(const EnvOptions& _env_options, const ImmutableCFOptions& _ioptions,
      const Comparator* _user_comparator)
      : env_options(_env_options),
        ioptions(_ioptions),
        internal_comparator(_user_comparator) {}

  std::unique_ptr<WritableFileWriter> file_writer;
  std::unique_ptr<TableBuilder> builder;
  EnvOptions env_options;
  ImmutableCFOptions ioptions;
  InternalKeyComparator internal_comparator;
  ExternalSstFileInfo file_info;
};

SstFileWriter::SstFileWriter(const EnvOptions& env_options,
                             const ImmutableCFOptions& ioptions,
                             const Comparator* user_comparator)
    : rep_(new Rep(env_options, ioptions, user_comparator)) {}

SstFileWriter::~SstFileWriter() { delete rep_; }

Status SstFileWriter::Open(const std::string& file_path) {
  Rep* r = rep_;
  Status s;
  std::unique_ptr<WritableFile> sst_file;
  s = r->ioptions.env->NewWritableFile(file_path, &sst_file, r->env_options);
  if (!s.ok()) {
    return s;
  }

  CompressionType compression_type = r->ioptions.compression;
  if (!r->ioptions.compression_per_level.empty()) {
    // Use the compression of the last level if we have per level compression
    compression_type = *(r->ioptions.compression_per_level.rbegin());
  }

  std::vector<std::unique_ptr<IntTblPropCollectorFactory>>
      int_tbl_prop_collector_factories;
  int_tbl_prop_collector_factories.emplace_back(
      new SstFileWriterPropertiesCollectorFactory(1 /* version */));

  TableBuilderOptions table_builder_options(
      r->ioptions, r->internal_comparator, &int_tbl_prop_collector_factories,
      compression_type, r->ioptions.compression_opts, false);
  r->file_writer.reset(
      new WritableFileWriter(std::move(sst_file), r->env_options));
  r->builder.reset(r->ioptions.table_factory->NewTableBuilder(
      table_builder_options,
      TablePropertiesCollectorFactory::Context::kUnknownColumnFamily,
      r->file_writer.get()));

  r->file_info.file_path = file_path;
  r->file_info.file_size = 0;
  r->file_info.num_entries = 0;
  r->file_info.sequence_number = 0;
  r->file_info.version = 1;
  return s;
}

Status SstFileWriter::Add(const Slice& user_key, const Slice& value) {
  Rep* r = rep_;
  if (!r->builder) {
    return Status::InvalidArgument("File is not opened");
  }

  if (r->file_info.num_entries == 0) {
    r->file_info.smallest_key = user_key.ToString();
  } else {
    if (r->internal_comparator.user_comparator()->Compare(
            user_key, r->file_info.largest_key) <= 0) {
      // Make sure that keys are added in order
      return Status::InvalidArgument("Keys must be added in order");
    }
  }

  // update file info
  r->file_info.num_entries++;
  r->file_info.largest_key = user_key.ToString();
  r->file_info.file_size = r->builder->FileSize();

  InternalKey ikey(user_key, 0 /* Sequence Number */,
                   ValueType::kTypeValue /* Put */);
  r->builder->Add(ikey.Encode(), value);

  return Status::OK();
}

Status SstFileWriter::Finish(ExternalSstFileInfo* file_info) {
  Rep* r = rep_;
  if (!r->builder) {
    return Status::InvalidArgument("File is not opened");
  }
  if (r->file_info.num_entries == 0) {
    return Status::InvalidArgument("Cannot create sst file with no entries");
  }

  Status s = r->builder->Finish();
  if (s.ok()) {
    if (!r->ioptions.disable_data_sync) {
      s = r->file_writer->Sync(r->ioptions.use_fsync);
    }
    if (s.ok()) {
      s = r->file_writer->Close();
    }
  } else {
    r->builder->Abandon();
  }

  if (!s.ok()) {
    r->ioptions.env->DeleteFile(r->file_info.file_path);
  }

  if (s.ok() && file_info != nullptr) {
    r->file_info.file_size = r->builder->FileSize();
    *file_info = r->file_info;
  }

  r->builder.reset();
  return s;
}
}  // namespace rocksdb
