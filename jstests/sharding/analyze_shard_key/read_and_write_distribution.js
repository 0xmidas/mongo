/**
 * Tests that the on sharded clusters the analyzeShardKey command returns read and write
 * distribution metrics, but on replica sets it does not since query sampling is only supported on
 * sharded clusters at this point.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

const calculatePercentage = AnalyzeShardKeyUtil.calculatePercentage;
const assertApprox = AnalyzeShardKeyUtil.assertApprox;

function sum(nums) {
    return nums.reduce((partialSum, num) => partialSum + num);
}

function assertReadMetricsEmptySampleSize(actual) {
    const expected = {sampleSize: {total: 0, find: 0, aggregate: 0, count: 0, distinct: 0}};
    return assert.eq(bsonWoCompare(actual, expected), 0, {actual, expected});
}

function assertWriteMetricsEmptySampleSize(actual) {
    const expected = {sampleSize: {total: 0, update: 0, delete: 0, findAndModify: 0}};
    return assert.eq(bsonWoCompare(actual, expected), 0, {actual, expected});
}

function assertReadMetricsNonEmptySampleSize(actual, expected, isHashed) {
    assert.eq(actual.sampleSize.total, expected.sampleSize.total, {actual, expected});
    assert.eq(actual.sampleSize.find, expected.sampleSize.find, {actual, expected});
    assert.eq(actual.sampleSize.aggregate, expected.sampleSize.aggregate, {actual, expected});
    assert.eq(actual.sampleSize.count, expected.sampleSize.count, {actual, expected});
    assert.eq(actual.sampleSize.distinct, expected.sampleSize.distinct, {actual, expected});

    assertApprox(actual.percentageOfSingleShardReads,
                 calculatePercentage(expected.numSingleShard, expected.sampleSize.total),
                 {actual, expected});
    assertApprox(actual.percentageOfMultiShardReads,
                 calculatePercentage(expected.numMultiShard, expected.sampleSize.total),
                 {actual, expected});
    assertApprox(actual.percentageOfScatterGatherReads,
                 calculatePercentage(expected.numScatterGather, expected.sampleSize.total),
                 {actual, expected});

    assert.eq(actual.numReadsByRange.length, analyzeShardKeyNumRanges, {actual, expected});
    if (isHashed) {
        assert.eq(actual.percentageOfMultiShardReads, 0, {actual, expected});
        assert.eq(sum(actual.numReadsByRange),
                  expected.numSingleShard + expected.numScatterGather * analyzeShardKeyNumRanges,
                  {actual, expected});
    } else {
        assert.gte(sum(actual.numReadsByRange),
                   expected.numSingleShard + expected.numMultiShard +
                       expected.numScatterGather * analyzeShardKeyNumRanges,
                   {actual, expected});
    }
}

function assertWriteMetricsNonEmptySampleSize(actual, expected, isHashed) {
    assert.eq(actual.sampleSize.total, expected.sampleSize.total, {actual, expected});
    assert.eq(actual.sampleSize.update, expected.sampleSize.update, {actual, expected});
    assert.eq(actual.sampleSize.delete, expected.sampleSize.delete, {actual, expected});
    assert.eq(
        actual.sampleSize.findAndModify, expected.sampleSize.findAndModify, {actual, expected});

    assertApprox(actual.percentageOfSingleShardWrites,
                 calculatePercentage(expected.numSingleShard, expected.sampleSize.total),
                 {actual, expected});
    assertApprox(actual.percentageOfMultiShardWrites,
                 calculatePercentage(expected.numMultiShard, expected.sampleSize.total),
                 {actual, expected});
    assertApprox(actual.percentageOfScatterGatherWrites,
                 calculatePercentage(expected.numScatterGather, expected.sampleSize.total),
                 {actual, expected});

    assert.eq(actual.numWritesByRange.length, analyzeShardKeyNumRanges, {actual, expected});
    if (isHashed) {
        assert.eq(actual.percentageOfMultiShardWrites, 0, {actual, expected});
        assert.eq(sum(actual.numWritesByRange),
                  expected.numSingleShard + expected.numScatterGather * analyzeShardKeyNumRanges,
                  {actual, expected});
    } else {
        assert.gte(sum(actual.numWritesByRange),
                   expected.numSingleShard + expected.numMultiShard +
                       expected.numScatterGather * analyzeShardKeyNumRanges,
                   {actual, expected});
    }

    assertApprox(actual.percentageOfShardKeyUpdates,
                 calculatePercentage(expected.numShardKeyUpdates, expected.sampleSize.total),
                 {actual, expected});
    assertApprox(
        actual.percentageOfSingleWritesWithoutShardKey,
        calculatePercentage(expected.numSingleWritesWithoutShardKey, expected.sampleSize.total),
        {actual, expected});
    assertApprox(
        actual.percentageOfMultiWritesWithoutShardKey,
        calculatePercentage(expected.numMultiWritesWithoutShardKey, expected.sampleSize.total),
        {actual, expected});
}

function assertMetricsEmptySampleSize(actual) {
    AnalyzeShardKeyUtil.assertContainReadWriteDistributionMetrics(actual);
    assertReadMetricsEmptySampleSize(actual.readDistribution);
    assertWriteMetricsEmptySampleSize(actual.writeDistribution);
}

function assertMetricsNonEmptySampleSize(actual, expected, isHashed) {
    AnalyzeShardKeyUtil.assertContainReadWriteDistributionMetrics(actual);
    assertReadMetricsNonEmptySampleSize(
        actual.readDistribution, expected.readDistribution, isHashed);
    assertWriteMetricsNonEmptySampleSize(
        actual.writeDistribution, expected.writeDistribution, isHashed);
}

function assertSoonNoConfigSplitPointDocuments(conn) {
    const coll = conn.getCollection("config.analyzeShardKeySplitPoints");

    let numTries;
    assert.soon(() => {
        numTries++;
        if (numTries % 100 == 0) {
            const docs = coll.find().toArray();
            jsTest.log("Waiting for the spit point documents to get deleted " +
                       tojson({numTries, docs}));
        }
        return coll.find().itcount() === 0;
    });
}

const readCmdNames = ["find", "aggregate", "count", "distinct"];
function isReadCmdObj(cmdObj) {
    const cmdName = Object.keys(cmdObj)[0];
    return readCmdNames.includes(cmdName);
}

function getRandomCount() {
    return AnalyzeShardKeyUtil.getRandInteger(1, 100);
}

function makeTestCase(sampledCollName,
                      notSampledCollName,
                      {candidateShardKeyField, currentShardKeyField, isHashed, minVal, maxVal}) {
    // Generate commands and populate the expected metrics.
    const cmdObjs = [];

    const usedVals = new Set();
    const getNextVal = () => {
        while (usedVals.size < (maxVal + 1 - minVal)) {
            const val = AnalyzeShardKeyUtil.getRandInteger(minVal, maxVal);
            if (!usedVals.has(val)) {
                usedVals.add(val);
                return val;
            }
        }
        throw new Error("No unused values left");
    };

    const readDistribution = {
        sampleSize: {total: 0, find: 0, aggregate: 0, count: 0, distinct: 0},
        numSingleShard: 0,
        numMultiShard: 0,
        numScatterGather: 0
    };
    const writeDistribution = {
        sampleSize: {total: 0, update: 0, delete: 0, findAndModify: 0},
        numSingleShard: 0,
        numMultiShard: 0,
        numScatterGather: 0,
        numShardKeyUpdates: 0,
        numSingleWritesWithoutShardKey: 0,
        numMultiWritesWithoutShardKey: 0
    };

    // Below are reads targeting a single shard.

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({find: sampledCollName, filter: {[candidateShardKeyField]: getNextVal()}});
        readDistribution.sampleSize.find++;
        readDistribution.sampleSize.total++;
        readDistribution.numSingleShard++;
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({
            aggregate: sampledCollName,
            pipeline: [{$match: {[candidateShardKeyField]: getNextVal()}}],
            cursor: {}
        });
        readDistribution.sampleSize.aggregate++;
        readDistribution.sampleSize.total++;
        readDistribution.numSingleShard++;
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({
            aggregate: sampledCollName,
            pipeline: [{$match: {$expr: {$eq: ["$" + candidateShardKeyField, "$$value"]}}}],
            cursor: {},
            let : {value: getNextVal()}
        });
        readDistribution.sampleSize.aggregate++;
        readDistribution.sampleSize.total++;
        readDistribution.numSingleShard++;
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({
                aggregate: notSampledCollName,
                pipeline: [{
                    $lookup: {
                        from: sampledCollName,
                        let : {value: getNextVal()},
                        pipeline: [{$match: {$expr: {$eq: ["$" + candidateShardKeyField, "$$value"]}}}],
                        as: "joined"
                    }
                }],
                cursor: {}
            });
        readDistribution.sampleSize.aggregate++;
        readDistribution.sampleSize.total++;
        readDistribution.numSingleShard++;
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({count: sampledCollName, query: {[candidateShardKeyField]: getNextVal()}});
        readDistribution.sampleSize.count++;
        readDistribution.sampleSize.total++;
        readDistribution.numSingleShard++;
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push(
            {distinct: sampledCollName, key: "x", query: {[candidateShardKeyField]: getNextVal()}});
        readDistribution.sampleSize.distinct++;
        readDistribution.sampleSize.total++;
        readDistribution.numSingleShard++;
    }

    // Below are reads targeting a variable number of shards.

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push(
            {find: sampledCollName, filter: {[candidateShardKeyField]: {$gte: getNextVal()}}});
        readDistribution.sampleSize.find++;
        readDistribution.sampleSize.total++;
        if (isHashed) {
            // For hashed sharding, range queries on the shard key target all shards.
            readDistribution.numScatterGather++;
        } else {
            readDistribution.numMultiShard++;
        }
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({
            aggregate: sampledCollName,
            pipeline: [{$match: {[candidateShardKeyField]: {$lt: getNextVal()}}}],
            cursor: {}
        });
        readDistribution.sampleSize.aggregate++;
        readDistribution.sampleSize.total++;
        if (isHashed) {
            // For hashed sharding, range queries on the shard key target all shards.
            readDistribution.numScatterGather++;
        } else {
            readDistribution.numMultiShard++;
        }
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push(
            {count: sampledCollName, query: {[candidateShardKeyField]: {$lte: getNextVal()}}});
        readDistribution.sampleSize.count++;
        readDistribution.sampleSize.total++;
        if (isHashed) {
            // For hashed sharding, range queries on the shard key target all shards.
            readDistribution.numScatterGather++;
        } else {
            readDistribution.numMultiShard++;
        }
    }

    // Below are reads targeting all shards.

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({find: sampledCollName, filter: {}});
        readDistribution.sampleSize.find++;
        readDistribution.sampleSize.total++;
        readDistribution.numScatterGather++;
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({
            aggregate: sampledCollName,
            pipeline: [{$match: {$expr: {$eq: ["$ts", "$$NOW"]}}}],
            cursor: {}
        });
        readDistribution.sampleSize.aggregate++;
        readDistribution.sampleSize.total++;
        readDistribution.numScatterGather++;
    }

    let makeWriteFilter = (isMulti, val, op) => {
        if (currentShardKeyField && (currentShardKeyField == candidateShardKeyField)) {
            // For a sharded collection, it is illegal to perform a multi:false write that targets
            // more than one shard. This collection is sharded on the shard key being analyzed, so
            // we cannot test multi:false writes that do not filter by shard key equality since such
            // writes may target more than one shard.
            assert(isMulti || ((val != null) && !op));
        }

        const filter = {};
        if (val != null) {
            filter[candidateShardKeyField] = op ? {[op]: val} : val;
        }
        if (!isMulti && currentShardKeyField && (currentShardKeyField != candidateShardKeyField)) {
            // To guarantee that this multi:false write targets only one shard, add shard key
            // equality to its filter.
            filter[currentShardKeyField] = val ? val : getNextVal();
        }
        return filter;
    };

    // Below are writes targeting a single shard.

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({
            update: sampledCollName,
            updates: [
                {
                    q: makeWriteFilter(false /* isMulti */, getNextVal()),
                    u: {$set: {z: 0}},
                    multi: true
                },
                {
                    q: makeWriteFilter(false /* isMulti */, getNextVal()),
                    u: {$set: {z: 0}},
                },
            ]
        });
        writeDistribution.sampleSize.update += 2;
        writeDistribution.sampleSize.total += 2;
        writeDistribution.numSingleShard += 2;
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({
            delete: sampledCollName,
            deletes: [
                {q: makeWriteFilter(false /* isMulti */, minVal++), limit: 1},
                {q: makeWriteFilter(true /* isMulti */, maxVal--), limit: 0},
            ]
        });
        writeDistribution.sampleSize.delete += 2;
        writeDistribution.sampleSize.total += 2;
        writeDistribution.numSingleShard += 2;
    }

    for (let i = 0; i < getRandomCount(); i++) {
        cmdObjs.push({
            // This is a shard key update.
            findAndModify: sampledCollName,
            query: makeWriteFilter(false /* isMulti */, minVal++),
            update: {$inc: {[candidateShardKeyField]: 1}},
            lsid: {id: UUID()},
            txnNumber: NumberLong(1)
        });
        writeDistribution.sampleSize.findAndModify++;
        writeDistribution.sampleSize.total++;
        writeDistribution.numSingleShard++;
        writeDistribution.numShardKeyUpdates++;
    }

    // For a sharded collection, it is illegal to perform a multi:false write that targets
    // more than one shard. For simplicity, if this collection is sharded on the shard key being
    // analyzed, just skip testing all writes that may target more than one shard.
    if (!currentShardKeyField || (currentShardKeyField != candidateShardKeyField)) {
        // Below are writes targeting a variable number of shards.

        for (let i = 0; i < getRandomCount(); i++) {
            cmdObjs.push({
                update: sampledCollName,
                updates: [{
                    q: makeWriteFilter(false /* isMulti */, getNextVal(), "$gte"),
                    u: {$set: {z: 0}}
                }]
            });
            writeDistribution.sampleSize.update++;
            writeDistribution.sampleSize.total++;
            if (isHashed) {
                // For hashed sharding, range queries on the shard key target all shards.
                writeDistribution.numScatterGather++;
            } else {
                writeDistribution.numMultiShard++;
            }
            writeDistribution.numSingleWritesWithoutShardKey++;
        }

        for (let i = 0; i < getRandomCount(); i++) {
            cmdObjs.push({
                delete: sampledCollName,
                deletes: [{q: makeWriteFilter(true /* isMulti */, minVal++, "$lte"), limit: 0}]
            });
            writeDistribution.sampleSize.delete ++;
            writeDistribution.sampleSize.total++;
            if (isHashed) {
                // For hashed sharding, range queries on the shard key target all shards.
                writeDistribution.numScatterGather++;
            } else {
                writeDistribution.numMultiShard++;
            }
            writeDistribution.numMultiWritesWithoutShardKey++;
        }

        for (let i = 0; i < getRandomCount(); i++) {
            cmdObjs.push({
                findAndModify: sampledCollName,
                query: makeWriteFilter(false /* isMulti */, getNextVal(), "$lte"),
                update: {$set: {z: 0}}
            });
            writeDistribution.sampleSize.findAndModify++;
            writeDistribution.sampleSize.total++;
            if (isHashed) {
                // For hashed sharding, range queries on the shard key target all shards.
                writeDistribution.numScatterGather++;
            } else {
                writeDistribution.numMultiShard++;
            }
            writeDistribution.numSingleWritesWithoutShardKey++;
        }

        // Below are writes targeting all shards.

        for (let i = 0; i < getRandomCount(); i++) {
            cmdObjs.push({
                findAndModify: sampledCollName,
                query: makeWriteFilter(false /* isMulti */),
                update: {$set: {z: 0}}
            });
            writeDistribution.sampleSize.findAndModify++;
            writeDistribution.sampleSize.total++;
            writeDistribution.numScatterGather++;
            writeDistribution.numSingleWritesWithoutShardKey++;
        }
    }

    return {cmdObjs, metrics: {readDistribution, writeDistribution}};
}

