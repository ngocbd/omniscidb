/*
 * Copyright 2020 OmniSci, Inc.
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

#include "ForeignStorageMgr.h"

#include "Catalog/ForeignTable.h"
#include "CsvDataWrapper.h"
#include "ForeignTableSchema.h"
#include "ParquetDataWrapper.h"

namespace foreign_storage {
ForeignStorageMgr::ForeignStorageMgr() : AbstractBufferMgr(0), data_wrapper_map_({}) {}

AbstractBuffer* ForeignStorageMgr::getBuffer(const ChunkKey& chunk_key,
                                             const size_t num_bytes) {
  UNREACHABLE();
  return nullptr;  // Added to avoid "no return statement" compiler warning
}

void ForeignStorageMgr::fetchBuffer(const ChunkKey& chunk_key,
                                    AbstractBuffer* destination_buffer,
                                    const size_t num_bytes) {
  CHECK(destination_buffer);
  CHECK(!destination_buffer->isDirty());
  // Use a temp buffer if we have no cache buffers and have one mapped for this chunk.
  if (fetchBufferIfTempBufferMapEntryExists(chunk_key, destination_buffer, num_bytes)) {
    return;
  }
  createAndPopulateDataWrapperIfNotExists(chunk_key);

  // TODO: Populate optional buffers as part of CSV performance improvement
  std::set<ChunkKey> chunk_keys = get_keys_set_from_table(chunk_key);
  chunk_keys.erase(chunk_key);
  std::map<ChunkKey, AbstractBuffer*> optional_buffers;
  auto required_buffers = allocateTempBuffersForChunks(chunk_keys);
  required_buffers[chunk_key] = destination_buffer;
  // populate will write directly to destination_buffer so no need to copy.
  getDataWrapper(chunk_key)->populateChunkBuffers(required_buffers, optional_buffers);
}

bool ForeignStorageMgr::fetchBufferIfTempBufferMapEntryExists(
    const ChunkKey& chunk_key,
    AbstractBuffer* destination_buffer,
    const size_t num_bytes) {
  AbstractBuffer* buffer{nullptr};
  {
    std::shared_lock temp_chunk_buffer_map_lock(temp_chunk_buffer_map_mutex_);
    if (temp_chunk_buffer_map_.find(chunk_key) == temp_chunk_buffer_map_.end()) {
      return false;
    }
    buffer = temp_chunk_buffer_map_[chunk_key].get();
  }
  CHECK(buffer);
  buffer->copyTo(destination_buffer, num_bytes);
  {
    std::lock_guard temp_chunk_buffer_map_lock(temp_chunk_buffer_map_mutex_);
    temp_chunk_buffer_map_.erase(chunk_key);
  }
  return true;
}

void ForeignStorageMgr::getChunkMetadataVecForKeyPrefix(
    ChunkMetadataVector& chunk_metadata,
    const ChunkKey& keyPrefix) {
  CHECK(is_table_key(keyPrefix));
  createDataWrapperIfNotExists(keyPrefix);
  getDataWrapper(keyPrefix)->populateChunkMetadata(chunk_metadata);
}

void ForeignStorageMgr::removeTableRelatedDS(const int db_id, const int table_id) {
  const ChunkKey table_key{db_id, table_id};
  {
    std::lock_guard data_wrapper_lock(data_wrapper_mutex_);
    data_wrapper_map_.erase(table_key);
  }
  clearTempChunkBufferMapEntriesForTable(table_key);
}

MgrType ForeignStorageMgr::getMgrType() {
  return FOREIGN_STORAGE_MGR;
}

std::string ForeignStorageMgr::getStringMgrType() {
  return ToString(FOREIGN_STORAGE_MGR);
}

bool ForeignStorageMgr::hasDataWrapperForChunk(const ChunkKey& chunk_key) {
  std::shared_lock data_wrapper_lock(data_wrapper_mutex_);
  CHECK(has_table_prefix(chunk_key));
  ChunkKey table_key{chunk_key[CHUNK_KEY_DB_IDX], chunk_key[CHUNK_KEY_TABLE_IDX]};
  return data_wrapper_map_.find(table_key) != data_wrapper_map_.end();
}

std::shared_ptr<ForeignDataWrapper> ForeignStorageMgr::getDataWrapper(
    const ChunkKey& chunk_key) {
  std::shared_lock data_wrapper_lock(data_wrapper_mutex_);
  ChunkKey table_key{chunk_key[CHUNK_KEY_DB_IDX], chunk_key[CHUNK_KEY_TABLE_IDX]};
  CHECK(data_wrapper_map_.find(table_key) != data_wrapper_map_.end());
  return data_wrapper_map_[table_key];
}

void ForeignStorageMgr::setDataWrapper(
    const ChunkKey& table_key,
    std::shared_ptr<MockForeignDataWrapper> data_wrapper) {
  CHECK(is_table_key(table_key));
  std::lock_guard data_wrapper_lock(data_wrapper_mutex_);
  CHECK(data_wrapper_map_.find(table_key) != data_wrapper_map_.end());
  data_wrapper->setParentWrapper(data_wrapper_map_[table_key]);
  data_wrapper_map_[table_key] = data_wrapper;
}

bool ForeignStorageMgr::createDataWrapperIfNotExists(const ChunkKey& chunk_key) {
  std::lock_guard data_wrapper_lock(data_wrapper_mutex_);
  ChunkKey table_key{chunk_key[CHUNK_KEY_DB_IDX], chunk_key[CHUNK_KEY_TABLE_IDX]};
  if (data_wrapper_map_.find(table_key) == data_wrapper_map_.end()) {
    auto db_id = chunk_key[CHUNK_KEY_DB_IDX];
    auto foreign_table =
        Catalog_Namespace::Catalog::checkedGet(db_id)->getForeignTableUnlocked(
            chunk_key[CHUNK_KEY_TABLE_IDX]);

    if (foreign_table->foreign_server->data_wrapper_type ==
        foreign_storage::DataWrapperType::CSV) {
      data_wrapper_map_[table_key] =
          std::make_shared<CsvDataWrapper>(db_id, foreign_table);
    } else if (foreign_table->foreign_server->data_wrapper_type ==
               foreign_storage::DataWrapperType::PARQUET) {
      data_wrapper_map_[table_key] =
          std::make_shared<ParquetDataWrapper>(db_id, foreign_table);
    } else {
      throw std::runtime_error("Unsupported data wrapper");
    }
    return true;
  }
  return false;
}

void ForeignStorageMgr::refreshTable(const ChunkKey& table_key,
                                     const bool evict_cached_entries) {
  // Noop - If the cache is not enabled then a refresh does nothing.
}

void ForeignStorageMgr::clearTempChunkBufferMapEntriesForTable(
    const ChunkKey& table_key) {
  CHECK(is_table_key(table_key));
  std::lock_guard temp_chunk_buffer_map_lock(temp_chunk_buffer_map_mutex_);
  auto start_it = temp_chunk_buffer_map_.lower_bound(table_key);
  ChunkKey upper_bound_prefix{table_key[CHUNK_KEY_DB_IDX],
                              table_key[CHUNK_KEY_TABLE_IDX],
                              std::numeric_limits<int>::max()};
  auto end_it = temp_chunk_buffer_map_.upper_bound(upper_bound_prefix);
  temp_chunk_buffer_map_.erase(start_it, end_it);
}

bool ForeignStorageMgr::isDatawrapperRestored(const ChunkKey& chunk_key) {
  if (!hasDataWrapperForChunk(chunk_key)) {
    return false;
  }
  return getDataWrapper(chunk_key)->isRestored();
}

void ForeignStorageMgr::deleteBuffer(const ChunkKey& chunk_key, const bool purge) {
  UNREACHABLE();
}

void ForeignStorageMgr::deleteBuffersWithPrefix(const ChunkKey& chunk_key_prefix,
                                                const bool purge) {
  UNREACHABLE();
}

bool ForeignStorageMgr::isBufferOnDevice(const ChunkKey& chunk_key) {
  UNREACHABLE();
  return false;  // Added to avoid "no return statement" compiler warning
}

size_t ForeignStorageMgr::getNumChunks() {
  UNREACHABLE();
  return 0;  // Added to avoid "no return statement" compiler warning
}

AbstractBuffer* ForeignStorageMgr::createBuffer(const ChunkKey& chunk_key,
                                                const size_t page_size,
                                                const size_t initial_size) {
  UNREACHABLE();
  return nullptr;  // Added to avoid "no return statement" compiler warning
}

AbstractBuffer* ForeignStorageMgr::putBuffer(const ChunkKey& chunk_key,
                                             AbstractBuffer* source_buffer,
                                             const size_t num_bytes) {
  UNREACHABLE();
  return nullptr;  // Added to avoid "no return statement" compiler warning
}

std::string ForeignStorageMgr::printSlabs() {
  UNREACHABLE();
  return {};  // Added to avoid "no return statement" compiler warning
}

void ForeignStorageMgr::clearSlabs() {
  UNREACHABLE();
}

size_t ForeignStorageMgr::getMaxSize() {
  UNREACHABLE();
  return 0;  // Added to avoid "no return statement" compiler warning
}

size_t ForeignStorageMgr::getInUseSize() {
  UNREACHABLE();
  return 0;  // Added to avoid "no return statement" compiler warning
}

size_t ForeignStorageMgr::getAllocated() {
  UNREACHABLE();
  return 0;  // Added to avoid "no return statement" compiler warning
}

bool ForeignStorageMgr::isAllocationCapped() {
  UNREACHABLE();
  return false;  // Added to avoid "no return statement" compiler warning
}

void ForeignStorageMgr::checkpoint() {
  UNREACHABLE();
}

void ForeignStorageMgr::checkpoint(const int db_id, const int tb_id) {
  UNREACHABLE();
}

AbstractBuffer* ForeignStorageMgr::alloc(const size_t num_bytes) {
  UNREACHABLE();
  return nullptr;  // Added to avoid "no return statement" compiler warning
}

void ForeignStorageMgr::free(AbstractBuffer* buffer) {
  UNREACHABLE();
}

void ForeignStorageMgr::createAndPopulateDataWrapperIfNotExists(
    const ChunkKey& chunk_key) {
  ChunkKey table_key = get_table_key(chunk_key);
  if (createDataWrapperIfNotExists(table_key)) {
    ChunkMetadataVector chunk_metadata;
    getDataWrapper(table_key)->populateChunkMetadata(chunk_metadata);
  }
}

std::set<ChunkKey> get_keys_set_from_table(const ChunkKey& destination_chunk_key) {
  std::set<ChunkKey> chunk_keys;
  auto db_id = destination_chunk_key[CHUNK_KEY_DB_IDX];
  auto table_id = destination_chunk_key[CHUNK_KEY_TABLE_IDX];
  auto destination_column_id = destination_chunk_key[CHUNK_KEY_COLUMN_IDX];
  auto fragment_id = destination_chunk_key[CHUNK_KEY_FRAGMENT_IDX];
  auto foreign_table =
      Catalog_Namespace::Catalog::checkedGet(db_id)->getForeignTableUnlocked(table_id);

  ForeignTableSchema schema{db_id, foreign_table};
  auto logical_column = schema.getLogicalColumn(destination_column_id);
  auto logical_column_id = logical_column->columnId;

  for (auto column_id = logical_column_id;
       column_id <= (logical_column_id + logical_column->columnType.get_physical_cols());
       column_id++) {
    auto column = schema.getColumnDescriptor(column_id);
    if (column->columnType.is_varlen_indeed()) {
      ChunkKey data_chunk_key = {db_id, table_id, column->columnId, fragment_id, 1};
      chunk_keys.emplace(data_chunk_key);

      ChunkKey index_chunk_key{db_id, table_id, column->columnId, fragment_id, 2};
      chunk_keys.emplace(index_chunk_key);
    } else {
      ChunkKey data_chunk_key = {db_id, table_id, column->columnId, fragment_id};
      chunk_keys.emplace(data_chunk_key);
    }
  }
  return chunk_keys;
}

std::vector<ChunkKey> get_keys_vec_from_table(const ChunkKey& destination_chunk_key) {
  std::vector<ChunkKey> chunk_keys;
  auto db_id = destination_chunk_key[CHUNK_KEY_DB_IDX];
  auto table_id = destination_chunk_key[CHUNK_KEY_TABLE_IDX];
  auto destination_column_id = destination_chunk_key[CHUNK_KEY_COLUMN_IDX];
  auto fragment_id = destination_chunk_key[CHUNK_KEY_FRAGMENT_IDX];
  auto foreign_table =
      Catalog_Namespace::Catalog::checkedGet(db_id)->getForeignTableUnlocked(table_id);

  ForeignTableSchema schema{db_id, foreign_table};
  auto logical_column = schema.getLogicalColumn(destination_column_id);
  auto logical_column_id = logical_column->columnId;

  for (auto column_id = logical_column_id;
       column_id <= (logical_column_id + logical_column->columnType.get_physical_cols());
       column_id++) {
    auto column = schema.getColumnDescriptor(column_id);
    if (column->columnType.is_varlen_indeed()) {
      ChunkKey data_chunk_key = {db_id, table_id, column->columnId, fragment_id, 1};
      chunk_keys.emplace_back(data_chunk_key);

      ChunkKey index_chunk_key{db_id, table_id, column->columnId, fragment_id, 2};
      chunk_keys.emplace_back(index_chunk_key);
    } else {
      ChunkKey data_chunk_key = {db_id, table_id, column->columnId, fragment_id};
      chunk_keys.emplace_back(data_chunk_key);
    }
  }
  return chunk_keys;
}

std::map<ChunkKey, AbstractBuffer*> ForeignStorageMgr::allocateTempBuffersForChunks(
    const std::set<ChunkKey>& chunk_keys) {
  std::map<ChunkKey, AbstractBuffer*> chunk_buffer_map;
  std::lock_guard temp_chunk_buffer_map_lock(temp_chunk_buffer_map_mutex_);
  for (const auto& chunk_key : chunk_keys) {
    temp_chunk_buffer_map_[chunk_key] = std::make_unique<ForeignStorageBuffer>();
    chunk_buffer_map[chunk_key] = temp_chunk_buffer_map_[chunk_key].get();
  }
  return chunk_buffer_map;
}
}  // namespace foreign_storage
