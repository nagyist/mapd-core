/*
 * Copyright 2022 HEAVY.AI, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file		StringNoneEncoder.h
 * @brief		For unencoded strings
 *
 */

#ifndef STRING_NONE_ENCODER_H
#define STRING_NONE_ENCODER_H
#include "Logger/Logger.h"

#include <cassert>
#include <string>
#include <vector>
#include "AbstractBuffer.h"
#include "ChunkMetadata.h"
#include "Encoder.h"

using Data_Namespace::AbstractBuffer;

class StringNoneEncoder : public Encoder {
 public:
  StringNoneEncoder(AbstractBuffer* buffer)
      : Encoder(buffer), index_buf_(nullptr), last_offset_(-1), has_nulls_(false) {}

  size_t getNumElemsForBytesInsertData(const std::string* srcData,
                                       const int start_idx,
                                       const size_t numAppendElems,
                                       const size_t byteLimit,
                                       const bool replicating = false);

  size_t getNumElemsForBytesEncodedDataAtIndices(const int8_t* index_data,
                                                 const std::vector<size_t>& selected_idx,
                                                 const size_t byte_limit) override;

  std::shared_ptr<ChunkMetadata> appendData(int8_t*& src_data,
                                            const size_t num_elems_to_append,
                                            const SQLTypeInfo& ti,
                                            const bool replicating = false,
                                            const int64_t offset = -1) override {
    UNREACHABLE();  // should never be called for strings
    return nullptr;
  }

  std::shared_ptr<ChunkMetadata> appendEncodedDataAtIndices(
      const int8_t* index_data,
      int8_t* data,
      const std::vector<size_t>& selected_idx) override;

  std::shared_ptr<ChunkMetadata> appendEncodedData(const int8_t* index_data,
                                                   int8_t* data,
                                                   const size_t start_idx,
                                                   const size_t num_elements) override;

  template <typename StringType>
  std::shared_ptr<ChunkMetadata> appendData(const StringType* srcData,
                                            const int start_idx,
                                            const size_t numAppendElems,
                                            const bool replicating = false);

  template <typename StringType>
  std::shared_ptr<ChunkMetadata> appendData(const std::vector<StringType>* srcData,
                                            const int start_idx,
                                            const size_t numAppendElems,
                                            const bool replicating = false);

  ChunkStats getChunkStats() const override;

  ChunkStats synthesizeChunkStats(const SQLTypeInfo&) const override;

  void updateStats(const int64_t, const bool) override { CHECK(false); }

  void updateStats(const double, const bool) override { CHECK(false); }

  void updateStats(const int8_t* const src_data, const size_t num_elements) override {
    UNREACHABLE();
  }

  void updateStats(const std::vector<std::string>* const src_data,
                   const size_t start_idx,
                   const size_t num_elements) override;

  void updateStats(const std::string* src_data,
                   const size_t start_idx,
                   const size_t num_elements) override;

  void updateStats(const std::vector<ArrayDatum>* const src_data,
                   const size_t start_idx,
                   const size_t num_elements) override {
    UNREACHABLE();
  }

  void updateStats(const ArrayDatum* src_data,
                   const size_t start_idx,
                   const size_t num_elements) override {
    UNREACHABLE();
  }

  void reduceStats(const Encoder&) override { CHECK(false); }

  void writeChunkStats(FILE* f) override {
    fwrite((int8_t*)&has_nulls_, sizeof(bool), 1, f);
  }

  void readChunkStats(FILE* f) override {
    CHECK_NE(fread((int8_t*)&has_nulls_, sizeof(bool), size_t(1), f), size_t(0));
  }

  void copyChunkStats(const Encoder* copyFromEncoder) override {
    has_nulls_ = static_cast<const StringNoneEncoder*>(copyFromEncoder)->has_nulls_;
  }

  AbstractBuffer* getIndexBuf() const { return index_buf_; }
  void setIndexBuffer(AbstractBuffer* buf) { index_buf_ = buf; }

  bool setChunkStats(const ChunkStats& stats) override {
    if (has_nulls_ == stats.has_nulls) {
      return false;
    }
    has_nulls_ = stats.has_nulls;
    return true;
  }

  void resetChunkStats() override { has_nulls_ = false; }

  static std::string_view getStringAtIndex(const int8_t* index_data,
                                           const int8_t* data,
                                           size_t index);

 private:
  static std::pair<StringOffsetT, StringOffsetT> getStringOffsets(
      const int8_t* index_data,
      size_t index);

  static size_t getStringSizeAtIndex(const int8_t* index_data, size_t index);

  AbstractBuffer* index_buf_;
  StringOffsetT last_offset_;
  bool has_nulls_;

  template <typename StringType>
  void update_elem_stats(const StringType& elem);

};  // class StringNoneEncoder

#endif  // STRING_NONE_ENCODER_H