/**
 * Repeatedly runs the analyzeShardKey command until the sample size expected by the test case is
 * reached, and returns the last metrics returned.
 */
function waitForSampledQueries(conn, ns, shardKey, testCase) {
    let res;
    let numTries = 0;
    assert.soon(() => {
        numTries++;

        res = assert.commandWorked(conn.adminCommand({analyzeShardKey: ns, key: shardKey}));
        const numShardKeyUpdates = res.writeDistribution.percentageOfShardKeyUpdates *
            res.writeDistribution.sampleSize.total / 100;

        if (numTries % 100 == 0) {
            jsTest.log("Waiting for sampled queries and diffs " + tojsononeline({
                           actual: {
                               readSampleSize: res.readDistribution.sampleSize,
                               writeSampleSize: res.writeDistribution.sampleSize,
                               diffSampleSize: numShardKeyUpdates,
                           },
                           expected: {
                               readSampleSize: testCase.metrics.readDistribution.sampleSize,
                               writeSampleSize: testCase.metrics.writeDistribution.sampleSize,
                               diffSampleSize: testCase.metrics.writeDistribution.numShardKeyUpdates
                           }
                       }));
        }

        return (res.readDistribution.sampleSize.total >=
                testCase.metrics.readDistribution.sampleSize.total) &&
            (res.writeDistribution.sampleSize.total >=
             testCase.metrics.writeDistribution.sampleSize.total) &&
            (numShardKeyUpdates >= testCase.metrics.writeDistribution.numShardKeyUpdates);
    });

    return res;
}

