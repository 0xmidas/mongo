/**
 * Tests catalog shard topology.
 *
 * Requires both catalog shard feature flags.
 * @tags: [
 *   featureFlagCatalogShard,
 *   featureFlagConfigServerAlwaysShardRemote,
 * ]
 */
(function() {
"use strict";

const dbName = "foo";
const collName = "bar";
const ns = dbName + "." + collName;

function basicCRUD(conn) {
    assert.commandWorked(conn.getCollection(ns).insert({_id: 1, x: 1}));
    assert.sameMembers(conn.getCollection(ns).find({x: 1}).toArray(), [{_id: 1, x: 1}]);
    assert.commandWorked(conn.getCollection(ns).remove({x: 1}));
    assert.eq(conn.getCollection(ns).find({x: 1}).toArray().length, 0);
}

const st = new ShardingTest({
    shards: 0,
    config: 3,
    configOptions: {setParameter: {featureFlagCatalogShard: true}},
});
const configCS = st.configRS.getURL();
let configShardName;

//
// Dedicated config server mode tests (pre addShard).
//
{
    // Can't create user namespaces.
    assert.commandFailedWithCode(st.configRS.getPrimary().getCollection(ns).insert({_id: 1, x: 1}),
                                 ErrorCodes.InvalidNamespace);

    // Failover works.
    st.configRS.stepUp(st.configRS.getSecondary());
}

//
// Catalog shard mode tests (post addShard).
//
{
    //
    // Adding the config server as a shard works.
    //
    configShardName = assert.commandWorked(st.s.adminCommand({addShard: configCS})).shardAdded;

    // More than once works.
    assert.commandWorked(st.s.adminCommand({addShard: configCS}));
    assert.commandWorked(st.s.adminCommand({addShard: configCS}));
}

// Refresh the logical session cache now that we have a shard to create the sessions collection to
// verify it works as expected.
st.configRS.getPrimary().adminCommand({refreshLogicalSessionCacheNow: 1});

{
    //
    // Basic unsharded CRUD.
    //
    basicCRUD(st.s);
}

{
    //
    // Basic sharded CRUD.
    //
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {skey: 1}}));

    basicCRUD(st.s);
}

// Add a shard to move chunks to and from it in later tests.
const newShardRS = new ReplSetTest({name: "new-shard-rs", nodes: 1});
newShardRS.startSet({shardsvr: ""});
newShardRS.initiate();
const newShardName =
    assert.commandWorked(st.s.adminCommand({addShard: newShardRS.getURL()})).shardAdded;

{
    //
    // Basic sharded DDL.
    //
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {skey: 0}}));
    assert.commandWorked(st.s.getCollection(ns).insert([{skey: 1}, {skey: -1}]));

    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: 0}, to: newShardName}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: 0}, to: configShardName}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: 0}, to: newShardName}));
}

{
    //
    // Basic secondary reads.
    //
    assert.commandWorked(
        st.s.getCollection(ns).insert({readFromSecondary: 1, skey: -1}, {writeConcern: {w: 3}}));
    let secondaryRes = assert.commandWorked(st.s.getDB(dbName).runCommand({
        find: collName,
        filter: {readFromSecondary: 1, skey: -1},
        $readPreference: {mode: "secondary"}
    }));
    assert.eq(secondaryRes.cursor.firstBatch.length, 1, tojson(secondaryRes));
}

{
    //
    // Failover in shard role works.
    //
    st.configRS.stepUp(st.configRS.getSecondary());

    // Basic CRUD and sharded DDL still works.
    basicCRUD(st.s);
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {skey: 20}}));
}

st.stop();
newShardRS.stopSet();
}());
