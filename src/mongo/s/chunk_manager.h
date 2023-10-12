/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <set>
#include <string>
#include <vector>

#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace mongo {

class CanonicalQuery;
struct QuerySolutionNode;
class OperationContext;
class ChunkManager;

// Map from a shard is to the max chunk version on that shard
using ShardVersionMap = std::map<ShardId, ChunkVersion>;

// This class serves as a Facade around how the mapping of ranges to chunks is represented. It also
// provides a simpler, high-level interface for domain specific operations without exposing the
// underlying implementation.
class ChunkMap {
public:
    // Vector of chunks ordered by max key in ascending order.
    using ChunkVector = std::vector<std::shared_ptr<ChunkInfo>>;
    using ChunkVectorMap = std::map<std::string, std::shared_ptr<ChunkVector>>;

    explicit ChunkMap(OID epoch, size_t chunkVectorSize)
        : _collectionVersion(0, 0, epoch), _maxChunkVectorSize(chunkVectorSize) {}

    size_t size() const;

    // Max version across all chunks
    ChunkVersion getVersion() const {
        return _collectionVersion;
    }

    size_t getMaxChunkVectorSize() const {
        return _maxChunkVectorSize;
    }

    const ShardVersionMap& getShardVersionsMap() const {
        return _shardVersions;
    }

    const ChunkVectorMap& getChunkVectorMap() const {
        return _chunkVectorMap;
    }


    /*
     * Invoke the given handler for each std::shared_ptr<ChunkInfo> contained in this chunk map
     * until either all matching chunks have been processed or @handler returns false.
     *
     * Chunks are yielded in ascending order of shardkey (e.g. minKey to maxKey);
     *
     * When shardKey is provided the function will start yileding from the chunk that contains the
     * given shard key.
     */
    template <typename Callable>
    void forEach(Callable&& handler, const BSONObj& shardKey = BSONObj()) const {
        if (shardKey.isEmpty()) {
            for (const auto& mapIt : _chunkVectorMap) {
                for (const auto& chunkInfoPtr : *(mapIt.second)) {
                    if (!handler(chunkInfoPtr))
                        return;
                }
            }

            return;
        }

        auto shardKeyString = ShardKeyPattern::toKeyString(shardKey);

        const auto mapItBegin = _chunkVectorMap.upper_bound(shardKeyString);
        for (auto mapIt = mapItBegin; mapIt != _chunkVectorMap.end(); mapIt++) {
            const auto& chunkVector = *(mapIt->second);
            auto it = mapIt == mapItBegin ? _findIntersectingChunkIterator(shardKeyString,
                                                                           chunkVector.begin(),
                                                                           chunkVector.end(),
                                                                           true /*isMaxInclusive*/)
                                          : chunkVector.begin();
            for (; it != chunkVector.end(); ++it) {
                if (!handler(*it))
                    return;
            }
        }
    }


    /*
     * Invoke the given @handler for each std::shared_ptr<ChunkInfo> that overlaps with range [@min,
     * @max] until either all matching chunks have been processed or @handler returns false.
     *
     * Chunks are yielded in ascending order of shardkey (e.g. minKey to maxKey);
     *
     * When @isMaxInclusive is true also the chunk whose minKey is equal to @max will be yielded.
     */
    template <typename Callable>
    void forEachOverlappingChunk(const BSONObj& min,
                                 const BSONObj& max,
                                 bool isMaxInclusive,
                                 Callable&& handler) const {
        const auto minShardKeyStr = ShardKeyPattern::toKeyString(min);
        const auto maxShardKeyStr = ShardKeyPattern::toKeyString(max);
        const auto bounds =
            _overlappingVectorSlotBounds(minShardKeyStr, maxShardKeyStr, isMaxInclusive);
        for (auto mapIt = bounds.first; mapIt != bounds.second; ++mapIt) {

            const auto& chunkVector = *(mapIt->second);

            const auto chunkItBegin = [&] {
                if (mapIt == bounds.first) {
                    // On first vector we need to start from chunk that contain the given minKey
                    return _findIntersectingChunkIterator(minShardKeyStr,
                                                          chunkVector.begin(),
                                                          chunkVector.end(),
                                                          true /* isMaxInclusive */);
                }
                return chunkVector.begin();
            }();

            const auto chunkItEnd = [&] {
                if (mapIt == std::prev(bounds.second)) {
                    // On last vector we need to skip all chunks that are greater than the give
                    // maxKey
                    auto it = _findIntersectingChunkIterator(
                        maxShardKeyStr, chunkItBegin, chunkVector.end(), isMaxInclusive);
                    return it == chunkVector.end() ? it : ++it;
                }
                return chunkVector.end();
            }();

            for (auto chunkIt = chunkItBegin; chunkIt != chunkItEnd; ++chunkIt) {
                if (!handler(*chunkIt))
                    return;
            }
        }
    }

