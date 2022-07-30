/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/timeseries/timeseries_extended_range.h"

#include "mongo/db/timeseries/timeseries_constants.h"

namespace mongo::timeseries {

bool dateOutsideStandardRange(Date_t date) {
    constexpr long long kMaxNormalRangeTimestamp = ((1LL << 31) - 1);
    long long timeSeconds = durationCount<Seconds>(date.toDurationSinceEpoch());
    return timeSeconds < 0 || timeSeconds > kMaxNormalRangeTimestamp;
}

bool bucketsHaveDateOutsideStandardRange(const TimeseriesOptions& options,
                                         std::vector<InsertStatement>::const_iterator first,
                                         std::vector<InsertStatement>::const_iterator last) {
    return std::any_of(first, last, [&](const InsertStatement& stmt) -> bool {
        auto controlElem = stmt.doc.getField(timeseries::kBucketControlFieldName);
        uassert(6781400,
                "Time series bucket document is missing 'control' field",
                controlElem.isABSONObj());
        auto minElem = controlElem.Obj().getField(timeseries::kBucketControlMinFieldName);
        uassert(6781401,
                "Time series bucket document is missing 'control.min' field",
                minElem.isABSONObj());
        auto timeElem = minElem.Obj().getField(options.getTimeField());
        uassert(6781402,
                "Time series bucket document does not have a valid min time element",
                timeElem && BSONType::Date == timeElem.type());

        auto date = timeElem.Date();
        return dateOutsideStandardRange(date);
    });
}

bool collectionMayRequireExtendedRangeSupport(OperationContext* opCtx,
                                              const CollectionPtr& collection) {
    bool requiresExtendedRangeSupport = false;

    // We use a heuristic here to perform a check as quickly as possible and get the correct answer
    // with high probability. The rough idea is that if a user has dates outside the standard range
    // from 1970-2038, they most likely have some dates near either end of that range, i.e. between
    // 1902-1969 or 2039-2106. Given this assumption, we can assume that at least one document in
    // the collection should have the high bit of the timestamp portion of the OID set. If such a
    // document exists, then the maximum OID will have this bit set. So we can just check the last
    // document in the record store and test this high bit of it's _id.

    auto* rs = collection->getRecordStore();
    auto cursor = rs->getCursor(opCtx, /* forward */ false);
    if (auto record = cursor->next()) {
        const auto& obj = record->data.toBson();
        OID id = obj.getField(kBucketIdFieldName).OID();

        uint8_t highDateBits = id.view().read<uint8_t>(0);
        if (highDateBits & 0x80) {
            requiresExtendedRangeSupport = true;
        }
    }

    return requiresExtendedRangeSupport;
}

bool collectionHasTimeIndex(OperationContext* opCtx, const Collection& collection) {
    auto tsOptions = collection.getTimeseriesOptions();
    invariant(tsOptions);
    std::string controlMinTimeField = timeseries::kControlMinFieldNamePrefix.toString();
    controlMinTimeField.append(tsOptions->getTimeField().toString());
    std::string controlMaxTimeField = timeseries::kControlMaxFieldNamePrefix.toString();
    controlMaxTimeField.append(tsOptions->getTimeField().toString());

    auto indexCatalog = collection.getIndexCatalog();
    // The IndexIterator is initialized lazily, so the first call to 'next' positions it to the
    // first entry.
    for (auto it = indexCatalog->getIndexIterator(opCtx, false); it->more();) {
        auto index = it->next();
        auto desc = index->descriptor();
        auto pattern = desc->keyPattern();
        auto keyIt = pattern.begin();
        StringData field = keyIt->fieldNameStringData();
        if (field == controlMinTimeField || field == controlMaxTimeField) {
            return true;
        }
    }
    return false;
}

}  // namespace mongo::timeseries
