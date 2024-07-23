/**
 * With a normal ascending index on a geospatial field, {$not: {$geoWithin: <>}} and
 * {$not: {$geoIntersects: <>}} queries should return proper results. Previously, those queries
 * failed due to attempting to build geospatial index bounds on the non-geo index. See SERVER-92193
 * and AF-625 for more details.
 */

const coll = db.negated_geo_queries;
coll.drop();
coll.createIndex({loc: 1});

coll.insert(
    {loc: {type: "Polygon", coordinates: [[[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]]}, a: 0});
coll.insert(
    {loc: {type: "Polygon", coordinates: [[[30, 0], [30, 1], [32, 1], [32, 0], [30, 0]]]}, a: 0});
coll.insert(
    {loc: {type: "Polygon", coordinates: [[[-55, 10], [-35, 15], [-45, 10], [-55, 10]]]}, a: 1});

const targetPolygonWithin = {
    type: "Polygon",
    coordinates: [[[-5, -5], [5, -5], [5, 5], [-5, 5], [-5, -5]]]
};

// Tests $geoWithin.
let res = coll.find({loc: {$geoWithin: {$geometry: targetPolygonWithin}}});
assert.eq(res.itcount(), 1);

// Tests $geoWithin under a $not.
res = coll.find({loc: {$not: {$geoWithin: {$geometry: targetPolygonWithin}}}});
assert.eq(res.itcount(), 2);

const targetLineIntersect = {
    type: "LineString",
    coordinates: [[29, -1], [31, 1]]
};

// Tests $geoIntersects.
res = coll.find({loc: {$geoIntersects: {$geometry: targetLineIntersect}}});
assert.eq(res.itcount(), 1);

// Tests $geoIntersects under a $not.
res = coll.find({loc: {$not: {$geoIntersects: {$geometry: targetLineIntersect}}}});
assert.eq(res.itcount(), 2);

// Tests an $and of the negated $geoWithin and negated $geoIntersects.
res = coll.find({
    $and: [
        {loc: {$not: {$geoWithin: {$geometry: targetPolygonWithin}}}},
        {loc: {$not: {$geoIntersects: {$geometry: targetLineIntersect}}}}
    ]
});
assert.eq(res.itcount(), 1);

// Tests the same logic as above written with a $nor.
res = coll.find({
    $nor: [
        {loc: {$geoWithin: {$geometry: targetPolygonWithin}}},
        {loc: {$geoIntersects: {$geometry: targetLineIntersect}}}
    ]
});
assert.eq(res.itcount(), 1);

// Tests an $and of the negated $geoWithin and an expression on a non-geo field.
res = coll.find({$and: [{loc: {$not: {$geoWithin: {$geometry: targetPolygonWithin}}}}, {a: 0}]});
assert.eq(res.itcount(), 1);

// Tests an $and of the negated $geoIntersects and an expression on a non-geo field.
res =
    coll.find({$and: [{loc: {$not: {$geoIntersects: {$geometry: targetLineIntersect}}}}, {a: 0}]});
assert.eq(res.itcount(), 1);