    std::shared_ptr<ChunkInfo> findIntersectingChunk(const BSONObj& shardKey) const;

    ChunkMap createMerged(ChunkVector changedChunks) const;

    BSONObj toBSON() const;

    std::string toString() const;

private:
    ChunkVector::const_iterator _findIntersectingChunkIterator(const std::string& shardKeyString,
                                                               ChunkVector::const_iterator first,
                                                               ChunkVector::const_iterator last,
                                                               bool isMaxInclusive) const;

    std::pair<ChunkVectorMap::const_iterator, ChunkVectorMap::const_iterator>
    _overlappingVectorSlotBounds(const std::string& minShardKeyStr,
                                 const std::string& maxShardKeyStr,
                                 bool isMaxInclusive) const;
    ChunkMap _makeUpdated(ChunkVector&& changedChunks) const;

    void _updateShardVersionFromDiscardedChunk(const ChunkInfo& chunk);
    void _updateShardVersionFromUpdateChunk(const ChunkInfo& chunk);
    void _commitUpdatedChunkVector(std::shared_ptr<ChunkVector>&& chunkVectorPtr,
                                   bool checkMaxKeyConsistency);
    void _mergeAndCommitUpdatedChunkVector(ChunkVectorMap::const_iterator pos,
                                           std::shared_ptr<ChunkVector>&& chunkVectorPtr);
    void _splitAndCommitUpdatedChunkVector(ChunkVectorMap::const_iterator pos,
                                           std::shared_ptr<ChunkVector>&& chunkVectorPtr);

    ChunkVectorMap _chunkVectorMap;

    // Max version across all chunks
    ChunkVersion _collectionVersion;

    // The representation of shard versions and staleness indicators for this namespace. If a
    // shard does not exist, it will not have an entry in the map.
    // Note: this declaration must not be moved before _chunkMap since it is initialized by using
    // the _chunkVectorMap instance.
    ShardVersionMap _shardVersions;

    // Maximum size of chunk vectors stored in the chunk vector map.
    // Bigger vectors will imply slower incremental refreshes (more chunks to copy) but
    // faster map copy (less chunk vector pointers to copy).
    size_t _maxChunkVectorSize;
};

/**
 * In-memory representation of the routing table for a single sharded collection at various points
 * in time.
 */
class RoutingTableHistory : public std::enable_shared_from_this<RoutingTableHistory> {
    RoutingTableHistory(const RoutingTableHistory&) = delete;
    RoutingTableHistory& operator=(const RoutingTableHistory&) = delete;

public:
    /**
     * Makes an instance with a routing table for collection "nss", sharded on
     * "shardKeyPattern".
     *
     * "defaultCollator" is the default collation for the collection, "unique" indicates whether
     * or not the shard key for each document will be globally unique, and "epoch" is the globally
     * unique identifier for this version of the collection.
     *
     * The "chunks" vector must contain the chunk routing information sorted in ascending order by
     * chunk version, and adhere to the requirements of the routing table update algorithm.
     */
    static std::shared_ptr<RoutingTableHistory> makeNew(
        NamespaceString nss,
        boost::optional<UUID>,
        KeyPattern shardKeyPattern,
        std::unique_ptr<CollatorInterface> defaultCollator,
        bool unique,
        OID epoch,
        const std::vector<ChunkType>& chunks);

    /**
     * Constructs a new instance with a routing table updated according to the changes described
     * in "changedChunks".
     *
     * The changes in "changedChunks" must be sorted in ascending order by chunk version, and adhere
     * to the requirements of the routing table update algorithm.
     */
    std::shared_ptr<RoutingTableHistory> makeUpdated(const std::vector<ChunkType>& changedChunks);

    /**
     * Returns an increasing number of the reload sequence number of this chunk manager.
     */
    unsigned long long getSequenceNumber() const {
        return _sequenceNumber;
    }

    const NamespaceString& getns() const {
        return _nss;
    }

    const ShardKeyPattern& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    const CollatorInterface* getDefaultCollator() const {
        return _defaultCollator.get();
    }

    bool isUnique() const {
        return _unique;
    }

    // Max version across all chunks
    ChunkVersion getVersion() const {
        return _chunkMap.getVersion();
    }

    ChunkVersion getVersion(const ShardId& shardId) const;

