/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/stage_types.h"

namespace mongo::sbe {
/**
 * TODO: Description of search_cursor_stage.
 *
 *
 * Debug string representation:
 *
 *  search_cursor_stage optionalIdSlot optionalResultSlot [metaSlot1, …, metadataSlotN] [fieldSlot1,
 * …, fieldSlotN] optionalSearchMetaSlot
 */
class SearchCursorStage final : public PlanStage {
public:
    SearchCursorStage(boost::optional<value::SlotId> idSlot,
                      boost::optional<value::SlotId> resultSlot,
                      std::vector<std::string> metadataNames,
                      value::SlotVector metadataSlots,
                      std::vector<std::string> fieldNames,
                      value::SlotVector fieldSlots,
                      boost::optional<value::SlotId> searchMetaSlot,
                      value::SlotId cursorIdSlot,
                      value::SlotId firstBatchSlot,
                      PlanYieldPolicy* yieldPolicy,
                      PlanNodeId planNodeId,
                      bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final override;
    size_t estimateCompileTimeSize() const final;

private:
    const boost::optional<value::SlotId> _idSlot;
    const boost::optional<value::SlotId> _resultSlot;
    const IndexedStringVector _metadataNames;
    const value::SlotVector _metadataSlots;
    const IndexedStringVector _fieldNames;
    const value::SlotVector _fieldSlots;
    const boost::optional<value::SlotId> _searchMetaSlot;
    const value::SlotId _cursorIdSlot;
    const value::SlotId _firstBatchSlot;
};

template <typename... Ts>
inline auto makeSearchCursorStage(std::unique_ptr<PlanStage> input,
                                  PlanNodeId nodeId,
                                  Ts&&... pack) {
    return makeS<SearchCursorStage>(std::move(input), makeEM(std::forward<Ts>(pack)...), nodeId);
}
}  // namespace mongo::sbe
