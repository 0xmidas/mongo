/*
 * This test makes sure the 'dbcheck' command is ignored during rollback and a warning health log
 * entry is logged.
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/libs/rollback_test.js");
load("jstests/replsets/libs/dbcheck_utils.js");

// This test injects inconsistencies between replica set members; do not fail because of expected
// dbHash differences.
TestData.skipCollectionAndIndexValidation = true;
TestData.skipCheckDBHashes = true;

const dbName = "ignore_dbcheck_in_rollback";
const collName = "ignore_dbcheck_in_rollback-collection";

const replSet = new ReplSetTest({
    name: jsTestName(),
    nodes: [{}, {}, {rsConfig: {priority: 0}}],
    useBridge: true,
    settings: {chainingAllowed: false}
});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

let primary = replSet.getPrimary();
let primaryDB = primary.getDB(dbName);
const secondary = replSet.getSecondary();
const secondaryDb = secondary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

const nDocs = 200;
resetAndInsert(replSet, primaryDB, collName, nDocs);
assert.commandWorked(
    primaryDB.runCommand({createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}]}));
replSet.awaitReplication();
assert.eq(primaryColl.find({}).count(), nDocs);

// Set up inconsistency.
const skipUnindexingDocumentWhenDeleted =
    configureFailPoint(primaryDB, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});
jsTestLog("Deleting docs");
const stableTimestamp = assert.commandWorked(primaryColl.deleteMany({}));

replSet.awaitReplication();
assert.eq(primaryColl.find({}).count(), 0);
assert.eq(secondaryDb.getCollection(collName).find({}).count(), 0);

const rollbackTest = new RollbackTest(jsTestName(), replSet);
primary = rollbackTest.getPrimary();
primaryDB = primary.getDB(dbName);

// Hold stable timestamp.
const stableTimestampFailPoint = configureFailPoint(
    primary, "holdStableTimestampAtSpecificTimestamp", {timestamp: stableTimestamp});

// TODO SERVER-89921: Uncomment validateMode and secondaryIndex once the relevant tickets are
// backported.
runDbCheck(rollbackTest,
           primaryDB,
           collName,
           {
               maxDocsPerBatch: 20  //, validateMode: "extraIndexKeysCheck", secondaryIndex: "a_1"
           });

// TODO SERVER-89921: Uncomment checkHealthLog once the relevant tickets are backported.
// Check that the old primary prior to transitioning to rollback has start, batch, and stop entries.
const oldPrimaryHealthLog = primary.getDB("local").system.healthlog;
// checkHealthLog(oldPrimaryHealthLog, logQueries.recordNotFoundQuery, nDocs);
checkHealthLog(oldPrimaryHealthLog, logQueries.startStopQuery, 2);

const rollbackNode = rollbackTest.transitionToRollbackOperations();
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations({skipDataConsistencyChecks: true});

primary = rollbackTest.getPrimary();
const primaryHealthLog = primary.getDB("local").system.healthlog;
const rollbackNodeHealthLog = rollbackNode.getDB("local").system.healthlog;

// TODO SERVER-89921: Change the # of logs to check once the relevant tickets are backported.
// Check that the start, batch (10 batches), and stop entries on the rollback node are all warning
// logs.
checkHealthLog(rollbackNodeHealthLog, logQueries.duringStableRecovery, 3);  // 12
// The rollback node will contain start, batch, and stop entries from when it was primary. Check
// that there are no extra error logs other than recordNotFound.
checkHealthLog(rollbackNodeHealthLog, logQueries.startStopQuery, 2);
// checkHealthLog(rollbackNodeHealthLog, logQueries.recordNotFoundQuery, nDocs);
checkHealthLog(rollbackNodeHealthLog, logQueries.allErrorsOrWarningsQuery, 3);  // nDocs + 12

// TODO SERVER-89921: Uncomment the following checkHealthLog once the relevant tickets are
// backported.
// Check that the primary only has the batch inconsistent entries from when it was secondary.
// checkHealthLog(primaryHealthLog, logQueries.inconsistentBatchQuery, 10);
// Check that the primary does not have other error/warning entries.
// checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 10);

skipUnindexingDocumentWhenDeleted.off();
stableTimestampFailPoint.off();
rollbackTest.stop(null /* checkDataConsistencyOptions */, true /* skipDataConsistencyCheck */);
