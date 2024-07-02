load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers
const kShellApplicationName = "MongoDB Shell";
const kDefaultQueryStatsHmacKey = BinData(8, "MjM0NTY3ODkxMDExMTIxMzE0MTUxNjE3MTgxOTIwMjE=");

/**
 * Utility for checking that the aggregated queryStats metrics are logical (follows sum >= max >=
 * min, and sum = max = min if only one execution).
 */
function verifyMetrics(batch) {
    batch.forEach(element => {
        if (element.metrics.execCount === 1) {
            for (const [metricName, summaryValues] of Object.entries(element.metrics)) {
                // Skip over fields that aren't aggregated metrics with sum/min/max (execCount,
                // lastExecutionMicros).
                if (summaryValues.sum === undefined) {
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
                // Skip over fields that aren't aggregated metrics with sum/min/max (execCount,
                // lastExecutionMicros).
                if (summaryValues.sum === undefined) {
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
function getLatestQueryStatsEntry(conn, options = {
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
function getQueryStats(conn, options = {
    collName: ""
}) {
    let match =
        Object.assign({"key.client.application.name": kShellApplicationName}, options.extraMatch);
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
function getQueryStatsFindCmd(conn, options = {
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
function getQueryStatsAggCmd(db, options = {
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

function confirmAllExpectedFieldsPresent(expectedKey, resultingKey) {
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
        assert.eq(expectedKey[field], resultingKey[field]);
    }
    // Make sure the resulting key isn't missing any fields.
    assert.eq(fieldsCounter, Object.keys(expectedKey).length, resultingKey);
}

function assertExpectedResults(results,
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
        firstResponseExecMicros
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
        if (getMores) {
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

function asFieldPath(str) {
    return "$" + str;
}

function asVarRef(str) {
    return "$$" + str;
}

function resetQueryStatsStore(conn, queryStatsStoreSize) {
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
            return false;
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
function getValueAtPath(object, dottedPath) {
    let nestedFields = dottedPath.split(".");
    for (const nestedField of nestedFields) {
        if (!object.hasOwnProperty(nestedField)) {
            return false;
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
function withQueryStatsEnabled(collName, callbackFn) {
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
        coll.drop();
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
function runCommandAndValidateQueryStats({coll, commandName, commandObj, shapeFields, keyFields}) {
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
function getQueryStatsKeyHashes(entries) {
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
 * Given a query stats entry, and stats that the entry should have, this function checks that the
 * entry is the result of a change stream request and that the metrics are what are expected.
 */
function checkChangeStreamEntry({queryStatsEntry, db, collectionName, numExecs, numDocsReturned}) {
    assert.eq(collectionName, queryStatsEntry.key.queryShape.cmdNs.coll);

    // Confirm entry is a change stream request.
    let stringifiedPipeline = JSON.stringify(queryStatsEntry.key.queryShape.pipeline, null, 0);
    assert(stringifiedPipeline.includes("_internalChangeStream"));

    // TODO SERVER-76263 Support reporting 'collectionType' on a sharded cluster.
    if (!FixtureHelpers.isMongos(db)) {
        assert.eq("changeStream", queryStatsEntry.key.collectionType);
    }

    // Checking that metrics match expected metrics.
    assert.eq(queryStatsEntry.metrics.execCount, numExecs);
    assert.eq(queryStatsEntry.metrics.docsReturned.sum, numDocsReturned);

    // FirstResponseExecMicros and TotalExecMicros match since each getMore is recorded as a new
    // first response.
    assert.eq(queryStatsEntry.metrics.totalExecMicros.sum,
              queryStatsEntry.metrics.firstResponseExecMicros.sum);
    assert.eq(queryStatsEntry.metrics.totalExecMicros.max,
              queryStatsEntry.metrics.firstResponseExecMicros.max);
    assert.eq(queryStatsEntry.metrics.totalExecMicros.min,
              queryStatsEntry.metrics.firstResponseExecMicros.min);
}

/**
 * Given a change stream cursor, this function will return the number of getMores executed until the
 * change stream is updated. This only applies when the cursor is waiting for one new document (or
 * set the batchSize of the input cursor to 1) so each hasNext() call will correspond to an internal
 * getMore.
 */
function getNumberOfGetMoresUntilNextDocForChangeStream(cursor) {
    let numGetMores = 0;
    assert.soon(() => {
        numGetMores++;
        return cursor.hasNext();
    });

    // Get the document that is on the cursor to reset the cursor to a state where calling hasNext()
    // corresponds to a getMore.
    cursor.next();
    return numGetMores;
}
