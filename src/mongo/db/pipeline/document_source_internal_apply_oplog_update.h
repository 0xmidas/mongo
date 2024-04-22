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

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/update/update_driver.h"

namespace mongo {

/**
 * This is an internal stage that takes an oplog update description and applies the update to the
 * input Document.
 */
class DocumentSourceInternalApplyOplogUpdate final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalApplyOplogUpdate"_sd;
    static constexpr StringData kOplogUpdateFieldName = "oplogUpdate"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    DocumentSourceInternalApplyOplogUpdate(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                           const BSONObj& oplogUpdate);

    const char* getSourceName() const override {
        return kStageName.rawData();
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed,
                                     ChangeStreamRequirement::kDenylist);
        constraints.canSwapWithMatch = false;
        constraints.canSwapWithSkippingOrLimitingStage = true;
        constraints.isAllowedWithinUpdatePipeline = true;
        constraints.checkExistenceForDiffInsertOperations = true;
        constraints.isIndependentOfAnyCollection = false;
        return constraints;
    }

    DocumentSource::GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kAllPaths, {}, {}};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        return boost::none;
    }

private:
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final override;

    GetNextResult doGetNext() override;

    BSONObj _oplogUpdate;
    UpdateDriver _updateDriver;
};

}  // namespace mongo
