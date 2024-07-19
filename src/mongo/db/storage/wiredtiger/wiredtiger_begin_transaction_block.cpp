/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <fmt/format.h>
#include <string>

#include <wiredtiger.h>

#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl_set_member_in_standalone_mode.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_compiled_configuration.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

// There are a few delicate restore scenarios where untimestamped writes are still required.
bool allowUntimestampedWrites() {
    // Magic restore may need to perform untimestamped writes on timestamped tables as a part of
    // the server automated restore procedure.
    if (storageGlobalParams.magicRestore) {
        return true;
    }

    if (!gAllowUnsafeUntimestampedWrites) {
        return false;
    }

    // Ignore timestamps in selective restore mode.
    if (storageGlobalParams.restore) {
        return true;
    }

    // We can safely ignore setting this configuration option when recovering from the
    // oplog as standalone because:
    // 1. Replaying oplog entries write with a timestamp.
    // 2. The instance is put in read-only mode after oplog application has finished.
    if (getReplSetMemberInStandaloneMode(getGlobalServiceContext()) &&
        !repl::ReplSettings::shouldRecoverFromOplogAsStandalone()) {
        return true;
    }

    return false;
}

}  // namespace

using namespace fmt::literals;
using NoReadTimestamp = WiredTigerBeginTxnBlock::NoReadTimestamp;

static inline int getConfigOffset(int ignore_prepare,
                                  int roundup_prepared,
                                  int roundup_read,
                                  int no_timestamp) {
    static constexpr int roundup_read_factor = static_cast<int>(NoReadTimestamp::kMax);
    static constexpr int roundup_prepared_factor =
        static_cast<int>(RoundUpReadTimestamp::kMax) * roundup_read_factor;
    static constexpr int ignore_prepare_factor =
        static_cast<int>(RoundUpPreparedTimestamps::kMax) * roundup_prepared_factor;
    return ignore_prepare * ignore_prepare_factor + roundup_prepared * roundup_prepared_factor +
        roundup_read * roundup_read_factor + no_timestamp;
}

static std::vector<CompiledConfiguration>& makeCompiledConfigurations() {
    static std::vector<CompiledConfiguration> compiledConfigurations;
    std::string ignore_prepare_str[] = {"false", "true", "force"};
    std::string false_true_str[] = {"false", "true"};
    for (int ignore_prepare = static_cast<int>(PrepareConflictBehavior::kEnforce);
         ignore_prepare < static_cast<int>(PrepareConflictBehavior::kMax);
         ignore_prepare++)
        for (int roundup_prepared = static_cast<int>(RoundUpPreparedTimestamps::kNoRound);
             roundup_prepared < static_cast<int>(RoundUpPreparedTimestamps::kMax);
             roundup_prepared++)
            for (int roundup_read = static_cast<int>(RoundUpReadTimestamp::kNoRoundError);
                 roundup_read < static_cast<int>(RoundUpReadTimestamp::kMax);
                 roundup_read++)
                for (int no_timestamp = static_cast<int>(NoReadTimestamp::kFalse);
                     no_timestamp < static_cast<int>(NoReadTimestamp::kMax);
                     no_timestamp++) {
                    // not to compile default
                    int config = getConfigOffset(
                        ignore_prepare, roundup_prepared, roundup_read, no_timestamp);
                    if (config == 0) {
                        continue;
                    }
                    const std::string beginTxnConfigString = fmt::format(
                        "ignore_prepare={},roundup_timestamps=(prepared={},read={}),no_timestamp={"
                        "}",
                        ignore_prepare_str[ignore_prepare],
                        false_true_str[roundup_prepared],
                        false_true_str[roundup_read],
                        false_true_str[no_timestamp]);
                    compiledConfigurations.emplace_back("WT_SESSION.begin_transaction",
                                                        beginTxnConfigString.c_str());
                }
    return compiledConfigurations;
}

static std::vector<CompiledConfiguration>& compiledBeginTransactions = makeCompiledConfigurations();

WiredTigerBeginTxnBlock::WiredTigerBeginTxnBlock(
    WiredTigerSession* session,
    PrepareConflictBehavior prepareConflictBehavior,
    RoundUpPreparedTimestamps roundUpPreparedTimestamps,
    RoundUpReadTimestamp roundUpReadTimestamp,
    RecoveryUnit::UntimestampedWriteAssertionLevel allowUntimestampedWrite)
    : _session(session) {
    invariant(!_rollback);
    _wt_session = _session->getSession();

    NoReadTimestamp no_timestamp = NoReadTimestamp::kFalse;
    if (allowUntimestampedWrite != RecoveryUnit::UntimestampedWriteAssertionLevel::kEnforce ||
        MONGO_unlikely(allowUntimestampedWrites())) {
        no_timestamp = NoReadTimestamp::kTrue;
    }

    int config = getConfigOffset(static_cast<int>(prepareConflictBehavior),
                                 static_cast<int>(roundUpPreparedTimestamps),
                                 static_cast<int>(roundUpReadTimestamp),
                                 static_cast<int>(no_timestamp));
    const char* compiled_config = nullptr;
    if (config > 0) {
        compiled_config = compiledBeginTransactions[config - 1].getConfig(_session);
    }
    invariantWTOK(_wt_session->begin_transaction(_wt_session, compiled_config), _wt_session);
    _rollback = true;
}

WiredTigerBeginTxnBlock::WiredTigerBeginTxnBlock(WiredTigerSession* session, const char* config)
    : _session(session) {
    invariant(!_rollback);
    _wt_session = _session->getSession();
    invariantWTOK(_wt_session->begin_transaction(_wt_session, config), _wt_session);
    _rollback = true;
}

WiredTigerBeginTxnBlock::~WiredTigerBeginTxnBlock() {
    if (_rollback) {
        invariant(_wt_session->rollback_transaction(_wt_session, nullptr) == 0);
    }
}

Status WiredTigerBeginTxnBlock::setReadSnapshot(Timestamp readTimestamp) {
    invariant(_rollback);
    return wtRCToStatus(_wt_session->timestamp_transaction_uint(
                            _wt_session, WT_TS_TXN_TYPE_READ, readTimestamp.asULL()),
                        _wt_session);
}

void WiredTigerBeginTxnBlock::done() {
    invariant(_rollback);
    _rollback = false;
}

}  // namespace mongo
