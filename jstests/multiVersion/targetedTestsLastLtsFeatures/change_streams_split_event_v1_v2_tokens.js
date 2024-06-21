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
        rs: {
            nodes: 2,
            // Reserving enough of oplog space to accommodate 4 nearly 16MB-large changes.
            oplogSize: 16 * 5
        }
    }
});

let testDB = st.s.getDB(jsTestName());
let testColl = testDB["test"];

// Helper function to retrieve high-watermark change stream tokens.
function getHighWaterMarkToken(collection, pipeline = [], options = {}) {
    const csCursor = collection.watch(pipeline, Object.assign({batchSize: 0}, options));
    const result = csCursor.getResumeToken();
    csCursor.close();
    return result;
}

// Record a high-watermark resume token marking the start point of the test.
const testStartV1HWMToken = getHighWaterMarkToken(testColl, []);

// An array which will list the expected sequence of change events generated by the test.
const expectedEvents = [];

//
// Below, we generate one of each type of change event so that we can later test resuming from a v1
// token representing each such event.
//

// Produces no events on v5.0.
testColl = assertCreateCollection(testDB, testColl.getName());

// Produces no events on v5.0.
assert.commandWorked(testColl.createIndexes([{shard: 1}, {shard: 1, _id: 1}, {largeField: 1}]));

// Shard the test collection and split it into two chunks: one that contains all {shard: 1}
// documents and one that contains all {shard: 2} documents.
// Produces no events on v5.0.
st.shardColl(testColl, {shard: 1} /* shard key */, {shard: 2} /* split at */);

assert.commandWorked(testColl.insertMany([
    {_id: "a", shard: 1, largeField: ""},
    {_id: "b", shard: 2, largeField: ""},
    {_id: "c", shard: 2, largeField: ""}
]));
expectedEvents.push({operationType: "insert", documentKey: {_id: "a"}},
                    {operationType: "insert", documentKey: {_id: "b"}},
                    {operationType: "insert", documentKey: {_id: "c"}});

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

// Produces no events on v5.0.
assert.commandWorked(
    st.s.adminCommand({refineCollectionShardKey: testColl.getFullName(), key: {shard: 1, _id: 1}}));

// Produces no events on v5.0.
assert.commandWorked(st.s.adminCommand({reshardCollection: testColl.getFullName(), key: {_id: 1}}));

// Produces no events on v5.0.
assert.commandWorked(testColl.dropIndex({largeField: 1}));

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

// Leave only the last of the events with identical resume tokens, because the previous events will
// be skipped when resuming from such a token.
// TODO SERVER-90266: Remove this workaround when no longer needed.
{
    const csCursor = testDB.watch([], {startAfter: testStartV1HWMToken});

    // The 'drop' and 'rename' events coming from different shards are likely to get identical
    // resume tokens in v5.0.
    for (let prevEvent = csCursor.next(), nextEvent; csCursor.hasNext(); prevEvent = nextEvent) {
        nextEvent = csCursor.next();
        if (bsonWoCompare(nextEvent._id, prevEvent._id) === 0) {
            // If two or more consecutive events have identical resume tokens, remove all but the
            // last from the expected events.
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
    const csCursor = testDB.watch([...pipeline], options);
    const errorMsg = "could not retrieve the expected event matching " + tojson(expectedEvent);
    let event;
    assert.doesNotThrow(() => assert.soon(() => csCursor.hasNext()), [], errorMsg);
    event = csCursor.next();
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
                    expectedEvent, pipeline, Object.assign({startAfter: lastToken}, options)));
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
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

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
// Confirm that resuming after v1HwmTokenBeforeUpdate generates the expected series of split events.
// We artificially add a large field into the pipeline because we were unable to record pre-images
// on 5.0 and we cannot look up the current post-image because the collection has been dropped.
getTokensForExpectedEvents(expectedSplitEvents, v1HwmTokenBeforeUpdate, [
    {$addFields: {largeField2: "x".repeat(kLargeStringSize)}},
    {$changeStreamSplitLargeEvent: {}}
]);

st.stop();
