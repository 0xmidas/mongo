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

#include <set>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/commands/cluster_abort_transaction_cmd.h"

namespace mongo {
namespace {

/**
 * Implements the cluster abortTransaction command on mongos.
 */
struct ClusterAbortTransactionCmdS {
    static constexpr StringData kName = "abortTransaction"_sd;

    static const std::set<std::string>& getApiVersions() {
        return kApiVersions1;
    }

    static Status checkAuthForOperation(OperationContext*, const DatabaseName&, const BSONObj&) {
        return Status::OK();
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }
};
ClusterAbortTransactionCmdBase<ClusterAbortTransactionCmdS> clusterAbortTransactionS;

}  // namespace
}  // namespace mongo
