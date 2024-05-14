/**
 * Tests the compatibility of v1 and v2 resume tokens across server version upgrade / downgrade.
 * @tags: [uses_change_streams]
 */

load("jstests/multiVersion/libs/multi_cluster.js");  // For
                                                     // 'ShardingTest.[upgrade|downgrade]Cluster().
load("jstests/libs/collection_drop_recreate.js");    // For 'assert[Drop|Create]Collection()'.

const kLargeStringSize = 15 * 1024 * 1024;

const st = new ShardingTest({
    shards: 2,
    config: 1,
    other: {
        mongosOptions: {binVersion: "last-lts"},
        configOptions: {
            binVersion: "last-lts",
        },
        rsOptions: {
            binVersion: "last-lts",
        },
        rs: {nodes: 2},
    }
});

let testDB = st.s.getDB(jsTestName());
let testColl = testDB["test"];

// Helper function to retrieve high-watermark change stream tokens.
function getHighWaterMarkToken(collection, pipeline = [], options = {}) {
    const csCursor = collection.watch(pipeline, {batchSize: 0, ...options});
    const result = csCursor.getResumeToken();
    csCursor.close();
    return result;
}

// Record a high-watermark resume token marking the start point of the test.
const testStartV1HWMToken = getHighWaterMarkToken(testColl);

// An array which will list the expected sequence of change events generated by the test.
const expectedEvents = [];

//
// Below, we generate one of each type of change event so that we can later test resuming from a v1
// token representing each such event.
//
testColl = assertCreateCollection(testDB, testColl.getName());
expectedEvents.push({operationType: "create"});

assert.commandWorked(testColl.createIndexes([{shard: 1}, {shard: 1, _id: 1}, {largeField: 1}]));
expectedEvents.push({operationType: "createIndexes"},
                    {operationType: "createIndexes"},
                    {operationType: "createIndexes"});

// The 'modify' event has to come before sharding the collection, otherwise we get different number
// of events on 'last-lts' and on 'latest'.
assert.commandWorked(testDB.runCommand(
    {collMod: testColl.getName(), changeStreamPreAndPostImages: {enabled: true}}));
expectedEvents.push({operationType: "modify"});

// Shard the test collection and split it into two chunks: one that contains all {shard: 1}
// documents and one that contains all {shard: 2} documents.
st.shardColl(testColl, {shard: 1} /* shard key */, {shard: 2} /* split at */);
expectedEvents.push({operationType: "shardCollection"});

assert.commandWorked(testColl.insertMany([
    {_id: "a", shard: 1, largeField: ""},
    {_id: "b", shard: 2, largeField: ""},
    {_id: "c", shard: 2, largeField: ""}
]));
expectedEvents.push({operationType: "insert", documentKey: {shard: 1, _id: "a"}},
                    {operationType: "insert", documentKey: {shard: 2, _id: "b"}},
                    {operationType: "insert", documentKey: {shard: 2, _id: "c"}});

// This high watermark token will be at the same clusterTime as the subsequent update event that
// needs to be split.
const v1HwmTokenBeforeUpdate = getHighWaterMarkToken(testColl);

assert.commandWorked(
    testColl.update({_id: "a", shard: 1}, {$set: {largeField: "x".repeat(kLargeStringSize)}}));
expectedEvents.push({operationType: "update", documentKey: {_id: "a", shard: 1}});

assert.commandWorked(
    testColl.update({_id: "b", shard: 2}, {$set: {largeField: "x".repeat(kLargeStringSize)}}));
expectedEvents.push({operationType: "update", documentKey: {_id: "b", shard: 2}});

assert.commandWorked(testColl.replaceOne(
    {_id: "a", shard: 1}, {_id: "a", shard: 1, largeField: "y".repeat(kLargeStringSize)}));
expectedEvents.push({operationType: "replace", documentKey: {_id: "a", shard: 1}});

assert.commandWorked(testColl.replaceOne(
    {_id: "b", shard: 2}, {_id: "b", shard: 2, largeField: "y".repeat(kLargeStringSize)}));
expectedEvents.push({operationType: "replace", documentKey: {_id: "b", shard: 2}});

assert.commandWorked(testColl.remove({_id: "a"}));
expectedEvents.push({operationType: "delete", documentKey: {_id: "a", shard: 1}});

assert.commandWorked(testColl.remove({_id: "b"}));
expectedEvents.push({operationType: "delete", documentKey: {_id: "b", shard: 2}});

// Produces no events on v6.0.
assert.commandWorked(
    st.s.adminCommand({refineCollectionShardKey: testColl.getFullName(), key: {shard: 1, _id: 1}}));

assert.commandWorked(st.s.adminCommand({reshardCollection: testColl.getFullName(), key: {_id: 1}}));
expectedEvents.push({operationType: "reshardCollection"});

assert.commandWorked(testColl.dropIndex({largeField: 1}));
expectedEvents.push({operationType: "dropIndexes"}, {operationType: "dropIndexes"});

const newTestCollectionName = "test_";
assert.commandWorked(testColl.renameCollection(newTestCollectionName));
expectedEvents.push({operationType: "rename"}, {operationType: "rename"});

assertDropCollection(testDB, newTestCollectionName);
expectedEvents.push({operationType: "drop"}, {operationType: "drop"});

