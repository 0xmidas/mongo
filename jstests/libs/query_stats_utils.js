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
    const {key, metrics} = results;
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
function runCommandAndValidateQueryStats({commandName, commandObj, shapeFields, keyFields}) {
    let options = {
        setParameter: {internalQueryStatsRateLimit: -1},
    };

    const conn = MongoRunner.runMongod(options);
    const testDB = conn.getDB("test");
    var coll = testDB[jsTestName()];
    coll.drop();

    // Have to create an index for hint not to fail.
    assert.commandWorked(coll.createIndex({v: 1}));

    assert.commandWorked(testDB.runCommand(commandObj));
    let stats = getQueryStats(conn);
    assert.eq(1, stats.length);

    for (const entry of stats) {
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
            assert(hasValueAtPath(entry.key, field),
                   `Key: ${tojson(entry.key)} is missing field ${field}`);
            keyFieldsPrefixes.push(field.split(".")[0]);
        }

        // Every field in the key is in keyFields or is the base of a path in keyFields.
        for (const field in entry.key) {
            assert(keyFieldsPrefixes.includes(field),
                   `Unexpected field ${field} in key for ${commandName}`);
        }
    }
    // $hint can only be string(index name) or object (index spec).
    assert.throwsWithCode(() => {
        coll.find({v: {$eq: 2}}).hint({'v': 60, $hint: -128}).itcount();
    }, ErrorCodes.BadValue);
    MongoRunner.stopMongod(conn);
}