    size_t numChunks() const {
        return _chunkMap.size();
    }

    template <typename Callable>
    void forEachChunk(Callable&& handler, const BSONObj& shardKey = BSONObj()) const {
        _chunkMap.forEach(std::forward<Callable>(handler), shardKey);
    }

    template <typename Callable>
    void forEachOverlappingChunk(const BSONObj& min,
                                 const BSONObj& max,
                                 bool isMaxInclusive,
                                 Callable&& handler) const {
        _chunkMap.forEachOverlappingChunk(
            min, max, isMaxInclusive, std::forward<Callable>(handler));
    }

    std::shared_ptr<ChunkInfo> findIntersectingChunk(const BSONObj& shardKey) const {
        return _chunkMap.findIntersectingChunk(shardKey);
    }

    /**
     * Returns the ids of all shards on which the collection has any chunks.
     */
    void getAllShardIds(std::set<ShardId>* all) const;

    /**
     * Returns the number of shards on which the collection has any chunks
     */
    int getNShardsOwningChunks() const;

    /**
     * Returns true if, for this shard, the chunks are identical in both chunk managers
     */
    bool compatibleWith(const RoutingTableHistory& other, const ShardId& shard) const;

    std::string toString() const;

    bool uuidMatches(UUID uuid) const {
        return _uuid && *_uuid == uuid;
    }

    boost::optional<UUID> getUUID() const {
        return _uuid;
    }

private:
    RoutingTableHistory(NamespaceString nss,
                        boost::optional<UUID> uuid,
                        KeyPattern shardKeyPattern,
                        std::unique_ptr<CollatorInterface> defaultCollator,
                        bool unique,
                        ChunkMap chunkMap);

    // The shard versioning mechanism hinges on keeping track of the number of times we reload
    // ChunkManagers.
    const unsigned long long _sequenceNumber;

    // Namespace to which this routing information corresponds
    const NamespaceString _nss;

    // The invariant UUID of the collection.  This is optional in 3.6, except in change streams.
    const boost::optional<UUID> _uuid;

    // The key pattern used to shard the collection
    const ShardKeyPattern _shardKeyPattern;

    // Default collation to use for routing data queries for this collection
    const std::unique_ptr<CollatorInterface> _defaultCollator;

    // Whether the sharding key is unique
    const bool _unique;

    // Map from the max for each chunk to an entry describing the chunk. The union of all chunks'
    // ranges must cover the complete space from [MinKey, MaxKey).
    ChunkMap _chunkMap;

    // Map from shard id to the maximum chunk version for that shard. If a shard contains no
    // chunks, it won't be present in this map.
    ShardVersionMap _shardVersions;

    friend class ChunkManager;
};

// This will be renamed to RoutingTableHistory and the original RoutingTableHistory will be
// ChunkHistoryMap
class ChunkManager : public std::enable_shared_from_this<ChunkManager> {
    ChunkManager(const ChunkManager&) = delete;
    ChunkManager& operator=(const ChunkManager&) = delete;

public:
    ChunkManager(std::shared_ptr<RoutingTableHistory> rt, boost::optional<Timestamp> clusterTime)
        : _rt(std::move(rt)), _clusterTime(std::move(clusterTime)) {}

    /**
     * Returns an increasing number of the reload sequence number of this chunk manager.
     */
    unsigned long long getSequenceNumber() const {
        return _rt->getSequenceNumber();
    }

    const NamespaceString& getns() const {
        return _rt->getns();
    }

    const ShardKeyPattern& getShardKeyPattern() const {
        return _rt->getShardKeyPattern();
    }

    const CollatorInterface* getDefaultCollator() const {
        return _rt->getDefaultCollator();
    }

    bool isUnique() const {
        return _rt->isUnique();
    }

    ChunkVersion getVersion() const {
        return _rt->getVersion();
    }

    ChunkVersion getVersion(const ShardId& shardId) const {
        return _rt->getVersion(shardId);
    }

    template <typename Callable>
    void forEachChunk(Callable&& handler) const {
        _rt->forEachChunk(
            [this, handler = std::forward<Callable>(handler)](const auto& chunkInfo) mutable {
                if (!handler(Chunk{*chunkInfo, _clusterTime}))
                    return false;

                return true;
            });
    }

    int numChunks() const {
        return _rt->numChunks();
    }

    template <typename Callable>
    void forEachOverlappingChunk(const BSONObj& min,
                                 const BSONObj& max,
                                 bool isMaxInclusive,
                                 Callable&& handler) const {
        _rt->forEachOverlappingChunk(
            min,
            max,
            isMaxInclusive,
            [this, handler = std::forward<Callable>(handler)](const auto& chunkInfo) mutable {
                if (!handler(Chunk{*chunkInfo, _clusterTime})) {
                    return false;
                }
                return true;
            });
    }

