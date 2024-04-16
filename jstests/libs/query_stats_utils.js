import {resultsEq} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

export const kShellApplicationName = "MongoDB Shell";
export const kDefaultQueryStatsHmacKey = BinData(8, "MjM0NTY3ODkxMDExMTIxMzE0MTUxNjE3MTgxOTIwMjE=");

/**
 * Utility for checking that the aggregated queryStats metrics are logical (follows sum >= max >=
 * min, and sum = max = min if only one execution).
 */
export function verifyMetrics(batch) {
    batch.forEach(element => {
        function skipMetric(summaryValues) {
            // Skip over fields that aren't aggregated metrics with sum/min/max (execCount,
            // lastExecutionMicros). We also skip metrics that are aggregated, but that haven't
            // yet been updated (i.e have a sum of 0). This can happen on occasion when new metrics
            // are not yet in use, or are guarded by a feature flag. Instead, it may be better to
            // keep a list of the metrics we expect to see here in the long term for a more
            // consistent check.
            return summaryValues.sum === undefined ||
                (typeof summaryValues.sum === "object" && summaryValues.sum.valueOf() === 0);
        }
        if (element.metrics.execCount === 1) {
            for (const [metricName, summaryValues] of Object.entries(element.metrics)) {
                if (skipMetric(summaryValues)) {
                    continue;
                }
                const debugInfo = {[metricName]: summaryValues};
                // If there has only been one execution, all metrics should have min, max, and sum
                // equal to each other.
                assert.eq(summaryValues.sum, summaryValues.min, debugInfo);
                assert.eq(summaryValues.sum, summaryValues.max, debugInfo);
                assert.eq(summaryValues.min, summaryValues.max, debugInfo);
            }
        } else {
            for (const [metricName, summaryValues] of Object.entries(element.metrics)) {
                if (skipMetric(summaryValues)) {
                    continue;
                }
                const debugInfo = {[metricName]: summaryValues};
                assert.gte(summaryValues.sum, summaryValues.min, debugInfo);
                assert.gte(summaryValues.sum, summaryValues.max, debugInfo);
                assert.lte(summaryValues.min, summaryValues.max, debugInfo);
            }
        }
    });
}

/**
 * Return the latest query stats entry from the given collection. Only returns query shapes
 * generated by the shell that is running tests.
 *
 * @param conn - connection to database
 * @param {object} options {
 *  {String} collName - name of collection
 *  {object} - extraMatch - optional argument that can be used to filter the pipeline
 * }
 */
export function getLatestQueryStatsEntry(conn, options = {
    collName: ""
}) {
    let sortedEntries = getQueryStats(
        conn, Object.merge({customSort: {"metrics.latestSeenTimestamp": -1}}, options));
    assert.neq([], sortedEntries);
    return sortedEntries[0];
}

/**
 * Collect query stats from a given collection. Only include query shapes generated by the shell
 * that is running tests.
 *
 * @param conn - connection to database
 * @param {object} options {
 *  {String} collName - name of collection
 *  {object} - extraMatch - optional argument that can be used to filter the pipeline
 *  {object} - customSort - optional custom sort order - otherwise sorted by 'key' just to be
 * deterministic.
 * }
 */
export function getQueryStats(conn, options = {
    collName: ""
}) {
    let match = {"key.client.application.name": kShellApplicationName, ...options.extraMatch};
    if (options.collName && options.collName) {
        match["key.queryShape.cmdNs.coll"] = options.collName;
    }
    const result = conn.adminCommand({
        aggregate: 1,
        pipeline: [{$queryStats: {}}, {$sort: (options.customSort || {key: 1})}, {$match: match}],
        cursor: {}
    });
    assert.commandWorked(result);
    return result.cursor.firstBatch;
}

/**
 * @param {object} conn - connection to database
 * @param {object} options {
 *  {BinData} hmacKey
 *  {String} collName - name of collection
 *  {boolean} transformIdentifiers - whether to include transform identifiers
 * }
 */
