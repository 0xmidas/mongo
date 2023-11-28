
/**
 * Test a particular nested $and/$or query. This test was designed to reproduce SERVER-83091, a bug
 * in which this query could trigger an infinite loop during plan enumeration.
 */
(function() {
"use strict";
load("jstests/query_golden/libs/utils.js");

const coll = db.and_or_lockstep_coll;

// At the time when SERVER-83091 was filed, match expression rewrites would rewrite the {b: 4} and
// {b: 5} to {b: {$in: [4, 5]}}, resulting in a nested $or predicate which looks like this:
//
// $and
//   a $eq 1
//   $or
//     b $in [ 4, 5 ]
//     $or
//       $and
//         b $eq 2
//         c $eq 3
//       b $in [ 6, 7 ]
//
// The fact that this rewrite could result in an $or with a direct $or child was key to reproducing
// SERVER-83091. Note that SERVER-83602 improves the system such that it will not unnecessarily
// generate an $or with a direct $or child.
//
// This nested $or is never simplified later on. (In constrast, if the query is originally expressed
// with a nested $or it will get simplified; that's why the repro requires an $or containing some
// predicates that get rewritten to an $in.) The bug related to how lockstep enumeration dealt with
// nested $or nodes. A very particular query structure was also necessary, because to exercise the
// bug we needed to get into a state where the inner $or reached its limit on the number of plans it
// was willing to enumerate before the outer $or did.
const predicate = {
    a: 1,
    $or: [
        {b: 2, c: 3},
        {b: 4},
        {b: 5},
        {
            b: {$in: [6, 7]},
        }
    ],
};

const docs = [
    // No "a" field.
    {x: 2},

    // "a" does not match.
    {a: 2, x: 2},

    // No matching "b" or "c" value.
    {a: 1, x: 2, b: 8},
    {a: 1, x: 2, b: 2, c: 9},

    // Matching values.
    {a: 1, x: 2, b: 2, c: 3},
    {a: 1, x: 2, b: 4},
    {a: 1, x: 2, b: 5},
    {a: 1, x: 2, b: 6},
    {a: 1, x: 2, b: 7},
];
const indexes = [
    {a: 1, b: 1},
    {a: 1, c: 1},
];
resetCollection(coll, docs, indexes);

show(coll.find(predicate));
}());