assert.commandWorked(testDB.dropDatabase());
// A whole-DB stream will be invalidated by the dropDatabase event. We include a second dropDatabase
// event because one such event is generated on each shard, and will be reported if we resume after
// the invalidate. This second dropDatabase acts as a sentinel here, signifying that we have reached
// the end of the test stream.
expectedEvents.push({operationType: "dropDatabase"},
                    {operationType: "invalidate"},
                    {operationType: "dropDatabase"});

// Leave only one of two catalog events when they have identical resume tokens, because the
// second event will be skipped when resuming from the first event's token in such a case.
// TODO SERVER-90023: Remove this workaround when no longer needed.
{
    const csCursor = testDB.watch([], {
        showExpandedEvents: true,
        startAfter: testStartV1HWMToken,
        $_generateV2ResumeTokens: false
    });

    // The 'drop', 'dropIndexes' and 'rename' events coming from different shards may are likely to
    // get identical resume tokens in v6.0.
    for (let prevEvent = csCursor.next(), nextEvent; csCursor.hasNext(); prevEvent = nextEvent) {
        nextEvent = csCursor.next();
        if (bsonWoCompare(nextEvent._id, prevEvent._id) === 0) {
            // If two or more consecutive events have identical resume tokens, remove all but the
            // last from from the expected events.
            expectedEvents.splice(expectedEvents.findIndex(
                                      (event) => (event.operationType === prevEvent.operationType)),
                                  1);
        }
    }

    csCursor.close();
}

// Helper function to assert on the given event fields.
function assertEventMatches(event, expectedEvent, errorMsg) {
    for (const k in expectedEvent) {
        assert.docEq(expectedEvent[k], event[k], errorMsg + `: value mismatch for field '${k}'`);
    }
}

// Asserts the next change event with the given pipeline and options matches the expected event.
// Returns the resume token of the matched event on success.
function assertNextChangeEvent(expectedEvent, pipeline, options) {
    const csCursor = testDB.watch([...pipeline], {showExpandedEvents: true, ...options});
    const errorMsg = "could not retrieve the expected event matching " + tojson(expectedEvent);
    assert.doesNotThrow(() => assert.soon(() => csCursor.hasNext()), [], errorMsg);
    const event = csCursor.next();
    assertEventMatches(event, expectedEvent, errorMsg);
    csCursor.close();
    return event._id;
}

// Helper function to retrieve change event tokens for all events referred by 'expectedEvents'.
function getTokensForExpectedEvents(expectedEvents, startToken, pipeline = [], options = {}) {
    return expectedEvents
        .reduce(
            (result, expectedEvent) => {
                const lastToken = result[result.length - 1];
                result.push(assertNextChangeEvent(
                    expectedEvent, pipeline, {startAfter: lastToken, ...options}));
                return result;
            },
            [startToken])
        .slice(1);
}

// Generate v1 resume tokens for all expected events on 'last-lts'.
const resumeTokensLastLTS = getTokensForExpectedEvents(expectedEvents, testStartV1HWMToken, []);
// TODO SERVER-82330: Validate that these tokens are indeed all v1 tokens.

// Upgrade the cluster to 'latest' to allow testing v1 - v2 resume behaviour.
st.upgradeCluster("latest", {waitUntilStable: true});
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

testDB = st.s.getDB(jsTestName());

// Verify that we can resume from each of the v1 tokens on the new binary version and with the
// $changeStreamSplitLargeEvent stage in the pipeline. When resuming from (i-1)-th event's token we
// expect to get the i-th event. We do not need to test the last token, because it is simply a
// sentinel value that signifies the end of the test.
for (let i = 1; i < expectedEvents.length; ++i) {
    assertNextChangeEvent(expectedEvents[i],
                          [{$changeStreamSplitLargeEvent: {}}],
                          {startAfter: resumeTokensLastLTS[i - 1]});
}

// Test that we can split the update events after 'v1HwmTokenBeforeUpdate'.
const expectedSplitEvents = [
    {operationType: "update", splitEvent: {fragment: 1, of: 2}},
    {splitEvent: {fragment: 2, of: 2}},
    {operationType: "update", splitEvent: {fragment: 1, of: 2}},
    {splitEvent: {fragment: 2, of: 2}},
    {operationType: "replace"}
];
// Generate a set of v2 resume tokens for each of the entries in 'expectedSplitEvents'.
const resumeTokensWithSplitLatest = getTokensForExpectedEvents(expectedSplitEvents,
                                                               v1HwmTokenBeforeUpdate,
                                                               [{$changeStreamSplitLargeEvent: {}}],
                                                               {fullDocument: "required"});

// Downgrade back to the original version.
assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
st.downgradeCluster("last-lts", {waitUntilStable: true});

testDB = st.s.getDB(jsTestName());

// Test the v2 split 'update' event tokens on the downgraded binary version. When resuming from
// (i-1)-th event token we expect to get the i-th event. This means, after the last 'update' event
// we expect the first 'replace' event.
for (let i = 1; i < expectedSplitEvents.length; ++i) {
    assertNextChangeEvent(
        expectedSplitEvents[i],
        [{$changeStreamSplitLargeEvent: {}}],
        {resumeAfter: resumeTokensWithSplitLatest[i - 1], fullDocument: "required"});
}

st.stop();