function runTest(fixture, {isShardedColl, candidateShardKeyField, isHashed}) {
    const dbName = "testDb";
    // Make the database have two collections, one with query sampling enabled and one without.
    // The sampled collection is used for testing the analyzeShardKey metrics. The non-sampled
    // collection is used as the local collection when running aggregate commands with $lookup
    // against the sampled collection.
    const sampledCollName = isShardedColl ? "sampledCollSharded" : "sampledCollUnsharded";
    const notSampledCollName = "notSampledColl";
    const sampledNs = dbName + "." + sampledCollName;
    const shardKey = {[candidateShardKeyField]: isHashed ? "hashed" : 1};
    jsTest.log(`Test analyzing the shard key ${tojsononeline(shardKey)} for the collection ${
        tojson({ns: sampledNs})}`);

    const sampledColl = fixture.conn.getDB(dbName).getCollection(sampledCollName);
    const notSampledColl = fixture.conn.getDB(dbName).getCollection(notSampledCollName);

    const currentShardKeyField = fixture.setUpCollectionFn(dbName, sampledCollName, isShardedColl);
    assert.commandWorked(notSampledColl.insert([{a: 0}]));

    // Verify that the analyzeShardKey command fails while calculating the read and write
    // distribution if the cardinality of the shard key is lower than analyzeShardKeyNumRanges.
    assert.commandWorked(sampledColl.insert({[candidateShardKeyField]: 1}));
    assert.commandFailedWithCode(
        fixture.conn.adminCommand({analyzeShardKey: sampledNs, key: shardKey}), 4952606);

    // Insert documents into the sampled collection. The range of values is selected such that the
    // documents will be distributed across all the shards if the collection is sharded.
    const minVal = -1500;
    const maxVal = 1500;
    const docs = [];
    for (let i = minVal; i < maxVal + 1; i++) {
        docs.push({_id: i, x: i, y: i, ts: new Date()});
    }
    assert.commandWorked(sampledColl.insert(docs));
    const sampledCollUuid =
        QuerySamplingUtil.getCollectionUuid(fixture.conn.getDB(dbName), sampledCollName);

    // Verify that the analyzeShardKey command returns zeros for the read and write sample size
    // when there are no sampled queries.
    let res = assert.commandWorked(
        fixture.conn.adminCommand({analyzeShardKey: sampledNs, key: shardKey}));
    assertMetricsEmptySampleSize(res);

    // Turn on query sampling and wait for sampling to become active.
    assert.commandWorked(fixture.conn.adminCommand(
        {configureQueryAnalyzer: sampledNs, mode: "full", samplesPerSecond}));
    fixture.waitForActiveSamplingFn(sampledNs, sampledCollUuid);

    // Create and run test queries.
    const testCase =
        makeTestCase(sampledCollName,
                     notSampledCollName,
                     {candidateShardKeyField, currentShardKeyField, isHashed, minVal, maxVal});

    fixture.runCmdsFn(dbName, testCase.cmdObjs);

    // Turn off query sampling and wait for sampling to become inactive. The wait is necessary for
    // preventing the internal aggregate commands run by the analyzeShardKey commands below from
    // getting sampled.
    assert.commandWorked(
        fixture.conn.adminCommand({configureQueryAnalyzer: sampledNs, mode: "off"}));
    fixture.waitForInactiveSamplingFn(sampledNs, sampledCollUuid);

    res = waitForSampledQueries(fixture.conn, sampledNs, shardKey, testCase);
    // Verify that the metrics are as expected.
    assertMetricsNonEmptySampleSize(res, testCase.metrics, isHashed);

    assert(notSampledColl.drop());
    // Drop the sampled collection without removing its config.sampledQueries and
    // config.sampledQueriesDiff documents to get test coverage for analyzing shard keys for a
    // collection that has gone through multiple incarnations. That is, if the analyzeShardKey
    // command filters those documents by ns instead of collection uuid, it would return
    // incorrect metrics.
    assert(sampledColl.drop());
}

