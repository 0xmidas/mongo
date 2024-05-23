/**
 * create_collection.js
 *
 * Repeatedly creates a collection.
 */
export const $config = (function() {
    var data = {
        // Use the workload name as a prefix for the collection name,
        // since the workload name is assumed to be unique.
        prefix: 'create_collection'
    };

    var states = (function() {
        function uniqueCollectionName(prefix, tid, num) {
            return prefix + tid + '_' + num;
        }

        function init(db, collName) {
            this.num = 0;
        }

        // TODO: how to avoid having too many files open?
        function create(db, collName) {
            // TODO: should we ever do something different?
            var myCollName = uniqueCollectionName(this.prefix, this.tid, this.num++);
            assert.commandWorked(db.createCollection(myCollName));
        }

        return {init: init, create: create};
    })();

    var transitions = {init: {create: 1}, create: {create: 1}};

    return {
        threadCount: 5,
        iterations:
            // The config transition suites involve moving unsharded collections in and out of
            // the config server. Having up to 100 unsharded collections to move may make this test
            // take too long to run and get killed by resmoke.
            TestData.transitioningConfigShard ? 5 : 20,
        data: data,
        states: states,
        transitions: transitions,
    };
})();
