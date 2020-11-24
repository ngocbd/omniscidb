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

/*
  TODO(Misiu): A lot of methods here can be made asyncronous.  It may be worth an
  investigation to determine if it's worth adding async versions of them for performance
  reasons.
*/

#include "ForeignStorageCache.h"
#include "Shared/File.h"
#include "Shared/measure.h"

namespace foreign_storage {
using read_lock = mapd_shared_lock<mapd_shared_mutex>;
using write_lock = mapd_unique_lock<mapd_shared_mutex>;

namespace {
template <typename Func, typename T>
void iterate_over_matching_prefix(Func func,
                                  T& chunk_collection,
                                  const ChunkKey& chunk_prefix) {
  ChunkKey upper_prefix(chunk_prefix);
  upper_prefix.push_back(std::numeric_limits<int>::max());
  auto end_it = chunk_collection.upper_bound(static_cast<const ChunkKey>(upper_prefix));
  for (auto chunk_it = chunk_collection.lower_bound(chunk_prefix); chunk_it != end_it;
       ++chunk_it) {
    func(*chunk_it);
  }
}

void set_metadata_for_buffer(AbstractBuffer* buffer, ChunkMetadata* meta) {
  buffer->initEncoder(meta->sqlType);
  buffer->setSize(meta->numBytes);
  buffer->getEncoder()->setNumElems(meta->numElements);
  buffer->getEncoder()->resetChunkStats(meta->chunkStats);
  buffer->setUpdated();
}
}  // namespace

ForeignStorageCache::ForeignStorageCache(const DiskCacheConfig& config)
    : num_chunks_added_(0), num_metadata_added_(0), max_cached_bytes_(config.size_limit) {
  validatePath(config.path);
  global_file_mgr_ = std::make_unique<File_Namespace::GlobalFileMgr>(
      0, config.path, config.num_reader_threads);
  setLimit(config.size_limit);
}

void ForeignStorageCache::deleteBufferIfExists(const ChunkKey& chunk_key) {
  write_lock meta_lock(metadata_mutex_);
  write_lock chunk_lock(chunks_mutex_);
  if (cached_metadata_.find(chunk_key) != cached_metadata_.end()) {
    const auto& tracker_it = eviction_tracker_map_.find(get_table_key(chunk_key));
    if (tracker_it != eviction_tracker_map_.end()) {
      tracker_it->second.eviction_alg_->removeChunk(chunk_key);
    }
    global_file_mgr_->deleteBuffer(chunk_key);
    cached_chunks_.erase(chunk_key);
    cached_metadata_.erase(chunk_key);
  }
}

void ForeignStorageCache::cacheChunk(const ChunkKey& chunk_key, AbstractBuffer* buffer) {
  auto timer = DEBUG_TIMER(__func__);
  write_lock meta_lock(metadata_mutex_);
  write_lock chunk_lock(chunks_mutex_);
  // We should only be caching buffers that are in sync with storage.
  CHECK(!buffer->isDirty());
  if (insertChunkIntoEvictionAlg(chunk_key, buffer->size())) {
    buffer->setUpdated();
    num_chunks_added_++;
    global_file_mgr_->putBuffer(chunk_key, buffer);
    global_file_mgr_->checkpoint();
    // TODO(Misiu): This needs to happen even if insertChunkIntoEvictionAlg() fails.
    cached_metadata_.emplace(chunk_key);
  }
  CHECK(!buffer->isDirty());
}

void ForeignStorageCache::cacheTableChunks(const std::vector<ChunkKey>& chunk_keys) {
  auto timer = DEBUG_TIMER(__func__);
  write_lock lock(chunks_mutex_);
  CHECK(!chunk_keys.empty());

  auto db_id = chunk_keys[0][CHUNK_KEY_DB_IDX];
  auto table_id = chunk_keys[0][CHUNK_KEY_TABLE_IDX];
  const ChunkKey table_key{db_id, table_id};

  createTrackerMapEntryIfNoneExists(table_key);
  for (const auto& chunk_key : chunk_keys) {
    CHECK_EQ(db_id, chunk_key[CHUNK_KEY_DB_IDX]);
    CHECK_EQ(table_id, chunk_key[CHUNK_KEY_TABLE_IDX]);
    CHECK(global_file_mgr_->isBufferOnDevice(chunk_key));
    num_chunks_added_++;
    insertChunkIntoEvictionAlg(chunk_key, global_file_mgr_->getBuffer(chunk_key)->size());
  }
  global_file_mgr_->checkpoint(db_id, table_id);
}

AbstractBuffer* ForeignStorageCache::getCachedChunkIfExists(const ChunkKey& chunk_key) {
  auto timer = DEBUG_TIMER(__func__);
  {
    read_lock lock(chunks_mutex_);
    if (cached_chunks_.find(chunk_key) == cached_chunks_.end()) {
      return nullptr;
    }
  }
  write_lock lock(chunks_mutex_);
  const auto& eviction_tracker_it = eviction_tracker_map_.find(get_table_key(chunk_key));
  if (eviction_tracker_it != eviction_tracker_map_.end()) {
    eviction_tracker_it->second.eviction_alg_->touchChunk(chunk_key);
  }

  return global_file_mgr_->getBuffer(chunk_key);
}

bool ForeignStorageCache::isMetadataCached(const ChunkKey& chunk_key) const {
  auto timer = DEBUG_TIMER(__func__);
  read_lock lock(metadata_mutex_);
  return (cached_metadata_.find(chunk_key) != cached_metadata_.end());
}

bool ForeignStorageCache::recoverCacheForTable(ChunkMetadataVector& meta_vec,
                                               const ChunkKey& table_key) {
  write_lock lock(chunks_mutex_);
  CHECK(meta_vec.size() == 0);
  CHECK(is_table_key(table_key));
  CHECK(dynamic_cast<File_Namespace::FileMgr*>(global_file_mgr_->getFileMgr(table_key)));

  createTrackerMapEntryIfNoneExists(table_key);
  global_file_mgr_->getChunkMetadataVecForKeyPrefix(meta_vec, table_key);
  for (auto& [chunk_key, metadata] : meta_vec) {
    cached_metadata_.emplace(chunk_key);
    // If there is no page count then the chunk was metadata only and should not be
    // cached.
    if (const auto& buf = global_file_mgr_->getBuffer(chunk_key); buf->pageCount() > 0) {
      insertChunkIntoEvictionAlg(chunk_key, buf->size());
    }
  }
  return (meta_vec.size() > 0);
}

void ForeignStorageCache::evictThenEraseChunk(const ChunkKey& chunk_key) {
  write_lock chunk_lock(chunks_mutex_);
  evictThenEraseChunkUnlocked(chunk_key);
}

void ForeignStorageCache::evictThenEraseChunkUnlocked(const ChunkKey& chunk_key) {
  const ChunkKey table_prefix = get_table_key(chunk_key);
  const auto& eviction_tracker_it = eviction_tracker_map_.find(table_prefix);
  if (eviction_tracker_it != eviction_tracker_map_.end()) {
    eviction_tracker_it->second.eviction_alg_->removeChunk(chunk_key);
    eraseChunk(chunk_key, eviction_tracker_it->second);
  }
}

void ForeignStorageCache::cacheMetadataVec(const ChunkMetadataVector& metadata_vec) {
  auto timer = DEBUG_TIMER(__func__);
  write_lock meta_lock(metadata_mutex_);
  write_lock chunk_lock(chunks_mutex_);
  for (auto& [chunk_key, metadata] : metadata_vec) {
    cached_metadata_.emplace(chunk_key);
    AbstractBuffer* buf;
    AbstractBuffer* index_buffer = nullptr;
    ChunkKey index_chunk_key;
    if (is_varlen_key(chunk_key)) {
      // For variable length chunks, metadata is associated with the data chunk
      CHECK(is_varlen_data_key(chunk_key));
      index_chunk_key = {chunk_key[CHUNK_KEY_DB_IDX],
                         chunk_key[CHUNK_KEY_TABLE_IDX],
                         chunk_key[CHUNK_KEY_COLUMN_IDX],
                         chunk_key[CHUNK_KEY_FRAGMENT_IDX],
                         2};
    }

    if (!global_file_mgr_->isBufferOnDevice(chunk_key)) {
      buf = global_file_mgr_->createBuffer(chunk_key);

      if (!index_chunk_key.empty()) {
        CHECK(!global_file_mgr_->isBufferOnDevice(index_chunk_key));
        index_buffer = global_file_mgr_->createBuffer(index_chunk_key);
        CHECK(index_buffer);
      }
    } else {
      buf = global_file_mgr_->getBuffer(chunk_key);

      if (!index_chunk_key.empty()) {
        CHECK(global_file_mgr_->isBufferOnDevice(index_chunk_key));
        index_buffer = global_file_mgr_->getBuffer(index_chunk_key);
        CHECK(index_buffer);
      }
    }

    set_metadata_for_buffer(buf, metadata.get());
    evictThenEraseChunkUnlocked(chunk_key);

    if (!index_chunk_key.empty()) {
      CHECK(index_buffer);
      index_buffer->setUpdated();
      evictThenEraseChunkUnlocked(index_chunk_key);
    }
    num_metadata_added_++;
  }
  global_file_mgr_->checkpoint();
}

void ForeignStorageCache::getCachedMetadataVecForKeyPrefix(
    ChunkMetadataVector& metadata_vec,
    const ChunkKey& chunk_prefix) const {
  auto timer = DEBUG_TIMER(__func__);
  read_lock r_lock(metadata_mutex_);
  iterate_over_matching_prefix(
      [&metadata_vec, this](auto chunk) {
        std::shared_ptr<ChunkMetadata> buf_metadata = std::make_shared<ChunkMetadata>();
        global_file_mgr_->getBuffer(chunk)->getEncoder()->getMetadata(buf_metadata);
        metadata_vec.push_back(std::make_pair(chunk, buf_metadata));
      },
      cached_metadata_,
      chunk_prefix);
}

bool ForeignStorageCache::hasCachedMetadataForKeyPrefix(
    const ChunkKey& chunk_prefix) const {
  auto timer = DEBUG_TIMER(__func__);
  read_lock lock(metadata_mutex_);
  // We don't use iterateOvermatchingPrefix() here because we want to exit early if
  // possible.
  ChunkKey upper_prefix(chunk_prefix);
  upper_prefix.push_back(std::numeric_limits<int>::max());
  auto end_it = cached_metadata_.upper_bound(static_cast<const ChunkKey>(upper_prefix));
  for (auto meta_it = cached_metadata_.lower_bound(chunk_prefix); meta_it != end_it;
       ++meta_it) {
    return true;
  }
  return false;
}

void ForeignStorageCache::clearForTablePrefix(const ChunkKey& chunk_prefix) {
  CHECK(is_table_key(chunk_prefix));
  auto timer = DEBUG_TIMER(__func__);
  ChunkKey upper_prefix(chunk_prefix);
  upper_prefix.push_back(std::numeric_limits<int>::max());
  {
    write_lock w_lock(chunks_mutex_);
    // Delete chunks for prefix
    auto end_it = cached_chunks_.upper_bound(static_cast<const ChunkKey>(upper_prefix));
    for (auto chunk_it = cached_chunks_.lower_bound(chunk_prefix); chunk_it != end_it;) {
      chunk_it = eraseChunkByIterator(chunk_it);
    }
  }
  {
    write_lock w_lock(metadata_mutex_);
    // Delete metadata for prefix
    auto end_it = cached_metadata_.upper_bound(static_cast<const ChunkKey>(upper_prefix));
    for (auto meta_it = cached_metadata_.lower_bound(chunk_prefix); meta_it != end_it;) {
      meta_it = cached_metadata_.erase(meta_it);
    }
  }
  global_file_mgr_->removeTableRelatedDS(chunk_prefix[0], chunk_prefix[1]);
}

void ForeignStorageCache::clear() {
  auto timer = DEBUG_TIMER(__func__);
  std::set<ChunkKey> table_keys;
  {
    write_lock w_lock(chunks_mutex_);
    for (auto chunk_it = cached_chunks_.begin(); chunk_it != cached_chunks_.end();) {
      chunk_it = eraseChunkByIterator(chunk_it);
    }
  }
  {
    write_lock w_lock(metadata_mutex_);
    for (auto meta_it = cached_metadata_.begin(); meta_it != cached_metadata_.end();) {
      table_keys.emplace(ChunkKey{(*meta_it)[0], (*meta_it)[1]});
      meta_it = cached_metadata_.erase(meta_it);
    }
  }
  for (const auto& table_key : table_keys) {
    global_file_mgr_->removeTableRelatedDS(table_key[0], table_key[1]);
  }
}

void ForeignStorageCache::setLimit(uint64_t limit) {
  auto timer = DEBUG_TIMER(__func__);
  write_lock w_lock(chunks_mutex_);
  size_t file_size = global_file_mgr_->getDefaultPageSize() * MAX_FILE_N_PAGES;
  if (limit < file_size) {
    throw CacheTooSmallException("Could not create cache with size " + to_string(limit) +
                                 ".  Minimum cache size is " + to_string(file_size));
  }
  size_t max_num_files = (limit + (file_size - 1)) / file_size;
  max_pages_per_table_ = max_num_files * MAX_FILE_N_PAGES;
  for (auto& [table_key, tracker] : eviction_tracker_map_) {
    auto& [eviction_alg, num_pages] = tracker;
    while (num_pages > max_pages_per_table_) {
      eraseChunk(eviction_alg->evictNextChunk(), tracker);
    }
  }
  global_file_mgr_->checkpoint();
  max_cached_bytes_ = limit;
}

std::vector<ChunkKey> ForeignStorageCache::getCachedChunksForKeyPrefix(
    const ChunkKey& chunk_prefix) const {
  read_lock r_lock(chunks_mutex_);
  std::vector<ChunkKey> ret_vec;
  iterate_over_matching_prefix(
      [&ret_vec](auto chunk) { ret_vec.push_back(chunk); }, cached_chunks_, chunk_prefix);
  return ret_vec;
}

std::map<ChunkKey, AbstractBuffer*> ForeignStorageCache::getChunkBuffersForCaching(
    const std::vector<ChunkKey>& chunk_keys) const {
  auto timer = DEBUG_TIMER(__func__);
  std::map<ChunkKey, AbstractBuffer*> chunk_buffer_map;
  read_lock lock(chunks_mutex_);
  for (const auto& chunk_key : chunk_keys) {
    CHECK(cached_chunks_.find(chunk_key) == cached_chunks_.end());
    CHECK(global_file_mgr_->isBufferOnDevice(chunk_key));
    chunk_buffer_map[chunk_key] = global_file_mgr_->getBuffer(chunk_key);
    CHECK(dynamic_cast<File_Namespace::FileBuffer*>(chunk_buffer_map[chunk_key]));
    CHECK_EQ(chunk_buffer_map[chunk_key]->pageCount(), static_cast<size_t>(0));

    // Clear all buffer metadata
    chunk_buffer_map[chunk_key]->resetToEmpty();
  }
  return chunk_buffer_map;
}

// Private functions.  Locks should be acquired in the public interface before calling
// these functions.
// This function assumes the chunk has been erased from the eviction algorithm already.
void ForeignStorageCache::eraseChunk(const ChunkKey& chunk_key,
                                     TableEvictionTracker& tracker) {
  auto timer = DEBUG_TIMER(__func__);
  if (cached_chunks_.find(chunk_key) == cached_chunks_.end()) {
    return;
  }
  File_Namespace::FileBuffer* file_buffer =
      static_cast<File_Namespace::FileBuffer*>(global_file_mgr_->getBuffer(chunk_key));
  tracker.num_pages_ -= file_buffer->freeChunkPages();
  cached_chunks_.erase(chunk_key);
}

std::set<ChunkKey>::iterator ForeignStorageCache::eraseChunkByIterator(
    const std::set<ChunkKey>::iterator& chunk_it) {
  auto timer = DEBUG_TIMER(__func__);
  const ChunkKey table_key = get_table_key(*chunk_it);
  auto& [eviction_alg, num_pages] = eviction_tracker_map_.at(table_key);
  eviction_alg->removeChunk(*chunk_it);
  File_Namespace::FileBuffer* file_buffer =
      static_cast<File_Namespace::FileBuffer*>(global_file_mgr_->getBuffer(*chunk_it));
  num_pages -= file_buffer->freeChunkPages();
  return cached_chunks_.erase(chunk_it);
}

std::string ForeignStorageCache::dumpCachedChunkEntries() const {
  auto timer = DEBUG_TIMER(__func__);
  std::string ret_string = "Cached chunks:\n";
  for (const auto& chunk_key : cached_chunks_) {
    ret_string += "  " + show_chunk(chunk_key) + "\n";
  }
  return ret_string;
}

std::string ForeignStorageCache::dumpCachedMetadataEntries() const {
  auto timer = DEBUG_TIMER(__func__);
  std::string ret_string = "Cached ChunkMetadata:\n";
  for (const auto& meta_key : cached_metadata_) {
    ret_string += "  " + show_chunk(meta_key) + "\n";
  }
  return ret_string;
}

std::string ForeignStorageCache::dumpEvictionQueue() const {
  std::string ret;
  for (auto& [key, tracker] : eviction_tracker_map_) {
    auto& [alg, num_pages] = tracker;
    ret += "queue for table_key: " + show_chunk(key) + "\n" +
           ((LRUEvictionAlgorithm*)alg.get())->dumpEvictionQueue();
  }

  return ret;
}

void ForeignStorageCache::validatePath(const std::string& base_path) const {
  // check if base_path already exists, and if not create one
  boost::filesystem::path path(base_path);
  if (boost::filesystem::exists(path)) {
    if (!boost::filesystem::is_directory(path)) {
      throw std::runtime_error{
          "cache path \"" + base_path +
          "\" is not a directory.  Please specify a valid directory "
          "with --disk_cache_path=<path>, or use the default location."};
    }
  } else {  // data directory does not exist
    if (!boost::filesystem::create_directory(path)) {
      throw std::runtime_error{
          "could not create directory at cache path \"" + base_path +
          "\".  Please specify a valid directory location "
          "with --disk_cache_path=<path> or use the default location."};
    }
  }
}

std::string ForeignStorageCache::getCacheDirectoryForTablePrefix(
    const ChunkKey& table_prefix) const {
  CHECK(table_prefix.size() >= 2);
  auto fileMgr = dynamic_cast<File_Namespace::FileMgr*>(
      getGlobalFileMgr()->getFileMgr(table_prefix));
  CHECK(fileMgr);
  return fileMgr->getFileMgrBasePath();
}

bool ForeignStorageCache::insertChunkIntoEvictionAlg(const ChunkKey& chunk_key,
                                                     const size_t chunk_size) {
  size_t page_size = global_file_mgr_->getDefaultPageSize();
  const ChunkKey table_key = get_table_key(chunk_key);
  auto&& tracker_it = eviction_tracker_map_.find(table_key);
  CHECK(tracker_it != eviction_tracker_map_.end());
  auto& tracker = tracker_it->second;
  // number of pages per chunk rounded up.
  size_t num_pages_for_chunk = (chunk_size + (page_size - 1)) / page_size;
  if (num_pages_for_chunk > max_pages_per_table_) {
    // Can't fit the chunk in the cache, so bail.
    return false;
  }
  while (tracker.num_pages_ + num_pages_for_chunk > max_pages_per_table_) {
    eraseChunk(tracker.eviction_alg_->evictNextChunk(), tracker);
  }

  tracker.eviction_alg_->touchChunk(chunk_key);
  cached_chunks_.emplace(chunk_key);
  tracker.num_pages_ += num_pages_for_chunk;
  return true;
}

void ForeignStorageCache::createTrackerMapEntryIfNoneExists(const ChunkKey& table_key) {
  CHECK(is_table_key(table_key));
  if (eviction_tracker_map_.find(table_key) == eviction_tracker_map_.end()) {
    eviction_tracker_map_.emplace(table_key, TableEvictionTracker{});
  }
}

void ForeignStorageCache::cacheMetadataWithFragIdGreaterOrEqualTo(
    const ChunkMetadataVector& metadata_vec,
    const int frag_id) {
  // Only re-cache last fragment and above
  ChunkMetadataVector new_metadata_vec;
  for (const auto& chunk_metadata : metadata_vec) {
    if (chunk_metadata.first[CHUNK_KEY_FRAGMENT_IDX] >= frag_id) {
      new_metadata_vec.push_back(chunk_metadata);
    }
  }
  cacheMetadataVec(new_metadata_vec);
}

}  // namespace foreign_storage