export function getQueryStatsFindCmd(conn, options = {
    collName: "",
    transformIdentifiers: false,
    hmacKey: kDefaultQueryStatsHmacKey
}) {
    let matchExpr = {
        "key.queryShape.command": "find",
        "key.client.application.name": kShellApplicationName
    };
    if (options.collName) {
        matchExpr["key.queryShape.cmdNs.coll"] = options.collName;
    }
    // Filter out agg queries, including $queryStats.
    var pipeline;
    if (options.transformIdentifiers) {
        pipeline = [
            {
                $queryStats: {
                    transformIdentifiers: {
                        algorithm: "hmac-sha-256",
                        hmacKey: options.hmacKey ? options.hmacKey : kDefaultQueryStatsHmacKey
                    }
                }
            },
            {$match: matchExpr},
            // Sort on queryStats key so entries are in a deterministic order.
            {$sort: {key: 1}},
        ];
    } else {
        pipeline = [
            {$queryStats: {}},
            {$match: matchExpr},
            // Sort on queryStats key so entries are in a deterministic order.
            {$sort: {key: 1}},
        ];
    }
    const result = conn.adminCommand({aggregate: 1, pipeline: pipeline, cursor: {}});
    assert.commandWorked(result);
    return result.cursor.firstBatch;
}

/**
 * Collects query stats from any aggregate command query shapes (with $queryStats requests filtered
 * out) that were generated by the shell that is running tests.
 *
 * /**
 * @param {object} conn - connection to database
 * @param {object} options {
 *  {BinData} hmacKey
 *  {boolean} transformIdentifiers - whether to include transform identifiers
 * }
 */
export function getQueryStatsAggCmd(db, options = {
    transformIdentifiers: false,
    hmacKey: kDefaultQueryStatsHmacKey
}) {
    var pipeline;
    let queryStatsStage = {$queryStats: {}};
    if (options.transformIdentifiers) {
        queryStatsStage = {
            $queryStats: {
                transformIdentifiers: {
                    algorithm: "hmac-sha-256",
                    hmacKey: options.hmacKey ? options.hmacKey : kDefaultQueryStatsHmacKey
                }
            }
        };
    }

    pipeline = [
        queryStatsStage,
        // Filter out find queries and $queryStats aggregations.
        {
            $match: {
                "key.queryShape.command": "aggregate",
                "key.queryShape.pipeline.0.$queryStats": {$exists: false},
                "key.client.application.name": kShellApplicationName
            }
        },
        // Sort on key so entries are in a deterministic order.
        {$sort: {key: 1}},
    ];
    return db.getSiblingDB("admin").aggregate(pipeline).toArray();
}

export function confirmAllExpectedFieldsPresent(expectedKey, resultingKey) {
    let fieldsCounter = 0;
    for (const field in resultingKey) {
        fieldsCounter++;
        if (field === "client") {
            // client meta data is environment/machine dependent, so do not
            // assert on fields or specific fields other than the application name.
            assert.eq(resultingKey.client.application.name, kShellApplicationName);
            // SERVER-83926 We should never report the "mongos" section, since it is too specific
            // and would result in too many different query shapes.
            assert(!resultingKey.client.hasOwnProperty("mongos"), resultingKey.client);
            continue;
        }
        if (!expectedKey.hasOwnProperty(field)) {
            print("Field present in actual object but missing from expected: " + field);
            print("Expected " + tojson(expectedKey));
            print("Actual " + tojson(resultingKey));
        }
        assert(expectedKey.hasOwnProperty(field), field);

        if (field == "otherNss") {
            // When otherNss contains multiple namespaces, the order is not always consistent,
            // so use resultsEq to get stable results.
            assert(resultsEq(expectedKey[field], resultingKey[field]));
            continue;
        }
        assert.eq(expectedKey[field], resultingKey[field]);
    }

    // Make sure the resulting key isn't missing any fields.
    assert.eq(fieldsCounter,
              Object.keys(expectedKey).length,
              "Query Shape Key is missing or has extra fields: " + tojson(resultingKey));
}

