//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/log_writer.h"

#include <stdint.h>

#include <iostream>

#include "db/column_family.h"
#include "db/version_edit.h"
#include "db/version_set.h"
#include "options/db_options.h"
#include "rocksdb/env.h"
#include "table/get_context.h"
#include "terark/util/crc.hpp"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/file_reader_writer.h"

namespace rocksdb {
namespace log {

Writer::Writer(std::unique_ptr<WritableFileWriter>&& dest, uint64_t log_number,
               bool recycle_log_files, VersionSet* vs, bool manual_flush)
    : dest_(std::move(dest)),
      block_offset_(0),
      num_entries_(0),
      block_counts_(0),
      log_number_(log_number),
      recycle_log_files_(recycle_log_files),
      manual_flush_(manual_flush),
      vs_(vs) {
  for (int i = 0; i <= kMaxRecordType; i++) {
    char t = static_cast<char>(i);
    type_crc_[i] = crc32c::Value(&t, 1);
  }
}

Writer::Writer(std::unique_ptr<WritableFileWriter>&& dest, uint64_t log_number,
               bool recycle_log_files, bool manual_flush)
    : dest_(std::move(dest)),
      block_offset_(0),
      num_entries_(0),
      block_counts_(0),
      log_number_(log_number),
      recycle_log_files_(recycle_log_files),
      manual_flush_(manual_flush) {
  for (int i = 0; i <= kMaxRecordType; i++) {
    char t = static_cast<char>(i);
    type_crc_[i] = crc32c::Value(&t, 1);
  }
}

Writer::~Writer() { WriteBuffer(); }

Status Writer::WriteBuffer() {
  auto s = dest_->Flush();
  if (!s.ok()) {
    return s;
  }
  return s;
}

Status Writer::AddRecord(const Slice& slice, size_t num_entries,
                         WriteThread::Writer* wt) {
  const char* ptr = slice.data();
  size_t left = slice.size();

  assert(recycle_log_files_ == 0);  // Forbiden
  // Header size varies depending on whether we are recycling or not.
  const int header_size =
      recycle_log_files_ ? kRecyclableHeaderSize : kHeaderSize;
  if (wt) {
    wt->is_recycle = recycle_log_files_ ? true : false;
  }

  // Fragment the record if necessary and emit it.  Note that if slice
  // is empty, we still want to iterate once to emit a single
  // zero-length record
  Status s;
  bool begin = true;
  do {
    const int64_t leftover = kBlockSize - block_offset_;
    assert(leftover >= 0);
    if (leftover < header_size) {
      // Switch to a new block
      if (leftover > 0) {
        // Fill the trailer (literal below relies on kHeaderSize and
        // kRecyclableHeaderSize being <= 11)
        assert(header_size <= 11);
        s = dest_->Append(Slice("\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",
                                static_cast<size_t>(leftover)));
        if (!s.ok()) {
          break;
        }
      }
      block_offset_ = 0;
      block_counts_++;
    }

    // Invariant: we never leave < header_size bytes in a block.
    assert(static_cast<int64_t>(kBlockSize - block_offset_) >= header_size);

    const size_t avail = kBlockSize - block_offset_ - header_size;
    const size_t fragment_length = (left < avail) ? left : avail;

    RecordType type;
    const bool end = (left == fragment_length);
    if (begin && end) {
      type = recycle_log_files_ ? kRecyclableFullType : kFullType;
    } else if (begin) {
      type = recycle_log_files_ ? kRecyclableFirstType : kFirstType;
    } else if (end) {
      type = recycle_log_files_ ? kRecyclableLastType : kLastType;
    } else {
      type = recycle_log_files_ ? kRecyclableMiddleType : kMiddleType;
    }

    if (wt != nullptr && wt->wal_offset_of_wb_content_ == uint64_t(-1)) {
      // writebatch's content and set writebatch_content_offset only once.
      assert(dest_->GetFileSize() ==
             block_counts_ * kBlockSize + block_offset_);
      wt->wal_offset_of_wb_content_ =
          GetFirstEntryPhysicalOffset(dest_->GetFileSize(), header_size, avail);
      assert(wt->wal_offset_of_wb_content_ % kBlockSize >=
             (uint64_t)header_size);
    }
    s = EmitPhysicalRecord(type, ptr, fragment_length);
    ptr += fragment_length;
    left -= fragment_length;
    begin = false;
  } while (s.ok() && left > 0);
  num_entries_ += num_entries;
  return s;
}

bool Writer::TEST_BufferIsEmpty() { return dest_->TEST_BufferIsEmpty(); }

Status Writer::EmitPhysicalRecord(RecordType t, const char* ptr, size_t n) {
  assert(n <= 0xffff);  // Must fit in two bytes

  size_t header_size;
  char buf[kRecyclableHeaderSize];

  // Format the header
  buf[4] = static_cast<char>(n & 0xff);
  buf[5] = static_cast<char>(n >> 8);
  buf[6] = static_cast<char>(t);

  uint32_t crc = type_crc_[t];
  if (t < kRecyclableFullType) {
    // Legacy record format
    assert(block_offset_ + kHeaderSize + n <= kBlockSize);
    header_size = kHeaderSize;
  } else {
    // Recyclable record format
    assert(block_offset_ + kRecyclableHeaderSize + n <= kBlockSize);
    header_size = kRecyclableHeaderSize;

    // Only encode low 32-bits of the 64-bit log number.  This means
    // we will fail to detect an old record if we recycled a log from
    // ~4 billion logs ago, but that is effectively impossible, and
    // even if it were we'dbe far more likely to see a false positive
    // on the 32-bit CRC.
    EncodeFixed32(buf + 7, static_cast<uint32_t>(log_number_));
    crc = crc32c::Extend(crc, buf + 7, 4);
  }

  // Compute the crc of the record type and the payload.
  crc = crc32c::Extend(crc, ptr, n);
  crc = crc32c::Mask(crc);  // Adjust for storage
  EncodeFixed32(buf, crc);

  // Write the header and the payload
  Status s = dest_->Append(Slice(buf, header_size));
  if (s.ok()) {
    s = dest_->Append(Slice(ptr, n));
    if (s.ok()) {
      if (!manual_flush_) {
        s = dest_->Flush();
      }
    }
  }
  block_offset_ += header_size + n;
  return s;
}

WalBlobReader::WalBlobReader(std::unique_ptr<RandomAccessFile>&& src,
                             uint64_t log_no, const ImmutableDBOptions& idbo,
                             const EnvOptions& eo)
    : blob_cache_(idbo.blob_cache),
      wal_header_size_(idbo.recycle_log_file_num > 0 ? kRecyclableHeaderSize
                                                     : kHeaderSize),
      log_number_(log_no),
      src_(std::move(src)),
      ioptions_(idbo),
      env_options_(eo) {}

class Blob {
 public:
  explicit Blob(size_t capacity) : size_(capacity) {
    buf_ = (char*)malloc(capacity);
    slice_ = Slice(buf_, 0);
  }
  ~Blob() {
    free(buf_);
    buf_ = nullptr;
    size_ = 0;
  }

