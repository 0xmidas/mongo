/**
 * Tests that storage stats reporting on slow query logging does acquire the RSTL.
 *
 * @tags: [
 *   # Before we log the slow query, we need to acquire read access to the
 *   # test collection. This requires lock-free access (no RSTL) because
 *   # the test is running a concurrent sleep command with exclusive lock
 *   # on the RSTL resource.
 *   # Lock-free reads is also dependent on majority read concern support,
 *   # so this rules out storage engines such as ephemeralForTest.
 *   incompatible_with_lockfreereads,
 *   requires_majority_read_concern,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");  // startParallelShell
load("jstests/libs/wait_for_command.js");        // waitForCommand

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const testDB = rst.getPrimary().getDB("test");
const testCollection = testDB.getCollection("c");

const fieldValue = "slow query logging reporting storage statistics";

assert.commandWorked(testCollection.insertOne({a: fieldValue}));

// Log any query regardless of its completion time.
assert.commandWorked(testDB.setProfilingLevel(0, -1));

jsTestLog("Starting the sleep command in a parallel thread to take the RSTL MODE_X lock");
let rstlXLockSleepJoin = startParallelShell(() => {
    jsTestLog("Parallel Shell: about to start sleep command");
    assert.commandFailedWithCode(db.adminCommand({
        sleep: 1,
        secs: 60 * 60,
        // RSTL MODE_X lock.
        lockTarget: "RSTL",
        $comment: "RSTL lock sleep"
    }),
                                 ErrorCodes.Interrupted);
}, testDB.getMongo().port);

jsTestLog("Waiting for the sleep command to start and fetch the opID");
const sleepCmdOpID =
    waitForCommand("RSTL lock", op => (op["command"]["$comment"] == "RSTL lock sleep"), testDB);

jsTestLog("Wait for the sleep command to log that the RSTL MODE_X lock was acquired");
checkLog.containsJson(testDB, 6001600);

try {
    jsTestLog("Running the query while the RSTL is being held");

    const loggedQuery = RegExp("Slow query.*\"find\":\"c\".*" + fieldValue + ".*\"storage\":{");
    assert.eq(false, checkLog.checkContainsOnce(rst.getPrimary(), loggedQuery));
    assert.eq(1, testCollection.find({a: fieldValue}).itcount());
    assert.eq(true, checkLog.checkContainsOnce(rst.getPrimary(), loggedQuery));
} finally {
    jsTestLog("Ensure the sleep cmd releases the lock so that the server can shutdown");
    assert.commandWorked(testDB.killOp(sleepCmdOpID));  // kill the sleep cmd
    rstlXLockSleepJoin();  // wait for the thread running the sleep cmd to finish
}

rst.stopSet();
})();