export function assertExpectedResults(results,
                                      expectedQueryStatsKey,
                                      expectedExecCount,
                                      expectedDocsReturnedSum,
                                      expectedDocsReturnedMax,
                                      expectedDocsReturnedMin,
                                      expectedDocsReturnedSumOfSq,
                                      getMores) {
    const {key, keyHash, metrics, asOf} = results;
    confirmAllExpectedFieldsPresent(expectedQueryStatsKey, key);
    assert.eq(expectedExecCount, metrics.execCount);
    assert.docEq({
        sum: NumberLong(expectedDocsReturnedSum),
        max: NumberLong(expectedDocsReturnedMax),
        min: NumberLong(expectedDocsReturnedMin),
        sumOfSquares: NumberLong(expectedDocsReturnedSumOfSq)
    },
                 metrics.docsReturned);

    const {
        firstSeenTimestamp,
        latestSeenTimestamp,
        lastExecutionMicros,
        totalExecMicros,
        firstResponseExecMicros,
        workingTimeMillis,
    } = metrics;

    // The tests can't predict exact timings, so just assert these three fields have been set (are
    // non-zero).
    assert.neq(lastExecutionMicros, NumberLong(0));
    assert.neq(firstSeenTimestamp.getTime(), 0);
    assert.neq(latestSeenTimestamp.getTime(), 0);
    assert.neq(asOf.getTime(), 0);
    assert.neq(keyHash.length, 0);

    const distributionFields = ['sum', 'max', 'min', 'sumOfSquares'];
    for (const field of distributionFields) {
        assert.neq(totalExecMicros[field], NumberLong(0));
        assert.neq(firstResponseExecMicros[field], NumberLong(0));
        assert.gte(workingTimeMillis[field], NumberLong(0));
        if (metrics.execCount > 1) {
            // If there are prior executions of the same query shape, we can't be certain if those
            // runs had getMores or not, so we can only check totalExec >= firstResponse.
            assert.gte(totalExecMicros[field], firstResponseExecMicros[field]);
        } else if (getMores) {
            // If there are getMore calls, totalExecMicros fields should be greater than or equal to
            // firstResponseExecMicros.
            if (field == 'min' || field == 'max') {
                // In the case that we've executed multiple queries with the same shape, it is
                // possible for the min or max to be equal.
                assert.gte(totalExecMicros[field], firstResponseExecMicros[field]);
            } else {
                assert.gt(totalExecMicros[field], firstResponseExecMicros[field]);
            }
        } else {
            // If there are no getMore calls, totalExecMicros fields should be equal to
            // firstResponseExecMicros.
            assert.eq(totalExecMicros[field], firstResponseExecMicros[field]);
        }
    }
}

export function assertAggregatedMetric(results, metricName, {sum, min, max, sumOfSq}) {
    const {key, metrics, asOf} = results;

    assert.docEq({
        sum: NumberLong(sum),
        max: NumberLong(max),
        min: NumberLong(min),
        sumOfSquares: NumberLong(sumOfSq)
    },
                 metrics[metricName],
                 `Metric: ${metricName}`);
}

export function assertAggregatedBoolean(results, metricName, {trueCount, falseCount}) {
    const {key, metrics, asOf} = results;

    assert.docEq({
        "true": NumberLong(trueCount),
        "false": NumberLong(falseCount),
    },
                 metrics[metricName],
                 `Metric: ${metricName}`);
}

export function assertAggregatedMetricsSingleExec(
    results,
    {docsExamined, keysExamined, usedDisk, hasSortStage, fromPlanCache, fromMultiPlanner}) {
    const numericMetric = (x) => ({sum: x, min: x, max: x, sumOfSq: x ** 2});
    assertAggregatedMetric(results, "docsExamined", numericMetric(docsExamined));
    assertAggregatedMetric(results, "keysExamined", numericMetric(keysExamined));

    const booleanMetric = (x) => ({trueCount: x ? 1 : 0, falseCount: x ? 0 : 1});
    assertAggregatedBoolean(results, "usedDisk", booleanMetric(usedDisk));
    assertAggregatedBoolean(results, "hasSortStage", booleanMetric(hasSortStage));
    assertAggregatedBoolean(results, "fromPlanCache", booleanMetric(fromPlanCache));
    assertAggregatedBoolean(results, "fromMultiPlanner", booleanMetric(fromMultiPlanner));
}