// Make the periodic jobs for refreshing sample rates and writing sampled queries and diffs have a
// period of 1 second to speed up the test.
const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const queryAnalysisWriterIntervalSecs = 1;

const samplesPerSecond = 10000;
const analyzeShardKeyNumRanges = 10;

const mongodSetParameterOpts = {
    queryAnalysisSamplerConfigurationRefreshSecs,
    queryAnalysisWriterIntervalSecs,
    analyzeShardKeyNumRanges,
    logComponentVerbosity: tojson({sharding: 2}),
    // To speed up the test, make the split point documents expire right away. To prevent the split
    // point documents from being deleted while the analyzeShardKey command is still running, make
    // the TTL monitor have a large sleep interval at first and then lower it at the end of the test
    // when verifying that the documents do get deleted by the TTL monitor.
    analyzeShardKeySplitPointExpirationSecs: 1,
    ttlMonitorSleepSecs: 60 * 60,
};
const mongosSetParametersOpts = {
    queryAnalysisSamplerConfigurationRefreshSecs,
    logComponentVerbosity: tojson({sharding: 3})
};

{
    jsTest.log("Verify that on a sharded cluster the analyzeShardKey command returns correct read" +
               " and write distribution metrics");

    const numMongoses = 2;  // Test sampling on multiple mongoses.
    const numShards = 3;

    const st = new ShardingTest({
        mongos: numMongoses,
        shards: numShards,
        rs: {nodes: 2, setParameter: mongodSetParameterOpts},
        mongosOptions: {setParameter: mongosSetParametersOpts}
    });

    // This test expects every query to get sampled regardless of which mongos or mongod routes it.
    st.configRS.nodes.forEach(node => {
        configureFailPoint(node, "queryAnalysisCoordinatorDistributeSamplesPerSecondEqually");
    });

    const fixture = {
        conn: st.s0,
        setUpCollectionFn: (dbName, collName, isShardedColl) => {
            const ns = dbName + "." + collName;
            const currentShardKeyField = "x";

            assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));
            st.ensurePrimaryShard(dbName, st.shard0.name);

            if (isShardedColl) {
                // Set up the sharded collection. Make it have three chunks:
                // shard0: [MinKey, -1000]
                // shard1: [-1000, 1000]
                // shard2: [1000, MaxKey]
                assert.commandWorked(
                    st.s0.adminCommand({shardCollection: ns, key: {[currentShardKeyField]: 1}}));
                assert.commandWorked(
                    st.s0.adminCommand({split: ns, middle: {[currentShardKeyField]: -1000}}));
                assert.commandWorked(
                    st.s0.adminCommand({split: ns, middle: {[currentShardKeyField]: 1000}}));
                assert.commandWorked(st.s0.adminCommand({
                    moveChunk: ns,
                    find: {[currentShardKeyField]: -1000},
                    to: st.shard1.shardName
                }));
                assert.commandWorked(st.s0.adminCommand({
                    moveChunk: ns,
                    find: {[currentShardKeyField]: 1000},
                    to: st.shard2.shardName
                }));
            }

            return currentShardKeyField;
        },
        waitForActiveSamplingFn: (ns, collUuid) => {
            QuerySamplingUtil.waitForActiveSamplingShardedCluster(st, ns, collUuid);
        },
        runCmdsFn: (dbName, cmdObjs) => {
            for (let i = 0; i < cmdObjs.length; i++) {
                const db = st["s" + String(i % numMongoses)].getDB(dbName);
                assert.commandWorked(db.runCommand(cmdObjs[i]));
            }
        },
        waitForInactiveSamplingFn: (ns, collUuid) => {
            QuerySamplingUtil.waitForInactiveSamplingShardedCluster(st, ns, collUuid);
        }
    };

    runTest(fixture, {isShardedColl: false, candidateShardKeyField: "x", isHashed: false});
    runTest(fixture, {isShardedColl: false, candidateShardKeyField: "x", isHashed: true});

    // Note that {x: 1} is the current shard key for the sharded collection being tested.
    runTest(fixture, {isShardedColl: true, candidateShardKeyField: "x", isHashed: false});
    runTest(fixture, {isShardedColl: true, candidateShardKeyField: "x", isHashed: true});
    runTest(fixture, {isShardedColl: true, candidateShardKeyField: "y", isHashed: false});
    runTest(fixture, {isShardedColl: true, candidateShardKeyField: "y", isHashed: true});

    // Verify the split point documents are eventually deleted by the TTL monitor.
    st._rs.forEach(rs => {
        const primary = rs.test.getPrimary();
        assert.commandWorked(primary.adminCommand({setParameter: 1, ttlMonitorSleepSecs: 1}));
        assertSoonNoConfigSplitPointDocuments(primary);
    });

    st.stop();
}

