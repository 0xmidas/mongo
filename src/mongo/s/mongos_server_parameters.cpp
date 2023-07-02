/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/s/mongos_server_parameters.h"

#include <string>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/tenant_id.h"
#include "mongo/s/mongos_server_parameters_gen.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {
std::string toReadHedgingModeString(ReadHedgingMode readHedgingMode) {
    return readHedgingMode == ReadHedgingMode::kOn ? "on" : "off";
}
}  // namespace

AtomicWord<ReadHedgingMode> gReadHedgingMode{ReadHedgingMode::kOn};

void HedgingModeServerParameter::append(OperationContext*,
                                        BSONObjBuilder* builder,
                                        StringData name,
                                        const boost::optional<TenantId>&) {
    builder->append(name, toReadHedgingModeString(gReadHedgingMode.load()));
}

Status HedgingModeServerParameter::setFromString(StringData modeStr,
                                                 const boost::optional<TenantId>&) {
    if (modeStr == "on") {
        gReadHedgingMode.store(ReadHedgingMode::kOn);
    } else if (modeStr == "off") {
        gReadHedgingMode.store(ReadHedgingMode::kOff);
    } else {
        return Status{ErrorCodes::BadValue,
                      str::stream() << "Unrecognized readHedgingMode '" << modeStr << "'"};
    }
    return Status::OK();
}

}  // namespace mongo