export function asFieldPath(str) {
    return "$" + str;
}

export function asVarRef(str) {
    return "$$" + str;
}

export function resetQueryStatsStore(conn, queryStatsStoreSize) {
    // Set the cache size to 0MB to clear the queryStats store, and then reset to
    // queryStatsStoreSize.
    assert.commandWorked(conn.adminCommand({setParameter: 1, internalQueryStatsCacheSize: "0MB"}));
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, internalQueryStatsCacheSize: queryStatsStoreSize}));
}

/**
 *  Checks that the given object contains the dottedPath.
 * @param {object} object
 * @param {string} dottedPath
 */
function hasValueAtPath(object, dottedPath) {
    let nestedFields = dottedPath.split(".");
    for (const nestedField of nestedFields) {
        if (!object.hasOwnProperty(nestedField)) {
            return false
        }
        object = object[nestedField];
    }
    return true;
}

/**
 *  Returns the object's value at the dottedPath.
 * @param {object} object
 * @param {string} dottedPath
 */
export function getValueAtPath(object, dottedPath) {
    let nestedFields = dottedPath.split(".");
    for (const nestedField of nestedFields) {
        if (!object.hasOwnProperty(nestedField)) {
            return false
        }
        object = object[nestedField];
    }
    return object;
}

/**
 * Runs an assertion callback function on a node with query stats enabled - once with a mongod, and
 * once with a mongos.
 * @param {String} collName - The desired collection name to use. The db will be "test".
 * @param {Function} callbackFn - The function to make the assertion on each connection.
 */
export function withQueryStatsEnabled(collName, callbackFn) {
    const options = {
        setParameter: {internalQueryStatsRateLimit: -1},
    };

    {
        const conn = MongoRunner.runMongod(options);
        const testDB = conn.getDB("test");
        var coll = testDB[collName];
        coll.drop();

        callbackFn(coll);
        MongoRunner.stopMongod(conn);
    }

    {
        const st = new ShardingTest({shards: 2, mongosOptions: options});
        const testDB = st.getDB("test");
        var coll = testDB[collName];
        st.shardColl(coll, {_id: 1}, {_id: 1});

        callbackFn(coll);
        st.stop();
    }
}
/**
 * We run the command on an new database with an empty collection that has an index {v:1}. We then
 * obtain the queryStats key entry that is created and check the following things.
 * 1. The command associated with the key matches the commandName.
 * 2. The fields nested inside of queryShape field of the key exactly matches those given by
 * shapeFields.
 * 3. The list of fields of the key exactly matches those given by keyFields.
 * /**
 * @param {string} commandName -  string name of type of command, ex. "find" or "aggregate"
 * @param {object} commandObj - The command that will be  run
 * @param {object} shapeFields - List of fields that are part of the queryShape and should be nested
 *     inside of Query Shape
 * @param {object} keyFields - List of outer fields not nested inside queryShape but should be part
 *     of the key
 */
