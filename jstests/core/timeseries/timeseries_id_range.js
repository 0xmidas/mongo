/**
 * Verifies that the we can internalize match predicates generated by time series rewrites on _id as
 * range scan using a combination of minRecord and maxRecord.
 *
 * @tags: [
 *   # The test assumes no index exists on the time field. shardCollection implicitly creates an
 *   # index.
 *   assumes_unsharded_collection,
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   tenant_migration_incompatible,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getAggPlanStage, getPlanStage} from "jstests/libs/analyze_plan.js";

TimeseriesTest.run((insert) => {
    // These dates will all be inserted into individual buckets.
    const dates = [
        ISODate("2021-04-01T00:00:00.001Z"),
        ISODate("2021-04-02T00:00:00.007Z"),
        ISODate("2021-04-03T00:00:00.005Z"),
        ISODate("2021-04-04T00:00:00.003Z"),
        ISODate("2021-04-05T00:00:00.009Z"),
        ISODate("2021-04-06T00:00:00.008Z"),  // Starting document for $gt & $gte predicates.
        ISODate("2021-04-06T00:00:00.010Z"),
        ISODate("2021-04-06T00:00:00.010Z"),  // Starting document for $lt & $gte predicates.
        ISODate("2021-04-07T00:00:00.006Z"),
        ISODate("2021-04-08T00:00:00.003Z"),
        ISODate("2021-04-09T00:00:00.007Z"),
        ISODate("2021-04-10T00:00:00.002Z"),
    ];

    const coll = db.timeseries_id_range;
    const timeFieldName = "time";

    function init() {
        coll.drop();

        assert.commandWorked(
            db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
    }

    (function testEQ() {
        init();

        let expl = assert.commandWorked(db.runCommand({
            explain: {
                update: "system.buckets.timeseries_id_range",
                updates: [{q: {"_id": dates[5]}, u: {$set: {a: 1}}}]
            }
        }));

        assert(dates[5], getPlanStage(expl, "CLUSTERED_IXSCAN").minRecord);
        assert(dates[5], getPlanStage(expl, "CLUSTERED_IXSCAN").maxRecord);
    })();

    (function testLTE() {
        init();
        // Just for this test, use a more complex pipeline with unwind.
        const pipeline = [{$match: {time: {$lte: dates[7]}}}, {$unwind: '$x'}];
        let res = coll.aggregate(pipeline).toArray();
        assert.eq(0, res.length);

        let expl = coll.explain("executionStats").aggregate(pipeline);
        assert(getAggPlanStage(expl, "CLUSTERED_IXSCAN"), expl);
        assert(getAggPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("maxRecord"), expl);
        assert.eq(0, expl.stages[0].$cursor.executionStats.executionStages.nReturned);

        for (let i = 0; i < dates.length; i++) {
            assert.commandWorked(insert(coll, {_id: i, [timeFieldName]: dates[i], x: [1, 2]}));
        }

        res = coll.aggregate(pipeline).toArray();
        assert.eq(16, res.length);  // 8 documents x 2 unwound array entries per document.

        expl = coll.explain("executionStats").aggregate(pipeline);
        assert.eq(7, expl.stages[0].$cursor.executionStats.totalDocsExamined);
    })();

    (function testLT() {
        init();
        const pipeline = [{$match: {time: {$lt: dates[7]}}}];
        let res = coll.aggregate(pipeline).toArray();
        assert.eq(0, res.length);

        let expl = coll.explain("executionStats").aggregate(pipeline);
        assert(getAggPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("maxRecord"));
        assert.eq(0, expl.stages[0].$cursor.executionStats.executionStages.nReturned);

        for (let i = 0; i < dates.length; i++) {
            assert.commandWorked(insert(coll, {_id: i, [timeFieldName]: dates[i]}));
        }

        res = coll.aggregate(pipeline).toArray();
        assert.eq(6, res.length);

        expl = coll.explain("executionStats").aggregate(pipeline);
        assert.eq(7, expl.stages[0].$cursor.executionStats.totalDocsExamined);
    })();

    (function testGTE() {
        init();
        const pipeline = [{$match: {time: {$gte: dates[5]}}}];
        let res = coll.aggregate(pipeline).toArray();
        assert.eq(0, res.length);

        let expl = coll.explain("executionStats").aggregate(pipeline);
        assert(getAggPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("minRecord"));
        assert.eq(0, expl.stages[0].$cursor.executionStats.executionStages.nReturned);

        for (let i = 0; i < dates.length; i++) {
            assert.commandWorked(insert(coll, {_id: i, [timeFieldName]: dates[i]}));
        }

        res = coll.aggregate(pipeline).toArray();
        assert.eq(7, res.length, coll.explain().aggregate(pipeline));

        expl = coll.explain("executionStats").aggregate(pipeline);
        assert.eq(6, expl.stages[0].$cursor.executionStats.totalDocsExamined);
    })();

    (function testGT() {
        init();
        const pipeline = [{$match: {time: {$gt: dates[5]}}}];
        let res = coll.aggregate(pipeline).toArray();
        assert.eq(0, res.length);

        let expl = coll.explain("executionStats").aggregate(pipeline);
        assert(getAggPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("minRecord"));
        assert.eq(0, expl.stages[0].$cursor.executionStats.executionStages.nReturned);

        for (let i = 0; i < dates.length; i++) {
            assert.commandWorked(insert(coll, {_id: i, [timeFieldName]: dates[i]}));
        }

        res = coll.aggregate(pipeline).toArray();
        assert.eq(6, res.length);

        expl = coll.explain("executionStats").aggregate(pipeline);
        assert.eq(6, expl.stages[0].$cursor.executionStats.totalDocsExamined);
    })();

    (function testRange1() {
        init();

        const pipeline = [{$match: {time: {$gte: dates[5], $lte: dates[7]}}}];
        let res = coll.aggregate(pipeline).toArray();
        assert.eq(0, res.length);

        let expl = coll.explain("executionStats").aggregate(pipeline);
        assert(getAggPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("minRecord"));
        assert(getAggPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("maxRecord"));
        assert.eq(0, expl.stages[0].$cursor.executionStats.executionStages.nReturned);

        for (let i = 0; i < dates.length; i++) {
            assert.commandWorked(insert(coll, {_id: i, [timeFieldName]: dates[i]}));
        }

        res = coll.aggregate(pipeline).toArray();
        assert.eq(3, res.length);

        expl = coll.explain("executionStats").aggregate(pipeline);
        assert.eq(3, expl.stages[0].$cursor.executionStats.totalDocsExamined, expl);
    })();

    (function testRange2() {
        init();

        const pipeline = [{$match: {time: {$gt: dates[5], $lt: dates[7]}}}];
        let res = coll.aggregate(pipeline).toArray();
        assert.eq(0, res.length);

        let expl = coll.explain("executionStats").aggregate(pipeline);
        assert(getAggPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("minRecord"));
        assert(getAggPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("maxRecord"));
        assert.eq(0, expl.stages[0].$cursor.executionStats.executionStages.nReturned);

        for (let i = 0; i < dates.length; i++) {
            assert.commandWorked(insert(coll, {_id: i, [timeFieldName]: dates[i]}));
        }

        res = coll.aggregate(pipeline).toArray();
        assert.eq(0, res.length);

        expl = coll.explain("executionStats").aggregate(pipeline);
        assert.eq(3, expl.stages[0].$cursor.executionStats.totalDocsExamined);
    })();

    (function testRange3() {
        init();

        const pipeline = [{$match: {time: {$lt: dates[5], $gt: dates[7]}}}];
        let res = coll.aggregate(pipeline).toArray();
        assert.eq(0, res.length);

        let expl = coll.explain("executionStats").aggregate(pipeline);
        assert(getAggPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("minRecord"));
        assert(getAggPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("maxRecord"));
        assert.eq(0, expl.stages[0].$cursor.executionStats.executionStages.nReturned);

        for (let i = 0; i < dates.length; i++) {
            assert.commandWorked(insert(coll, {_id: i, [timeFieldName]: dates[i]}));
        }

        res = coll.aggregate(pipeline).toArray();
        assert.eq(0, res.length);

        expl = coll.explain("executionStats").aggregate(pipeline);
        assert.eq(3, expl.stages[0].$cursor.executionStats.totalDocsExamined);
    })();

    (function testRange4() {
        init();

        const pipeline = [{$match: {time: {$gte: dates[3], $gt: dates[5], $lt: dates[7]}}}];
        let res = coll.aggregate(pipeline).toArray();
        assert.eq(0, res.length);

        let expl = coll.explain("executionStats").aggregate(pipeline);
        assert(getAggPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("minRecord"));
        assert(getAggPlanStage(expl, "CLUSTERED_IXSCAN").hasOwnProperty("maxRecord"));
        assert.eq(0, expl.stages[0].$cursor.executionStats.executionStages.nReturned);

        for (let i = 0; i < dates.length; i++) {
            assert.commandWorked(insert(coll, {_id: i, [timeFieldName]: dates[i]}));
        }

        res = coll.aggregate(pipeline).toArray();
        assert.eq(0, res.length);

        expl = coll.explain("executionStats").aggregate(pipeline);
        assert.eq(3, expl.stages[0].$cursor.executionStats.totalDocsExamined);
    })();
});