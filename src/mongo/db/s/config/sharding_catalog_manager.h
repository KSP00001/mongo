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

#include "mongo/base/status_with.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/s/config/namespace_serializer.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {

struct CollectionOptions;
class OperationContext;
class RemoteCommandTargeter;
class ServiceContext;
class UUID;

struct RemoveShardProgress {

    /**
     * Used to indicate to the caller of the removeShard method whether draining of chunks for
     * a particular shard has started, is ongoing, or has been completed.
     */
    enum DrainingShardStatus {
        STARTED,
        ONGOING,
        COMPLETED,
    };

    /**
     * Used to indicate to the caller of the removeShard method the remaining amount of chunks,
     * jumbo chunks and databases within the shard
     */
    struct DrainingShardUsage {
        const long long totalChunks;
        const long long databases;
        const long long jumboChunks;
    };

    DrainingShardStatus status;
    boost::optional<DrainingShardUsage> remainingCounts;
};

/**
 * Implements modifications to the sharding catalog metadata.
 *
 * TODO: Currently the code responsible for writing the sharding catalog metadata is split between
 * this class and ShardingCatalogClient. Eventually all methods that write catalog data should be
 * moved out of ShardingCatalogClient and into this class.
 */
class ShardingCatalogManager {
    ShardingCatalogManager(const ShardingCatalogManager&) = delete;
    ShardingCatalogManager& operator=(const ShardingCatalogManager&) = delete;
    friend class ConfigSvrShardCollectionCommand;

public:
    ShardingCatalogManager(ServiceContext* serviceContext,
                           std::unique_ptr<executor::TaskExecutor> addShardExecutor);
    ~ShardingCatalogManager();

    /**
     * Indicates the desired modification to the config.chunks and config.tags collections during
     * setFeatureCompatibilityVersion.
     *
     * TODO SERVER-44034: Remove this enum.
     */
    enum class ConfigUpgradeType { kUpgrade, kDowngrade };

    /**
     * Instantiates an instance of the sharding catalog manager and installs it on the specified
     * service context. This method is not thread-safe and must be called only once when the service
     * is starting.
     */
    static void create(ServiceContext* serviceContext,
                       std::unique_ptr<executor::TaskExecutor> addShardExecutor);

    /**
     * Retrieves the per-service instance of the ShardingCatalogManager. This instance is only
     * available if the node is running as a config server.
     */
    static ShardingCatalogManager* get(ServiceContext* serviceContext);
    static ShardingCatalogManager* get(OperationContext* operationContext);

    /**
     * Safe to call multiple times as long as the calls are externally synchronized to be
     * non-overlapping.
     */
    void startup();

    /**
     * Performs necessary cleanup when shutting down cleanly.
     */
    void shutDown();

    //
    // Sharded cluster initialization logic
    //

    /**
     * Checks if this is the first start of a newly instantiated config server and if so pre-creates
     * the catalog collections and their indexes. Also generates and persists the cluster's
     * identity.
     */
    Status initializeConfigDatabaseIfNeeded(OperationContext* opCtx);

    /**
     * Invoked on cluster identity metadata rollback after replication step down. Throws out any
     * cached identity information and causes it to be reloaded/re-created on the next attempt.
     */
    void discardCachedConfigDatabaseInitializationState();

    //
    // Zone Operations
    //

    /**
     * Adds the given shardName to the zone. Returns ErrorCodes::ShardNotFound if a shard by that
     * name does not exist.
     */
    Status addShardToZone(OperationContext* opCtx,
                          const std::string& shardName,
                          const std::string& zoneName);

    /**
     * Removes the given shardName from the zone. Returns ErrorCodes::ShardNotFound if a shard by
     * that name does not exist.
     */
    Status removeShardFromZone(OperationContext* opCtx,
                               const std::string& shardName,
                               const std::string& zoneName);

    /**
     * Assigns a range of a sharded collection to a particular shard zone. If range is a prefix of
     * the shard key, the range will be converted into a new range with full shard key filled with
     * MinKey values.
     */
    Status assignKeyRangeToZone(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const ChunkRange& range,
                                const std::string& zoneName);

    /**
     * Removes a range from a zone.
     *
     * NOTE: unlike assignKeyRangeToZone, the given range will never be converted to include the
     * full shard key.
     */
    Status removeKeyRangeFromZone(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const ChunkRange& range);

    /**
     * Exposes the zone operations mutex to external callers in order to allow them to synchronize
     * with any changes to the zones.
     */
    Lock::ExclusiveLock lockZoneMutex(OperationContext* opCtx);