export function runCommandAndValidateQueryStats(
    {coll, commandName, commandObj, shapeFields, keyFields}) {
    const testDB = coll.getDB();
    assert.commandWorked(testDB.runCommand(commandObj));
    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});

    assert.eq(entry.key.queryShape.command, commandName);
    const kApplicationName = "MongoDB Shell";
    assert.eq(entry.key.client.application.name, kApplicationName);

    {
        assert(hasValueAtPath(entry, "key.client.driver"), entry);
        assert(hasValueAtPath(entry, "key.client.driver.name"), entry);
        assert(hasValueAtPath(entry, "key.client.driver.version"), entry);
    }

    {
        assert(hasValueAtPath(entry, "key.client.os"), entry);
        assert(hasValueAtPath(entry, "key.client.os.type"), entry);
        assert(hasValueAtPath(entry, "key.client.os.name"), entry);
        assert(hasValueAtPath(entry, "key.client.os.architecture"), entry);
        assert(hasValueAtPath(entry, "key.client.os.version"), entry);
    }

    // SERVER-83926 Make sure the mongos section doesn't show up.
    assert(!hasValueAtPath(entry, "key.client.mongos"), entry);

    // Every path in shapeFields is in the queryShape.
    let shapeFieldsPrefixes = [];
    for (const field of shapeFields) {
        assert(hasValueAtPath(entry.key.queryShape, field),
               `QueryShape: ${tojson(entry.key.queryShape)} is missing field ${field}`);
        shapeFieldsPrefixes.push(field.split(".")[0]);
    }

    // Every field in queryShape is in shapeFields or is the base of a path in shapeFields.
    for (const field in entry.key.queryShape) {
        assert(shapeFieldsPrefixes.includes(field),
               `Unexpected field ${field} in shape for ${commandName}`);
    }

    // Every path in keyFields is in the key.
    let keyFieldsPrefixes = [];
    for (const field of keyFields) {
        if (field === "collectionType" && FixtureHelpers.isMongos(testDB)) {
            // TODO SERVER-76263 collectionType is not yet available on mongos.
            continue;
        }
        assert(hasValueAtPath(entry.key, field),
               `Key: ${tojson(entry.key)} is missing field ${field}`);
        keyFieldsPrefixes.push(field.split(".")[0]);
    }

    // Every field in the key is in keyFields or is the base of a path in keyFields.
    for (const field in entry.key) {
        assert(keyFieldsPrefixes.includes(field),
               `Unexpected field ${field} in key for ${commandName}`);
    }

    // $hint can only be string(index name) or object (index spec).
    assert.throwsWithCode(() => {
        coll.find({v: {$eq: 2}}).hint({'v': 60, $hint: -128}).itcount();
    }, ErrorCodes.BadValue);
}

/**
 * Helper function to verify that each of the query stats entries has a unique hash and returns a
 * list of the hashes.
 * @param {list} entries - List of entries returned from $queryStats.
 * @returns {list} list of unique hashes corresponding to the entries.
 */
export function getQueryStatsKeyHashes(entries) {
    const keyHashes = {};
    for (const entry of entries) {
        assert(entry.keyHash && entry.keyHash !== "",
               `Entry does not have a 'keyHash' field: ${tojson(entry)}`);
        keyHashes[entry.keyHash] = entry;
    }
    // We expect all keys and hashes to be unique, so assert that we have as many unique hashes as
    // entries.
    const keyHashArray = Object.keys(keyHashes);
    assert.eq(keyHashArray.length, entries.length, tojson(entries));
    return keyHashArray;
}

/**
 * Helper function to construct a query stats key for a find query.
 */
export function getFindQueryStatsKey(conn, collName, queryShapeExtra, extra = {}) {
    // This is most of the query stats key. There are configuration-specific details that are
    // added conditionally afterwards.
    const baseQueryShape = {
        cmdNs: {db: "test", coll: collName},
        command: "find",
    };
    const baseStatsKey = {
        queryShape: Object.assign(baseQueryShape, queryShapeExtra),
        client: {application: {name: "MongoDB Shell"}},
        batchSize: "?number",
    };
    const queryStatsKey = Object.assign(baseStatsKey, extra);

    const coll = conn.getDB("test")[collName];
    if (conn.isMongos()) {
        queryStatsKey.readConcern = {level: "local", provenance: "implicitDefault"};
    } else {
        // TODO SERVER-76263 - make this apply to mongos once it has collection telemetry info.
        queryStatsKey.collectionType = isView(conn, coll) ? "view" : "collection";
    }

    if (FixtureHelpers.isReplSet(conn.getDB("test"))) {
        queryStatsKey["$readPreference"] = {mode: "secondaryPreferred"};
    }

    return queryStatsKey;
}