{
    jsTest.log("Verify that on a replica set the analyzeShardKey command returns correct read " +
               "and write distribution metrics");

    const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: mongodSetParameterOpts}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    // This test expects every query to get sampled regardless of which mongod it runs against.
    rst.nodes.forEach(node => {
        configureFailPoint(node, "queryAnalysisCoordinatorDistributeSamplesPerSecondEqually");
    });

    const fixture = {
        conn: primary,
        setUpCollectionFn: (dbName, collName, isShardedColl) => {
            // No setup is needed.
        },
        waitForActiveSamplingFn: (ns, collUuid) => {
            QuerySamplingUtil.waitForActiveSamplingReplicaSet(rst, ns, collUuid);
        },
        runCmdsFn: (dbName, cmdObjs) => {
            for (let i = 0; i < cmdObjs.length; i++) {
                const node = isReadCmdObj(cmdObjs[i]) ? rst.getSecondary() : rst.getPrimary();
                assert.commandWorked(node.getDB(dbName).runCommand(cmdObjs[i]));
            }
        },
        waitForInactiveSamplingFn: (ns, collUuid) => {
            QuerySamplingUtil.waitForInactiveSamplingReplicaSet(rst, ns, collUuid);
        }
    };

    runTest(fixture, {isShardedColl: false, candidateShardKeyField: "x", isHashed: false});
    runTest(fixture, {isShardedColl: false, candidateShardKeyField: "x", isHashed: true});

    // Verify the split point documents are eventually deleted by the TTL monitor.
    assert.commandWorked(primary.adminCommand({setParameter: 1, ttlMonitorSleepSecs: 1}));
    assertSoonNoConfigSplitPointDocuments(primary);

    rst.stopSet();
}
})();