    //
    // Chunk Operations
    //

    /**
     * Updates metadata in the config.chunks collection to show the given chunk as split into
     * smaller chunks at the specified split points.
     */
    Status commitChunkSplit(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const OID& requestEpoch,
                            const ChunkRange& range,
                            const std::vector<BSONObj>& splitPoints,
                            const std::string& shardName);

    /**
     * Updates metadata in the config.chunks collection so the chunks with given boundaries are seen
     * merged into a single larger chunk.
     * If 'validAfter' is not set, this means the commit request came from an older server version,
     * which is not history-aware.
     */
    Status commitChunkMerge(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const OID& requestEpoch,
                            const std::vector<BSONObj>& chunkBoundaries,
                            const std::string& shardName,
                            const boost::optional<Timestamp>& validAfter);

    /**
     * Updates metadata in config.chunks collection to show the given chunk in its new shard.
     * If 'validAfter' is not set, this means the commit request came from an older server version,
     * which is not history-aware.
     *
     * Returns a BSON object with the newly produced chunk versions after the migration:
     *  - migratedChunkVersion - the version of the chunk, which was migrated
     *  - controlChunkVersion (optional) - the version of the "control" chunk, which was changed in
     *      order to reflect the change on the donor. This value will be missing if the last chunk
     *      on the donor shard was migrated out.
     */
    StatusWith<BSONObj> commitChunkMigration(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const ChunkType& migratedChunk,
                                             const OID& collectionEpoch,
                                             const ShardId& fromShard,
                                             const ShardId& toShard,
                                             const boost::optional<Timestamp>& validAfter);

    /**
     * Removes the jumbo flag from the specified chunk.
     */
    void clearJumboFlag(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const OID& collectionEpoch,
                        const ChunkRange& chunk);
    /**
     * If a chunk matching 'requestedChunk' exists, bumps the chunk's version to one greater than
     * the current collection version.
     */
    void ensureChunkVersionIsGreaterThan(OperationContext* opCtx,
                                         const BSONObj& minKey,
                                         const BSONObj& maxKey,
                                         const ChunkVersion& version);

    //
    // Database Operations
    //

    /**
     * Checks if a database with the same name already exists, and if not, selects a primary shard
     * for the database and creates a new entry for it in config.databases.
     *
     * Returns the database entry.
     *
     * Throws DatabaseDifferCase if the database already exists with a different case.
     */
    DatabaseType createDatabase(OperationContext* opCtx,
                                StringData dbName,
                                const ShardId& primaryShard);

    /**
     * Creates a ScopedLock on the database name in _namespaceSerializer. This is to prevent
     * timeouts waiting on the dist lock if multiple threads attempt to create or drop the same db.
     */
    NamespaceSerializer::ScopedLock serializeCreateOrDropDatabase(OperationContext* opCtx,
                                                                  StringData dbName);

    /**
     * Creates the database if it does not exist, then marks its entry in config.databases as
     * sharding-enabled.
     *
     * Throws DatabaseDifferCase if the database already exists with a different case.
     */
    void enableSharding(OperationContext* opCtx, StringData dbName, const ShardId& primaryShard);

    /**
     * Retrieves all databases for a shard.
     *
     * Returns a !OK status if an error occurs.
     */
    StatusWith<std::vector<std::string>> getDatabasesForShard(OperationContext* opCtx,
                                                              const ShardId& shardId);

    /**
     * Updates metadata in config.databases collection to show the given primary database on its
     * new shard.
     */
    Status commitMovePrimary(OperationContext* opCtx, const StringData nss, const ShardId& toShard);

    //
    // Collection Operations
    //