/**
 * Helper function to construct a query stats key for an aggregate query.
 */
export function getAggregateQueryStatsKey(conn, collName, queryShapeExtra, extra = {}) {
    const testDB = conn.getDB("test");
    const coll = testDB[collName];
    const baseQueryShape = {cmdNs: {db: "test", coll: collName}, command: "aggregate"};
    const baseStatsKey = {
        queryShape: Object.assign(baseQueryShape, queryShapeExtra),
        client: {application: {name: "MongoDB Shell"}},
        cursor: {batchSize: "?number"},
    };

    if (!conn.isMongos()) {
        // TODO SERVER-76263 - make this apply to mongos once it has collection telemetry info.
        baseStatsKey.collectionType = collName == "$cmd.aggregate" ? "virtual"
            : isView(conn, coll)                                   ? "view"
                                                                   : "collection";
    }

    if (FixtureHelpers.isReplSet(conn.getDB("test"))) {
        baseStatsKey["$readPreference"] = {mode: "secondaryPreferred"};
    }

    const queryStatsKey = Object.assign(baseStatsKey, extra);

    return queryStatsKey;
}

function getQueryStatsForNs(testDB, namespace) {
    return getQueryStats(testDB, {
        collName: namespace,
        extraMatch: {"key.queryShape.pipeline.0.$queryStats": {$exists: false}}
    });
}

function getSingleQueryStatsEntryForNs(testDB, namespace) {
    const queryStats = getQueryStatsForNs(testDB, namespace);
    assert.lte(queryStats.length, 1, "Multiple query stats entries found for " + namespace);
    assert.eq(queryStats.length, 1, "No query stats entries found for " + namespace);
    return queryStats[0];
}

function getExecCount(testDB, namespace) {
    const queryStats = getQueryStatsForNs(testDB, namespace);
    assert.lte(queryStats.length, 1, "Multiple query stats entries found for " + namespace);
    return queryStats.length == 0 ? 0 : queryStats[0].metrics.execCount;
}

/**
 * Clears the plan cache and query stats store so they are in a known (empty) state to prevent
 * stats and plan caches from leaking between queries.
 */
export function clearPlanCacheAndQueryStatsStore(conn, coll) {
    const testDB = conn.getDB("test");
    // If this is a query against a view, we need to reset the plan cache for the underlying
    // collection.
    const viewSource = getViewSource(conn, coll);
    const collName = viewSource ? viewSource : coll.getName();

    // Reset the query stats store and plan cache so the recorded stats will be consistent.
    resetQueryStatsStore(conn, "1%");
    assert.commandWorked(testDB.runCommand({planCacheClear: collName}));
}

function getViewSource(conn, coll) {
    const testDB = conn.getDB("test");
    const collectionInfos = testDB.getCollectionInfos({name: coll.getName()});
    assert.eq(collectionInfos.length, 1, "Couldn't find collection info for " + coll.getName());
    const collectionInfo = collectionInfos[0];
    if (collectionInfo.type == "view") {
        return collectionInfo.options.viewOn;
    }
    return null;
}

function isView(conn, coll) {
    return getViewSource(conn, coll) !== null;
}

/**
 * Given a command and batch size, runs the command and enough getMores to exhaust the cursor,
 * returning the query stats. Asserts that the expected number of documents is ultimately returned,
 * and asserts that query stats are written as expected.
 */