  void ShrinkVal(size_t head_size, size_t record_header_size) {
    assert(head_size != 0 && size_ == slice_.size() && buf_ == slice_.data());
    size_t kBlockAvailSize = kBlockSize - record_header_size;

    char* cur_offset = buf_ + head_size;
    size_t remain_bytes = size_ - head_size;
    while (remain_bytes > 0) {
      size_t cur_size =
          remain_bytes > kBlockAvailSize ? kBlockAvailSize : remain_bytes;
      char* actual_data = cur_offset + record_header_size;
      size_t actual_size = cur_size - record_header_size;
      memmove(cur_offset, actual_data, actual_size);

      cur_offset += actual_size;
      remain_bytes -= cur_size;
    }

    size_ = cur_offset - buf_;
    // buf_ = (char*)realloc(buf_, size_); // ? realloc may allocating new mem
    slice_ = Slice(buf_, size_);  // tailing unused space just wasted
  }

  uint64_t DataSize() { return size_; }

  Slice slice_;
  char* buf_ = nullptr;
  uint64_t size_;

 private:
  // No copying allowed
  Blob(const Blob&) = delete;
  void operator=(const Blob&) = delete;
};

// Delete the entry resided in the cache.
template <class Entry>
void DeleteCachedEntry(const Slice& /*key*/, void* Blob) {
  auto entry = reinterpret_cast<Entry*>(Blob);
  delete entry;
}

// generate an id from the file
void WalBlobReader::GenerateCacheUniqueId(const Slice& log_handle,
                                          std::string& uid) const {
  char prefix_id[kMaxCacheKeyPrefixSize];
  size_t prefix_length =
      src_->GetUniqueId(&prefix_id[0], kMaxCacheKeyPrefixSize);

  uid.append(&prefix_id[0], prefix_length);
  assert(prefix_length == uid.size() &&
         memcmp(uid.data(), prefix_id, prefix_length) == 0);
  uid.append(log_handle.data(), log_handle.size());
}

Status WalBlobReader::GetBlob(const Slice& log_handle,
                              LazyBuffer* lazy_blob) const {
  assert(lazy_blob);
  auto release_cache = [](void* c, void* h) {
    static_cast<Cache*>(c)->Release(static_cast<Cache::Handle*>(h), false);
  };
  // checkout blob_cache
  assert(log_handle.size() == sizeof(DefaultLogHandle));
  std::string blob_uid;
  GenerateCacheUniqueId(log_handle, blob_uid);
  Cache::Handle* handle = blob_cache_->Lookup(blob_uid);
  if (handle) {
    Blob* b = (Blob*)blob_cache_->Value(handle);
    lazy_blob->reset(b->slice_,
                     Cleanable(release_cache, blob_cache_.get(), handle),
                     log_number_);
    return Status::OK();
  }

  // decode and calculate sizes
  DefaultLogHandle content(log_handle);
  size_t blob_physical_length =
      GetPhysicalLength(content.length, content.offset, wal_header_size_);
  size_t head_size = content.length;
  size_t tail_size = 0;
  if (blob_physical_length > content.length) {
    head_size = kBlockSize - content.offset % kBlockSize;
    assert(head_size != 0 && head_size != kBlockSize);
    size_t kBlockAvailSize = kBlockSize - wal_header_size_;
    tail_size = (content.length - head_size) % kBlockAvailSize;
  }

  // read log file and check checksum
  Blob* blob = new Blob(blob_physical_length);
  Status s = src_->Read(content.offset, blob_physical_length, &(blob->slice_),
                        blob->buf_);
  assert(s.ok());  // TODO return status
  assert(blob->slice_.size() != 0 &&
         blob->slice_.size() == blob_physical_length);
  if (head_size != 0) {
    uint32_t head_crc =
        terark::Crc16c_update(0, blob->slice_.data(), head_size);
    // assert(content.head_crc == head_crc);
    if (content.head_crc != head_crc) {
      // return Status::IOError("");
      assert(false);
    }
  }
  if (tail_size != 0) {
    uint32_t tail_crc = terark::Crc16c_update(
        0, blob->slice_.data() + blob->slice_.size() - tail_size, tail_size);
    // assert(tail_crc == content.tail_crc);
    if (tail_crc != content.tail_crc) {
      assert(false);  // TODO
    }
  }
  // check middletype crc
  char* header = const_cast<char*>(blob->slice_.data()) + head_size;
  const char* tailer = blob->slice_.data() - tail_size;
  while (header <= tailer - kBlockSize) {
    const uint32_t a = static_cast<uint32_t>(header[4]) & 0xff;
    const uint32_t b = static_cast<uint32_t>(header[5]) & 0xff;
    const unsigned int type = header[6];
    const uint32_t length = a | (b << 8);
    assert(type == kMiddleType);

    uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(header));
    uint32_t actual_crc =
        crc32c::Value(header + 6, length + wal_header_size_ - 6);
    assert(actual_crc == expected_crc);
    header += kBlockSize;

    (void)type, (void)actual_crc, (void)expected_crc;
  }

