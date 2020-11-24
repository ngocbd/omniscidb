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

#include "CachingForeignStorageMgr.h"

#include "Catalog/ForeignTable.h"
#include "CsvDataWrapper.h"
#include "ForeignTableSchema.h"
#include "ParquetDataWrapper.h"

namespace foreign_storage {

namespace {
constexpr int64_t MAX_REFRESH_TIME_IN_SECONDS = 60 * 60;
const std::string wrapper_file_name = "/wrapper_metadata.json";
}  // namespace

CachingForeignStorageMgr::CachingForeignStorageMgr(ForeignStorageCache* cache)
    : ForeignStorageMgr(), disk_cache_(cache) {
  CHECK(disk_cache_);
}

void CachingForeignStorageMgr::fetchBuffer(const ChunkKey& chunk_key,
                                           AbstractBuffer* destination_buffer,
                                           const size_t num_bytes) {
  CHECK(destination_buffer);
  CHECK(!destination_buffer->isDirty());

  createOrRecoverDataWrapperIfNotExists(chunk_key);

  // TODO: Populate optional buffers as part of CSV performance improvement
  std::vector<ChunkKey> chunk_keys = get_keys_vec_from_table(chunk_key);
  std::map<ChunkKey, AbstractBuffer*> optional_buffers;
  std::map<ChunkKey, AbstractBuffer*> required_buffers =
      disk_cache_->getChunkBuffersForCaching(chunk_keys);
  CHECK(required_buffers.find(chunk_key) != required_buffers.end());
  getDataWrapper(chunk_key)->populateChunkBuffers(required_buffers, optional_buffers);
  disk_cache_->cacheTableChunks(chunk_keys);

  AbstractBuffer* buffer = required_buffers.at(chunk_key);
  CHECK(buffer);

  buffer->copyTo(destination_buffer, num_bytes);
}

void CachingForeignStorageMgr::getChunkMetadataVecForKeyPrefix(
    ChunkMetadataVector& chunk_metadata,
    const ChunkKey& keyPrefix) {
  ForeignStorageMgr::getChunkMetadataVecForKeyPrefix(chunk_metadata, keyPrefix);
  getDataWrapper(keyPrefix)->serializeDataWrapperInternals(
      disk_cache_->getCacheDirectoryForTablePrefix(keyPrefix) + wrapper_file_name);
}

void CachingForeignStorageMgr::recoverDataWrapperFromDisk(
    const ChunkKey& table_key,
    const ChunkMetadataVector& chunk_metadata) {
  getDataWrapper(table_key)->restoreDataWrapperInternals(
      disk_cache_->getCacheDirectoryForTablePrefix(table_key) + wrapper_file_name,
      chunk_metadata);
}

void CachingForeignStorageMgr::refreshTable(const ChunkKey& table_key,
                                            const bool evict_cached_entries) {
  CHECK(is_table_key(table_key));
  clearTempChunkBufferMapEntriesForTable(table_key);
  evict_cached_entries ? disk_cache_->clearForTablePrefix(table_key)
                       : refreshTableInCache(table_key);
}

void CachingForeignStorageMgr::refreshTableInCache(const ChunkKey& table_key) {
  CHECK(is_table_key(table_key));

  // Before we can refresh a table we should make sure it has recovered any data
  // if the table has been unused since the last server restart.
  if (!disk_cache_->hasCachedMetadataForKeyPrefix(table_key)) {
    ChunkMetadataVector old_cached_metadata;
    disk_cache_->recoverCacheForTable(old_cached_metadata, table_key);
  }

  // Preserve the list of which chunks were cached per table to refresh after clear.
  std::vector<ChunkKey> old_chunk_keys =
      disk_cache_->getCachedChunksForKeyPrefix(table_key);

  bool append_mode = Catalog_Namespace::Catalog::checkedGet(table_key[CHUNK_KEY_DB_IDX])
                         ->getForeignTableUnlocked(table_key[CHUNK_KEY_TABLE_IDX])
                         ->isAppendMode();

  append_mode ? refreshAppendTableInCache(table_key, old_chunk_keys)
              : refreshNonAppendTableInCache(table_key, old_chunk_keys);
}

int CachingForeignStorageMgr::getHighestCachedFragId(const ChunkKey& table_key) {
  // Determine last fragment ID
  int last_frag_id = 0;
  if (disk_cache_->hasCachedMetadataForKeyPrefix(table_key)) {
    ChunkMetadataVector cached_metadata;
    disk_cache_->getCachedMetadataVecForKeyPrefix(cached_metadata, table_key);
    for (const auto& [key, metadata] : cached_metadata) {
      last_frag_id = std::max(last_frag_id, key[CHUNK_KEY_FRAGMENT_IDX]);
    }
  }
  return last_frag_id;
}

void CachingForeignStorageMgr::refreshAppendTableInCache(
    const ChunkKey& table_key,
    const std::vector<ChunkKey>& old_chunk_keys) {
  CHECK(is_table_key(table_key));
  createOrRecoverDataWrapperIfNotExists(table_key);
  int last_frag_id = getHighestCachedFragId(table_key);

  ChunkMetadataVector storage_metadata;
  getChunkMetadataVecForKeyPrefix(storage_metadata, table_key);
  try {
    disk_cache_->cacheMetadataWithFragIdGreaterOrEqualTo(storage_metadata, last_frag_id);
    refreshChunksInCacheByFragment(old_chunk_keys, last_frag_id);
  } catch (std::runtime_error& e) {
    throw PostEvictionRefreshException(e);
  }
}

void CachingForeignStorageMgr::refreshNonAppendTableInCache(
    const ChunkKey& table_key,
    const std::vector<ChunkKey>& old_chunk_keys) {
  CHECK(is_table_key(table_key));
  // Getting metadata from (foreign) storage could throw if we have lost our connnection.
  // Therefore we only want clear the cache and refresh after we have confirmed that we
  // can get new data from storage, if we can't reach storage then throwing here will
  // leave the cache unchanged.
  ChunkMetadataVector storage_metadata;
  getChunkMetadataVecForKeyPrefix(storage_metadata, table_key);
  disk_cache_->clearForTablePrefix(table_key);
  try {
    disk_cache_->cacheMetadataVec(storage_metadata);
    refreshChunksInCacheByFragment(old_chunk_keys, 0);
  } catch (std::runtime_error& e) {
    throw PostEvictionRefreshException(e);
  }
}

void CachingForeignStorageMgr::refreshChunksInCacheByFragment(
    const std::vector<ChunkKey>& old_chunk_keys,
    int start_frag_id) {
  int64_t total_time{0};
  auto fragment_refresh_start_time = std::chrono::high_resolution_clock::now();

  if (old_chunk_keys.empty()) {
    return;
  }
  // Iterate through previously cached chunks and re-cache them. Caching is
  // done one fragment at a time, for all applicable chunks in the fragment.
  std::map<ChunkKey, AbstractBuffer*> optional_buffers;
  std::vector<ChunkKey> chunk_keys_to_be_cached;
  auto fragment_id = old_chunk_keys[0][CHUNK_KEY_FRAGMENT_IDX];
  const ChunkKey table_key{get_table_key(old_chunk_keys[0])};
  std::vector<ChunkKey> chunk_keys_in_fragment;
  for (const auto& chunk_key : old_chunk_keys) {
    if (chunk_key[CHUNK_KEY_FRAGMENT_IDX] < start_frag_id) {
      continue;
    }
    if (disk_cache_->isMetadataCached(chunk_key)) {
      if (chunk_key[CHUNK_KEY_FRAGMENT_IDX] != fragment_id) {
        if (chunk_keys_in_fragment.size() > 0) {
          auto required_buffers =
              disk_cache_->getChunkBuffersForCaching(chunk_keys_in_fragment);
          getDataWrapper(table_key)->populateChunkBuffers(required_buffers,
                                                          optional_buffers);
          chunk_keys_in_fragment.clear();
        }
        // At this point, cache buffers for refreshable chunks in the last fragment
        // have been populated. Exit if the max refresh time has been exceeded.
        // Otherwise, move to the next fragment.
        auto current_time = std::chrono::high_resolution_clock::now();
        total_time += std::chrono::duration_cast<std::chrono::seconds>(
                          current_time - fragment_refresh_start_time)
                          .count();
        if (total_time >= MAX_REFRESH_TIME_IN_SECONDS) {
          LOG(WARNING) << "Refresh time exceeded for table key: { " << table_key[0]
                       << ", " << table_key[1] << " } after fragment id: " << fragment_id;
          break;
        } else {
          fragment_refresh_start_time = std::chrono::high_resolution_clock::now();
        }
        fragment_id = chunk_key[CHUNK_KEY_FRAGMENT_IDX];
      }
      if (is_varlen_key(chunk_key)) {
        CHECK(is_varlen_data_key(chunk_key));
        ChunkKey index_chunk_key{chunk_key[CHUNK_KEY_DB_IDX],
                                 chunk_key[CHUNK_KEY_TABLE_IDX],
                                 chunk_key[CHUNK_KEY_COLUMN_IDX],
                                 chunk_key[CHUNK_KEY_FRAGMENT_IDX],
                                 2};
        chunk_keys_in_fragment.emplace_back(index_chunk_key);
        chunk_keys_to_be_cached.emplace_back(index_chunk_key);
      }
      chunk_keys_in_fragment.emplace_back(chunk_key);
      chunk_keys_to_be_cached.emplace_back(chunk_key);
    }
  }
  if (chunk_keys_in_fragment.size() > 0) {
    auto required_buffers =
        disk_cache_->getChunkBuffersForCaching(chunk_keys_in_fragment);
    getDataWrapper(table_key)->populateChunkBuffers(required_buffers, optional_buffers);
  }
  disk_cache_->cacheTableChunks(chunk_keys_to_be_cached);
}

void CachingForeignStorageMgr::createOrRecoverDataWrapperIfNotExists(
    const ChunkKey& chunk_key) {
  ChunkKey table_key = get_table_key(chunk_key);
  if (createDataWrapperIfNotExists(table_key)) {
    ChunkMetadataVector chunk_metadata;
    if (disk_cache_->hasCachedMetadataForKeyPrefix(table_key)) {
      disk_cache_->getCachedMetadataVecForKeyPrefix(chunk_metadata, table_key);
      recoverDataWrapperFromDisk(table_key, chunk_metadata);
    } else {
      getDataWrapper(table_key)->populateChunkMetadata(chunk_metadata);
    }
  }
}

}  // namespace foreign_storage
