/**
 * Tests TTL indexes with NaN for 'expireAfterSeconds'.
 *
 * Existing TTL indexes from older versions of the server may contain a NaN for the duration.
 * Newer server versions (5.0+) normalize the TTL duration to 0.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {votes: 0, priority: 0}}],
    nodeOptions: {setParameter: {ttlMonitorSleepSecs: 5}},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB('test');
const coll = db.t;

assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));
assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: NaN}));

// Wait for "TTL indexes require the expire field to be numeric, skipping TTL job" log message.
checkLog.contains(
    primary,
    "TTL indexes require the expireAfterSeconds field to be to be numeric and not a NaN, " +
        "skipping TTL job. ns: " + coll.getFullName());

// TTL index should be replicated to the secondary with a NaN 'expireAfterSeconds'.
const secondary = rst.getSecondary();
checkLog.contains(
    secondary, /index build: starting on test.t properties: .*name.*t_1.*expireAfterSeconds.*nan/);

assert.eq(
    coll.countDocuments({}), 1, 'ttl index with NaN duration should not remove any documents.');

rst.stopSet();
})();