  // insert into blob_cache, and return
  if (head_size != content.length) {
    // cross more than one block, need remove middle log record header
    blob->ShrinkVal(head_size, wal_header_size_);
  }
  s = blob_cache_->Insert(blob_uid, blob, sizeof(Blob) + blob->DataSize(),
                          &DeleteCachedEntry<Blob>, &handle);
  assert(s.ok());
  Blob* b = (Blob*)blob_cache_->Value(handle);
  lazy_blob->reset(b->slice_,
                   Cleanable(release_cache, blob_cache_.get(), handle),
                   log_number_);
  return Status::OK();
}

void WalBlobReader::GetCFWalTupleOffsets(uint32_t cf_id, uint64_t* cf_offset,
                                         uint64_t* cf_entries) {
  const WalIndexFooter* wif = reinterpret_cast<const WalIndexFooter*>(
      index_file_data_.data() + index_file_data_.size() -
      sizeof(WalIndexFooter));

  uint32_t cf_phy_index_size = wif->count_ * sizeof(WalCfIndex);
  Slice footer(index_file_data_.data() + index_file_data_.size() -
                   sizeof(WalIndexFooter) - cf_phy_index_size,
               cf_phy_index_size);

  bool find = false;
  for (uint32_t i = 0; i < wif->count_; ++i) {
    const WalCfIndex* wci = reinterpret_cast<const WalCfIndex*>(
        footer.data() + i * sizeof(WalCfIndex));
    if (wci->id_ == cf_id) {
      *cf_offset = wci->offset_;
      *cf_entries = wci->count_;
      find = true;
    }
  }
  assert(find ||
         wif->count_ == 0);  // TODO maybe skip not founded and return status
}

InternalIterator* WalBlobReader::NewIteratorWithCF(
    const ReadOptions&, uint32_t cf_id, const ImmutableCFOptions& ioptions,
    Arena* arena) {
  const std::string filename = LogIndexFileName(ioptions_.wal_dir, log_number_);
  uint64_t file_size = 0;
  auto s = ioptions_.env->GetFileSize(filename, &file_size);

  if (s.ok() && (!index_file_data_.valid() || index_file_data_.empty())) {
    // since many cf may shared same wal, but only mmap index file into
    // index_file_data once
    EnvOptions env_options_for_index = env_options_;
    env_options_for_index.use_mmap_reads = true;
    env_options_for_index.use_direct_reads = false;
    s = ioptions_.env->NewRandomAccessFile(filename, &src_idx_,
                                           env_options_for_index);
    if (s.ok()) {
      s = src_idx_->Read(0, file_size, &index_file_data_,
                         nullptr /*mmap read*/);
    }
  }
  if (s.ok() && index_file_data_.size() != file_size) {
    // TODO more details
    s = Status::IOError("Invalid file size");
  }
  if (!s.ok()) {
    return NewErrorInternalIterator(s, arena);
  }

  uint64_t cf_offset = 0;
  uint64_t cf_entries = 0;
  GetCFWalTupleOffsets(cf_id, &cf_offset, &cf_entries);
  assert(cf_offset % kWalEntrySize == 0);
  if (arena == nullptr) {
    return new WalBlobIterator(this, ioptions, cf_offset, cf_entries);
  } else {
    auto* mem = arena->AllocateAligned(sizeof(WalBlobIterator));
    return new (mem) WalBlobIterator(this, ioptions, cf_offset, cf_entries);
  }
}

Status WalBlobReader::GetFromHandle(const ReadOptions&, const Slice& handle,
                                    GetContext* get_context) {
  LazyBuffer value;
  auto s = GetBlob(handle, &value);
  if (s.ok()) {
    bool unused;
    bool read_more = get_context->SaveValue(
        ParsedInternalKey(handle, kMaxSequenceNumber, kTypeValue),
        std::move(value), &unused);
    (void)read_more;
    assert(!read_more);
    assert(unused);
  }
  return s;
}

void WalBlobIterator::Next() {
  // TODO key in wal does not merge repeated key, need do it when iterator it
  ++i_;
  if (Valid()) {
    FetchKV();
    if (!last_key_.empty()) {
      (void)ioptions_;
      assert(ioptions_.internal_comparator.Compare(iter_key_.GetKey(),
                                                   Slice(last_key_)) > 0);
    }
    last_key_.assign(iter_key_.GetKey().data(), iter_key_.GetKey().size());
  }
}

Status WalBlobIterator::FetchKV() {
  uint64_t seq;
  ValueType type;

  // unpack seq & type
  const char* cur_kv_base = cf_data_.data() + i_ * kWalEntrySize;
  Slice packed_seq(cur_kv_base + 2 * kDefaultLogHandleSize, 8);
  UnPackSequenceAndType(*(reinterpret_cast<const uint64_t*>(packed_seq.data())),
                        &seq, &type);
  assert(type == kTypeMerge || type == kTypeValue);

  // read userkey use WalBlobReader getblob with handle
  // in SeparateCf, key has already remove prefixed length, so this handle point
  // to user key data
  Slice k_handle(cur_kv_base, kDefaultLogHandleSize);
  LazyBuffer lazy_key;
  reader_->GetBlob(k_handle, &lazy_key);
  auto s = lazy_key.fetch();
  assert(s.ok() && lazy_key.slice().size() != 0);
  iter_key_.SetInternalKey(lazy_key.slice(), seq, type);
  parsed_ikey_ = ParsedInternalKey(lazy_key.slice(), seq, type);

  // read user value
  Slice v_handle(cur_kv_base + kDefaultLogHandleSize, kDefaultLogHandleSize);
  reader_->GetBlob(v_handle, &value_);
  s = value_.fetch();
  assert(value_.valid());
  assert(s.ok());

  return s;
}

Status WalIndexWriter::WriteCF(
    uint32_t cf_id,
    const std::vector<std::pair<ParsedInternalKey, WalEntry>>& sorted_entries) {
  uint32_t crc32 = 0;

  WalCfIndex wci;
  wci.id_ = cf_id;
  wci.offset_ = index_file_->GetFileSize();
  assert(wci.offset_ % sizeof(WalEntry) == 0);
  wci.count_ = sorted_entries.size();

  Status status;
  for (auto& t : sorted_entries) {
    DefaultLogHandle content(t.second.GetSlice());
    assert(content.length != 0);
    status = index_file_->Append(t.second.GetSlice());
    crc32 = crc32c::Extend(crc32, t.second.GetSlice().data(),
                           t.second.GetSlice().size());
    if (!status.ok()) {
      return status;
    }
  }
  wci.crc32_ = crc32;

  cf_indexes_.push_back(wci);
  return status;
}

Status WalIndexWriter::WriteFooter() {
  uint32_t crc32 = 0;
  for (auto& i : cf_indexes_) {
    auto s = index_file_->Append(Slice((char*)&i, sizeof(WalCfIndex)));
    if (!s.ok()) {
      return s;
    }
    crc32 = crc32c::Extend(crc32, (char*)&i, sizeof(WalCfIndex));
  }

  WalIndexFooter wif;
  wif.count_ = static_cast<uint32_t>(cf_indexes_.size());
  wif.crc32_ = crc32c::Extend(crc32, (char*)&wif.count_, 4);
  auto s = index_file_->Append(Slice((char*)&wif, sizeof(wif)));
  if (!s.ok()) {
    return s;
  }
  return s;
}

}  // namespace log
}  // namespace rocksdb