    /**
     * Drops the specified collection from the collection metadata store.
     *
     * Throws a DBException for any failures. These are some of the known failures:
     *  - NamespaceNotFound - Collection does not exist
     */
    void dropCollection(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Ensures that a namespace that has received a dropCollection, but no longer has an entry in
     * config.collections, has cleared all relevant metadata entries for the corresponding
     * collection. As part of this, sends dropCollection and setShardVersion to all shards -- in
     * case shards didn't receive these commands as part of the original dropCollection.
     *
     * This function does not guarantee that all shards will eventually receive setShardVersion,
     * unless the client infinitely retries until hearing back success. This function does, however,
     * increase the likelihood of shards having received setShardVersion.
     */

    void ensureDropCollectionCompleted(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Iterates through each entry in config.collections that does not have a UUID, generates a UUID
     * for the collection, and updates the entry with the generated UUID.
     *
     * If this function is not necessary for SERVER-33247, it can be removed.
     */
    void generateUUIDsForExistingShardedCollections(OperationContext* opCtx);

    /**
     * Creates a new unsharded collection with the given options.
     *
     * Throws exception on errors.
     */
    void createCollection(OperationContext* opCtx,
                          const NamespaceString& ns,
                          const CollectionOptions& options);

    /**
     * Refines the shard key of an existing collection with namespace 'nss'. Here, 'shardKey'
     * denotes the new shard key, which must contain the old shard key as a prefix.
     *
     * Throws exception on errors.
     */
    void refineCollectionShardKey(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const ShardKeyPattern& newShardKey);

    /**
     * Creates a ScopedLock on the collection name in _namespaceSerializer. This is to prevent
     * timeouts waiting on the dist lock if multiple threads attempt to create or drop the same
     * collection.
     */
    NamespaceSerializer::ScopedLock serializeCreateOrDropCollection(OperationContext* opCtx,
                                                                    const NamespaceString& nss);

    //
    // Shard Operations
    //

    /**
     *
     * Adds a new shard. It expects a standalone mongod process or replica set to be running on the
     * provided address.
     *
     * 'shardProposedName' is an optional string with the proposed name of the shard. If it is
     * nullptr, a name will be automatically generated; if not nullptr, it cannot
     *         contain the empty string.
     * 'shardConnectionString' is the complete connection string of the shard being added.
     * 'maxSize' is the optional space quota in bytes. Zero means there's no limitation to space
     * usage.
     *
     * On success returns the name of the newly added shard.
     */
    StatusWith<std::string> addShard(OperationContext* opCtx,
                                     const std::string* shardProposedName,
                                     const ConnectionString& shardConnectionString,
                                     const long long maxSize);

    /**
     * Tries to remove a shard. To completely remove a shard from a sharded cluster,
     * the data residing in that shard must be moved to the remaining shards in the
     * cluster by "draining" chunks from that shard.
     *
     * Because of the asynchronous nature of the draining mechanism, this method returns
     * the current draining status. See ShardDrainingStatus enum definition for more details.
     */
    RemoveShardProgress removeShard(OperationContext* opCtx, const ShardId& shardId);

    //
    // Cluster Upgrade Operations
    //

    /**
     * Runs the setFeatureCompatibilityVersion command on all shards.
     */
    Status setFeatureCompatibilityVersionOnShards(OperationContext* opCtx, const BSONObj& cmdObj);

    /**
     * Changes the _id format of all documents in config.chunks and config.tags to use either the
     * format introduced in 4.4 or the format expected by a 4.2 binary.
     *
     * TODO SERVER-44034: Remove this method.
     */
    void upgradeOrDowngradeChunksAndTags(OperationContext* opCtx, ConfigUpgradeType upgradeType);

    //
    // For Diagnostics
    //

    /**
     * Append information about the connection pools owned by the CatalogManager.
     */
    void appendConnectionStats(executor::ConnectionPoolStats* stats);

    /**
     * Only used for unit-tests, clears a previously-created catalog manager from the specified
     * service context, so that 'create' can be called again.
     */
    static void clearForTests(ServiceContext* serviceContext);

private:
    /**
     * Performs the necessary checks for version compatibility and creates a new config.version
     * document if the current cluster config is empty.
     */
    Status _initConfigVersion(OperationContext* opCtx);

    /**
     * Builds all the expected indexes on the config server.
     */
    Status _initConfigIndexes(OperationContext* opCtx);

    /**
     * Used during addShard to determine if there is already an existing shard that matches the
     * shard that is currently being added.  An OK return with boost::none indicates that there
     * is no conflicting shard, and we can proceed trying to add the new shard.  An OK return
     * with a ShardType indicates that there is an existing shard that matches the shard being added
     * but since the options match, this addShard request can do nothing and return success.  A
     * non-OK return either indicates a problem reading the existing shards from disk or more likely
     * indicates that an existing shard conflicts with the shard being added and they have different
     * options, so the addShard attempt must be aborted.
     */
    StatusWith<boost::optional<ShardType>> _checkIfShardExists(
        OperationContext* opCtx,
        const ConnectionString& propsedShardConnectionString,
        const std::string* shardProposedName,
        long long maxSize);

    /**
     * Validates that the specified endpoint can serve as a shard server. In particular, this
     * this function checks that the shard can be contacted and that it is not already member of
     * another sharded cluster.
     *
     * @param targeter For sending requests to the shard-to-be.
     * @param shardProposedName Optional proposed name for the shard. Can be omitted in which case
     *      a unique name for the shard will be generated from the shard's connection string. If it
     *      is not omitted, the value cannot be the empty string.
     *
     * On success returns a partially initialized ShardType object corresponding to the requested
     * shard. It will have the hostName field set and optionally the name, if the name could be
     * generated from either the proposed name or the connection string set name. The returned
     * shard's name should be checked and if empty, one should be generated using some uniform
     * algorithm.
     */
    StatusWith<ShardType> _validateHostAsShard(OperationContext* opCtx,
                                               std::shared_ptr<RemoteCommandTargeter> targeter,
                                               const std::string* shardProposedName,
                                               const ConnectionString& connectionString);

    /**
     * Drops the sessions collection on the specified host.
     */
    Status _dropSessionsCollection(OperationContext* opCtx,
                                   std::shared_ptr<RemoteCommandTargeter> targeter);

    /**
     * Runs the listDatabases command on the specified host and returns the names of all databases
     * it returns excluding those named local, config and admin, since they serve administrative
     * purposes.
     */
    StatusWith<std::vector<std::string>> _getDBNamesListFromShard(
        OperationContext* opCtx, std::shared_ptr<RemoteCommandTargeter> targeter);

    /**
     * Runs a command against a "shard" that is not yet in the cluster and thus not present in the
     * ShardRegistry.
     */
    StatusWith<Shard::CommandResponse> _runCommandForAddShard(OperationContext* opCtx,
                                                              RemoteCommandTargeter* targeter,
                                                              StringData dbName,
                                                              const BSONObj& cmdObj);

    /**
     * Helper method for running a count command against the config server with appropriate error
     * handling.
     */
    StatusWith<long long> _runCountCommandOnConfig(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   BSONObj query);

    /**
     * Appends a read committed read concern to the request object.
     */
    void _appendReadConcern(BSONObjBuilder* builder);

    /**
     * Retrieve the full chunk description from the config.
     */
    StatusWith<ChunkType> _findChunkOnConfig(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const BSONObj& key);

    /**
     * Returns true if the zone with the given name has chunk ranges associated with it and the
     * shard with the given name is the only shard that it belongs to.
     */
    StatusWith<bool> _isShardRequiredByZoneStillInUse(OperationContext* opCtx,
                                                      const ReadPreferenceSetting& readPref,
                                                      const std::string& shardName,
                                                      const std::string& zoneName);

    // The owning service context
    ServiceContext* const _serviceContext;

    // Executor specifically used for sending commands to servers that are in the process of being
    // added as shards. Does not have any connection hook set on it, thus it can be used to talk to
    // servers that are not yet in the ShardRegistry.
    const std::unique_ptr<executor::TaskExecutor> _executorForAddShard;

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (M) Must hold _mutex for access.
    // (R) Read only, can only be written during initialization.
    // (S) Self-synchronizing; access in any way from any context.
    //

    Mutex _mutex = MONGO_MAKE_LATCH("ShardingCatalogManager::_mutex");

    // True if shutDown() has been called. False, otherwise.
    bool _inShutdown{false};  // (M)

    // True if startup() has been called.
    bool _started{false};  // (M)

    // True if initializeConfigDatabaseIfNeeded() has been called and returned successfully.
    bool _configInitialized{false};  // (M)

    // Resource lock order:
    // _kShardMembershipLock -> _kChunkOpLock
    // _kZoneOpLock

    /**
     * Lock for shard zoning operations. This should be acquired when doing any operations that
     * can affect the config.tags collection or the tags field of the config.shards collection.
     * No other locks should be held when locking this. If an operation needs to take database
     * locks (for example to write to a local collection) those locks should be taken after
     * taking this.
     */
    Lock::ResourceMutex _kZoneOpLock;

    /**
     * Lock for chunk split/merge/move operations. This should be acquired when doing split/merge/
     * move operations that can affect the config.chunks collection.
     * No other locks should be held when locking this. If an operation needs to take database
     * locks (for example to write to a local collection) those locks should be taken after
     * taking this.
     */
    Lock::ResourceMutex _kChunkOpLock;

    /**
     * Lock that guards changes to the set of shards in the cluster (ie addShard and removeShard
     * requests).
     */
    Lock::ResourceMutex _kShardMembershipLock;

    /**
     * Optimization for DDL operations, which might be tried concurrently by multiple threads.
     * Avoids convoying and timeouts on the database/collection distributed lock.
     */
    NamespaceSerializer _namespaceSerializer;
};

}  // namespace mongo