export function exhaustCursorAndGetQueryStats(conn, coll, cmd, key, expectedDocs) {
    const testDB = conn.getDB("test");

    // Set up the namespace and the batch size - it goes in different fields for find vs. aggregate.
    // For the namespace, it's usually the collection name, but in the case of aggregate: 1 queries,
    // it will be "$cmd.aggregate".
    let namespace = 1;
    let batchSize = 0;
    if (cmd.hasOwnProperty("find")) {
        assert(cmd.hasOwnProperty("batchSize"), "find command missing batchSize");
        batchSize = cmd.batchSize;
        namespace = cmd.find;
    } else if (cmd.hasOwnProperty("aggregate")) {
        assert(cmd.hasOwnProperty("cursor"), "aggregate command missing cursor");
        assert(cmd.cursor.hasOwnProperty("batchSize"), "aggregate command missing batchSize");
        batchSize = cmd.cursor.batchSize;
        namespace = cmd.aggregate == 1 ? "$cmd.aggregate" : cmd.aggregate;
    } else {
        assert(false, "Unexpected command type");
    }

    const execCountPre = getExecCount(testDB, namespace);

    jsTestLog("Running command with batch size " + batchSize + ": " + tojson(cmd));
    const initialResult = assert.commandWorked(testDB.runCommand(cmd));
    let cursor = initialResult.cursor;
    let allResults = cursor.firstBatch;

    // When batchSize equals expectedDocs, we may or may not get cursor ID 0 in the initial
    // response depending on the exact query. However, if it is strictly greater or less than,
    // we assert on whether or not the initial batch exhausts the cursor.
    const expectGetMores = cursor.id != 0;
    if (batchSize < expectedDocs) {
        assert.neq(0, cursor.id, "Cursor unexpectedly exhausted in initial batch");
    } else if (batchSize > expectedDocs) {
        assert.eq(
            0, cursor.id, "Initial batch unexpectedly wasn't sufficient to exhaust the cursor");
    }

    while (cursor.id != 0) {
        // Since the cursor isn't exhausted, ensure no query stats results have been written.
        const execCount = getExecCount(testDB, namespace);
        assert.eq(execCount, execCountPre);

        const getMore = {getMore: cursor.id, collection: namespace, batchSize: batchSize};
        jsTestLog("Running getMore with batch size " + batchSize + ": " + tojson(getMore));
        const getMoreResult = assert.commandWorked(testDB.runCommand(getMore));
        cursor = getMoreResult.cursor;
        allResults = allResults.concat(cursor.nextBatch);
    }

    assert.eq(allResults.length, expectedDocs);

    const execCountPost = getExecCount(testDB, namespace);
    assert.eq(execCountPost, execCountPre + 1, "Didn't find query stats for namespace" + namespace);

    const queryStats = getSingleQueryStatsEntryForNs(conn, namespace);
    print("Query Stats: " + tojson(queryStats));

    assertExpectedResults(queryStats,
                          key,
                          /* expectedExecCount */ execCountPost,
                          /* expectedDocsReturnedSum */ expectedDocs * execCountPost,
                          /* expectedDocsReturnedMax */ expectedDocs,
                          /* expectedDocsReturnedMin */ expectedDocs,
                          /* expectedDocsReturnedSumOfSq */ expectedDocs ** 2 * execCountPost,
                          expectGetMores);

    return queryStats;
}

/**
 * Run the given callback for each of the deployment scenarios we want to test query stats against
 * (standalone, replset, sharded cluster). Callback must accept two arguments: the connection and
 * the test object (ReplSetTest or ShardingTest). For the standalone case, the test is null.
 */
export function runForEachDeployment(callbackFn) {
    const options = {setParameter: {internalQueryStatsRateLimit: -1}};

    {
        const conn = MongoRunner.runMongod(options);

        callbackFn(conn, null);

        MongoRunner.stopMongod(conn);
    }

    {
        const rst = new ReplSetTest({nodes: 3, nodeOptions: options});
        rst.startSet();
        rst.initiate();

        callbackFn(rst.getPrimary(), rst);

        rst.stopSet();
    }

    {
        const st = new ShardingTest(Object.assign({shards: 2, other: {mongosOptions: options}}));

        const testDB = st.s.getDB("test");
        // Enable sharding separate from per-test setup to avoid calling enableSharding repeatedly.
        assert.commandWorked(testDB.adminCommand(
            {enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));

        callbackFn(st.s, st);

        st.stop();
    }
}
