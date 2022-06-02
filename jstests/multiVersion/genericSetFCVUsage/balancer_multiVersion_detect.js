//
// Test checks whether the balancer correctly detects a mixed set of shards
//

// Test mixed version between "latest" and "last-lts"/"last-continuous".
var versions = ["latest", "last-lts"];
jsTest.log("Starting cluster with shard binVersion: " + tojson(versions));

var options = {
    mongosOptions: {verbose: 1, useLogFiles: true},
    configOptions: {},
    shardOptions: {binVersion: versions},
    enableBalancer: true
};

var st = new ShardingTest({shards: 3, mongos: 1, other: options});

var mongos = st.s0;
var admin = mongos.getDB("admin");
var coll = mongos.getCollection("foo.bar");

printjson(admin.runCommand({enableSharding: coll.getDB() + ""}));
st.ensurePrimaryShard(coll.getDB().getName(), st.shard1.shardName);
printjson(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));

assert.soon(function() {
    var log = cat(mongos.fullOptions.logFile);
    return /multiVersion cluster detected/.test(log);
}, "multiVersion warning not printed!", 30 * 16 * 60 * 1000, 5 * 1000);

st.stop();

jsTest.log("DONE!");