    /**
     * Returns true if a document with the given "shardKey" is owned by the shard with the given
     * "shardId" in this routing table. If "shardKey" is empty returns false. If "shardKey" is not a
     * valid shard key, the behaviour is undefined.
     */
    bool keyBelongsToShard(const BSONObj& shardKey, const ShardId& shardId) const;

    /**
     * Returns true if any chunk owned by the shard with the given "shardId" overlaps "range".
     */
    bool rangeOverlapsShard(const ChunkRange& range, const ShardId& shardId) const;

    /**
     * Given a shardKey, returns the first chunk which is owned by shardId and overlaps or sorts
     * after that shardKey. If the return value is empty, this means no such chunk exists.
     */
    boost::optional<Chunk> getNextChunkOnShard(const BSONObj& shardKey,
                                               const ShardId& shardId) const;

    /**
     * Given a shard key (or a prefix) that has been extracted from a document, returns the chunk
     * that contains that key.
     *
     * Example: findIntersectingChunk({a : hash('foo')}) locates the chunk for document
     *          {a: 'foo', b: 'bar'} if the shard key is {a : 'hashed'}.
     *
     * If 'collation' is empty, we use the collection default collation for targeting.
     *
     * Throws a DBException with the ShardKeyNotFound code if unable to target a single shard due to
     * collation or due to the key not matching the shard key pattern.
     */
    Chunk findIntersectingChunk(const BSONObj& shardKey,
                                const BSONObj& collation,
                                bool bypassIsFieldHashedCheck = false) const;

    /**
     * Same as findIntersectingChunk, but assumes the simple collation.
     */
    Chunk findIntersectingChunkWithSimpleCollation(const BSONObj& shardKey) const {
        return findIntersectingChunk(shardKey, CollationSpec::kSimpleSpec);
    }

    /**
     * Finds the shard IDs for a given filter and collation. If collation is empty, we use the
     * collection default collation for targeting.
     */
    void getShardIdsForQuery(OperationContext* opCtx,
                             const BSONObj& query,
                             const BSONObj& collation,
                             std::set<ShardId>* shardIds) const;

    /**
     * Returns all shard ids which contain chunks overlapping the range [min, max]. Please note the
     * inclusive bounds on both sides (SERVER-20768).
     */
    void getShardIdsForRange(const BSONObj& min,
                             const BSONObj& max,
                             std::set<ShardId>* shardIds) const;

    /**
     * Returns the ids of all shards on which the collection has any chunks.
     */
    void getAllShardIds(std::set<ShardId>* all) const {
        _rt->getAllShardIds(all);
    }

    /**
     * Returns the number of shards on which the collection has any chunks
     */
    int getNShardsOwningChunks() {
        return _rt->getNShardsOwningChunks();
    }

    // Transforms query into bounds for each field in the shard key
    // for example :
    //   Key { a: 1, b: 1 },
    //   Query { a : { $gte : 1, $lt : 2 },
    //            b : { $gte : 3, $lt : 4 } }
    //   => Bounds { a : [1, 2), b : [3, 4) }
    static IndexBounds getIndexBoundsForQuery(const BSONObj& key,
                                              const CanonicalQuery& canonicalQuery);

    // Collapse query solution tree.
    //
    // If it has OR node, the result could be a superset of the index bounds generated.
    // Since to give a single IndexBounds, this gives the union of bounds on each field.
    // for example:
    //   OR: { a: (0, 1), b: (0, 1) },
    //       { a: (2, 3), b: (2, 3) }
    //   =>  { a: (0, 1), (2, 3), b: (0, 1), (2, 3) }
    static IndexBounds collapseQuerySolution(const QuerySolutionNode* node);

    /**
     * Returns true if, for this shard, the chunks are identical in both chunk managers
     */
    bool compatibleWith(const ChunkManager& other, const ShardId& shard) const {
        return _rt->compatibleWith(*other._rt, shard);
    }

    std::string toString() const {
        return _rt->toString();
    }

    bool uuidMatches(UUID uuid) const {
        return _rt->uuidMatches(uuid);
    }

    auto getRoutingHistory() const {
        return _rt;
    }

    boost::optional<UUID> getUUID() const {
        return _rt->getUUID();
    }

private:
    std::shared_ptr<RoutingTableHistory> _rt;
    boost::optional<Timestamp> _clusterTime;
};

}  // namespace mongo
