'use strict';

/**
 * Sets the internalQueryEnableSlotBasedExecutionEngine flag to true and false, and
 * asserts that find queries using the plan cache produce the correct results.
 *
 * @tags: [
 *     # Our test infrastructure prevents tests which use the 'setParameter' command from running in
 *     # stepdown suites, since parameters are local to each mongod in the replica set.
 *     does_not_support_stepdowns,
 * ]
 */

var $config = (function() {
    let data = {originalParamValue: false};

    function getCollectionName(collName) {
        return "find_flip_sbe_enabled_" + collName;
    }

    function setup(db, collName, cluster) {
        const originalParamValue =
            db.adminCommand({getParameter: 1, internalQueryEnableSlotBasedExecutionEngine: 1});
        assertAlways.commandWorked(originalParamValue);
        assert(originalParamValue.hasOwnProperty("internalQueryEnableSlotBasedExecutionEngine"));
        this.originalParamValue = originalParamValue.internalQueryEnableSlotBasedExecutionEngine;
        const coll = db.getCollection(getCollectionName(collName));
        for (let i = 0; i < 10; ++i) {
            assertAlways.commandWorked(
                coll.insert({_id: i, x: i.toString(), y: i.toString(), z: i.toString()}));
        }

        assertAlways.commandWorked(coll.createIndex({x: 1}));
        assertAlways.commandWorked(coll.createIndex({y: 1}));
    }

    let states = (function() {
        function setEnableSlotBasedExecutionEngineOn(db, collName) {
            assertAlways.commandWorked(db.adminCommand(
                {setParameter: 1, internalQueryEnableSlotBasedExecutionEngine: true}));
        }

        function setEnableSlotBasedExecutionEngineOff(db, collName) {
            assertAlways.commandWorked(db.adminCommand(
                {setParameter: 1, internalQueryEnableSlotBasedExecutionEngine: false}));
        }

        function runQueriesAndCheckResults(db, collName) {
            const coll = db.getCollection(getCollectionName(collName));
            for (let i = 0; i < 10; i++) {
                let res;
                try {
                    res = coll.find({x: i.toString(), y: i.toString(), z: i.toString()}).toArray();
                    assertAlways.eq(res.length, 1);
                    assertAlways.eq(res[0]._id, i);
                } catch (e) {
                    if (e.code !== ErrorCodes.QueryPlanKilled) {
                        throw e;  // This is an unexpected error, so we throw it again.
                    }
                }
            }
        }

        function createIndex(db, collName) {
            const coll = db.getCollection(getCollectionName(collName));
            const res = coll.createIndex({z: 1});
            assertAlways(res.ok === 1 || res.code === ErrorCodes.IndexBuildAlreadyInProgress ||
                             res.code == ErrorCodes.IndexBuildAborted,
                         "Create index failed: " + tojson(res));
        }

        function dropIndex(db, collName) {
            const coll = db.getCollection(getCollectionName(collName));
            const res = coll.dropIndex({z: 1});
            assertAlways(res.ok === 1 || res.code === ErrorCodes.IndexNotFound,
                         "Drop index failed: " + tojson(res));
        }

        return {
            setEnableSlotBasedExecutionEngineOn: setEnableSlotBasedExecutionEngineOn,
            setEnableSlotBasedExecutionEngineOff: setEnableSlotBasedExecutionEngineOff,
            runQueriesAndCheckResults: runQueriesAndCheckResults,
            createIndex: createIndex,
            dropIndex: dropIndex
        };
    })();

    let transitions = {
        setEnableSlotBasedExecutionEngineOn: {
            setEnableSlotBasedExecutionEngineOn: 0.1,
            setEnableSlotBasedExecutionEngineOff: 0.1,
            runQueriesAndCheckResults: 0.8
        },

        setEnableSlotBasedExecutionEngineOff: {
            setEnableSlotBasedExecutionEngineOn: 0.1,
            setEnableSlotBasedExecutionEngineOff: 0.1,
            runQueriesAndCheckResults: 0.8
        },

        runQueriesAndCheckResults: {
            setEnableSlotBasedExecutionEngineOn: 0.1,
            setEnableSlotBasedExecutionEngineOff: 0.1,
            runQueriesAndCheckResults: 0.78,
            createIndex: 0.02,
        },

        createIndex: {
            setEnableSlotBasedExecutionEngineOn: 0.1,
            setEnableSlotBasedExecutionEngineOff: 0.1,
            runQueriesAndCheckResults: 0.78,
            createIndex: 0.01,
            dropIndex: 0.01
        },

        dropIndex: {
            setEnableSlotBasedExecutionEngineOn: 0.1,
            setEnableSlotBasedExecutionEngineOff: 0.1,
            runQueriesAndCheckResults: 0.78,
            createIndex: 0.02,
        }
    };

    function teardown(db, collName, cluster) {
        // Restore the original state of the ForceClassicEngine parameter.
        const setParam = this.originalParamValue;
        cluster.executeOnMongodNodes(function(db) {
            assertAlways.commandWorked(db.adminCommand(
                {setParameter: 1, internalQueryEnableSlotBasedExecutionEngine: setParam}));
        });
    }

    return {
        threadCount: 10,
        iterations: 100,
        startState: 'setEnableSlotBasedExecutionEngineOn',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data
    };
})();
