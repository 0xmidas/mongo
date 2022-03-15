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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include <algorithm>
#include <iterator>

#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"

#include <string>
#include <type_traits>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

namespace mongo {

/*
 * $_internalUnpackBucket is an internal stage for materializing time-series measurements from
 * time-series collections. It should never be used anywhere outside the MongoDB server.
 */
REGISTER_DOCUMENT_SOURCE(_internalUnpackBucket,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalUnpackBucket::createFromBsonInternal,
                         AllowedWithApiStrict::kAlways);

/*
 * $_unpackBucket is an alias of $_internalUnpackBucket. It only exposes the "timeField" and the
 * "metaField" parameters and is only used for special known use cases by other MongoDB products
 * rather than user applications.
 */
REGISTER_DOCUMENT_SOURCE(_unpackBucket,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalUnpackBucket::createFromBsonExternal,
                         AllowedWithApiStrict::kAlways);

namespace {
/**
 * A projection can be internalized if every field corresponds to a boolean value. Note that this
 * correctly rejects dotted fieldnames, which are mapped to objects internally.
 */
bool canInternalizeProjectObj(const BSONObj& projObj) {
    return std::all_of(projObj.begin(), projObj.end(), [](auto&& e) { return e.isBoolean(); });
}

/**
 * If 'src' represents an inclusion or exclusion $project, return a BSONObj representing it and a
 * bool indicating its type (true for inclusion, false for exclusion). Else return an empty BSONObj.
 */
auto getIncludeExcludeProjectAndType(DocumentSource* src) {
    if (const auto proj = dynamic_cast<DocumentSourceSingleDocumentTransformation*>(src); proj &&
        (proj->getType() == TransformerInterface::TransformerType::kInclusionProjection ||
         proj->getType() == TransformerInterface::TransformerType::kExclusionProjection)) {
        return std::pair{proj->getTransformer().serializeTransformation(boost::none).toBson(),
                         proj->getType() ==
                             TransformerInterface::TransformerType::kInclusionProjection};
    }
    return std::pair{BSONObj{}, false};
}

auto isLastpointSortTimeAscending(const SortPattern& sortPattern, const std::string& timeField) {
    for (auto entry : sortPattern) {
        if (entry.fieldPath->fullPath() == timeField) {
            return entry.isAscending;
        }
    }
    // A lastpoint query will always have the time field as part of the sort pattern.
    MONGO_UNREACHABLE;
}

/**
 * Checks if a sort stage's pattern following our internal unpack bucket is suitable to be reordered
 * before us. The sort stage must refer exclusively to the meta field or any subfields.
 *
 * If this check is being used for lastpoint, the sort stage can also refer to the time field,
 * which should be the last field in the pattern.
 */
bool checkMetadataSortReorder(
    const SortPattern& sortPattern,
    const StringData& metaFieldStr,
    const boost::optional<std::string&> lastpointTimeField = boost::none) {
    auto timeFound = false;
    for (const auto& sortKey : sortPattern) {
        if (!sortKey.fieldPath.has_value()) {
            return false;
        }
        if (sortKey.fieldPath->getPathLength() < 1) {
            return false;
        }
        if (sortKey.fieldPath->getFieldName(0) != metaFieldStr) {
            if (lastpointTimeField && sortKey.fieldPath->fullPath() == lastpointTimeField.get()) {
                // If we are checking the sort pattern for the lastpoint case, 'time' is allowed.
                timeFound = true;
                continue;
            }
            return false;
        } else {
            if (lastpointTimeField && timeFound) {
                // The time field was not the last field in the sort pattern.
                return false;
            }
        }
    }
    // If we are checking for lastpoint, make sure we encountered the time field.
    return !lastpointTimeField || timeFound;
}

/**
 * Returns a new DocumentSort to reorder before current unpack bucket document.
 */
boost::intrusive_ptr<DocumentSourceSort> createMetadataSortForReorder(
    const DocumentSourceSort& sort,
    const boost::optional<std::string&> lastpointTimeField = boost::none,
    bool flipSort = false) {
    auto sortPattern = flipSort
        ? SortPattern(
              QueryPlannerCommon::reverseSortObj(
                  sort.getSortKeyPattern()
                      .serialize(SortPattern::SortKeySerialization::kForPipelineSerialization)
                      .toBson()),
              sort.getContext())
        : sort.getSortKeyPattern();
    std::vector<SortPattern::SortPatternPart> updatedPattern;
    for (const auto& entry : sortPattern) {
        updatedPattern.push_back(entry);

        if (lastpointTimeField && entry.fieldPath->fullPath() == lastpointTimeField.get()) {
            updatedPattern.back().fieldPath =
                FieldPath((entry.isAscending ? timeseries::kControlMinFieldNamePrefix
                                             : timeseries::kControlMaxFieldNamePrefix) +
                          lastpointTimeField.get());
            updatedPattern.push_back(SortPattern::SortPatternPart{
                entry.isAscending,
                FieldPath((entry.isAscending ? timeseries::kControlMaxFieldNamePrefix
                                             : timeseries::kControlMinFieldNamePrefix) +
                          lastpointTimeField.get()),
                nullptr});
        } else {
            auto updated = FieldPath(timeseries::kBucketMetaFieldName);
            if (entry.fieldPath->getPathLength() > 1) {
                updated = updated.concat(entry.fieldPath->tail());
            }
            updatedPattern.back().fieldPath = updated;
        }
    }

    boost::optional<uint64_t> maxMemoryUsageBytes;
    if (auto sortStatsPtr = dynamic_cast<const SortStats*>(sort.getSpecificStats())) {
        maxMemoryUsageBytes = sortStatsPtr->maxMemoryUsageBytes;
    }

    return DocumentSourceSort::create(
        sort.getContext(), SortPattern{updatedPattern}, 0, maxMemoryUsageBytes);
}

boost::intrusive_ptr<DocumentSourceGroup> createGroupForReorder(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, FieldPath& fieldPath) {
    auto elem = BSON("bucket" << BSON(AccumulatorFirst::kName << "$_id")).firstElement();
    auto newAccum = AccumulationStatement::parseAccumulationStatement(
        expCtx.get(), elem, expCtx->variablesParseState);
    auto groupByExpr = ExpressionFieldPath::createPathFromString(
        expCtx.get(), fieldPath.fullPath(), expCtx->variablesParseState);
    return DocumentSourceGroup::create(expCtx, groupByExpr, {newAccum});
}

// Optimize the section of the pipeline before the $_internalUnpackBucket stage.
void optimizePrefix(Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    auto prefix = Pipeline::SourceContainer(container->begin(), itr);
    Pipeline::optimizeContainer(&prefix);
    container->erase(container->begin(), itr);
    container->splice(itr, prefix);
}

}  // namespace

DocumentSourceInternalUnpackBucket::DocumentSourceInternalUnpackBucket(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BucketUnpacker bucketUnpacker,
    int bucketMaxSpanSeconds,
    bool assumeNoMixedSchemaData)
    : DocumentSource(kStageNameInternal, expCtx),
      _assumeNoMixedSchemaData(assumeNoMixedSchemaData),
      _bucketUnpacker(std::move(bucketUnpacker)),
      _bucketMaxSpanSeconds{bucketMaxSpanSeconds} {}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalUnpackBucket::createFromBsonInternal(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5346500,
            str::stream() << "$_internalUnpackBucket specification must be an object, got: "
                          << specElem.type(),
            specElem.type() == BSONType::Object);

    // If neither "include" nor "exclude" is specified, the default is "exclude": [] and
    // if that's the case, no field will be added to 'bucketSpec.fieldSet' in the for-loop below.
    BucketUnpacker::Behavior unpackerBehavior = BucketUnpacker::Behavior::kExclude;
    BucketSpec bucketSpec;
    auto hasIncludeExclude = false;
    auto hasTimeField = false;
    auto hasBucketMaxSpanSeconds = false;
    auto bucketMaxSpanSeconds = 0;
    auto assumeClean = false;
    std::vector<std::string> computedMetaProjFields;
    for (auto&& elem : specElem.embeddedObject()) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == kInclude || fieldName == kExclude) {
            uassert(5408000,
                    "The $_internalUnpackBucket stage expects at most one of include/exclude "
                    "parameters to be specified",
                    !hasIncludeExclude);

            uassert(5346501,
                    str::stream() << "include or exclude field must be an array, got: "
                                  << elem.type(),
                    elem.type() == BSONType::Array);

            for (auto&& elt : elem.embeddedObject()) {
                uassert(5346502,
                        str::stream() << "include or exclude field element must be a string, got: "
                                      << elt.type(),
                        elt.type() == BSONType::String);
                auto field = elt.valueStringData();
                uassert(5346503,
                        "include or exclude field element must be a single-element field path",
                        field.find('.') == std::string::npos);
                bucketSpec.addIncludeExcludeField(field);
            }
            unpackerBehavior = fieldName == kInclude ? BucketUnpacker::Behavior::kInclude
                                                     : BucketUnpacker::Behavior::kExclude;
            hasIncludeExclude = true;
        } else if (fieldName == kAssumeNoMixedSchemaData) {
            uassert(6067202,
                    str::stream() << "assumeClean field must be a bool, got: " << elem.type(),
                    elem.type() == BSONType::Bool);
            assumeClean = elem.boolean();
        } else if (fieldName == timeseries::kTimeFieldName) {
            uassert(5346504,
                    str::stream() << "timeField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::String);
            bucketSpec.setTimeField(elem.str());
            hasTimeField = true;
        } else if (fieldName == timeseries::kMetaFieldName) {
            uassert(5346505,
                    str::stream() << "metaField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::String);
            auto metaField = elem.str();
            uassert(5545700,
                    str::stream() << "metaField field must be a single-element field path",
                    metaField.find('.') == std::string::npos);
            bucketSpec.setMetaField(std::move(metaField));
        } else if (fieldName == kBucketMaxSpanSeconds) {
            uassert(5510600,
                    str::stream() << "bucketMaxSpanSeconds field must be an integer, got: "
                                  << elem.type(),
                    elem.type() == BSONType::NumberInt);
            uassert(5510601,
                    "bucketMaxSpanSeconds field must be greater than zero",
                    elem._numberInt() > 0);
            bucketMaxSpanSeconds = elem._numberInt();
            hasBucketMaxSpanSeconds = true;
        } else if (fieldName == "computedMetaProjFields") {
            uassert(5509900,
                    str::stream() << "computedMetaProjFields field must be an array, got: "
                                  << elem.type(),
                    elem.type() == BSONType::Array);

            for (auto&& elt : elem.embeddedObject()) {
                uassert(5509901,
                        str::stream()
                            << "computedMetaProjFields field element must be a string, got: "
                            << elt.type(),
                        elt.type() == BSONType::String);
                auto field = elt.valueStringData();
                uassert(5509902,
                        "computedMetaProjFields field element must be a single-element field path",
                        field.find('.') == std::string::npos);
                bucketSpec.addComputedMetaProjFields(field);
            }
        } else {
            uasserted(5346506,
                      str::stream()
                          << "unrecognized parameter to $_internalUnpackBucket: " << fieldName);
        }
    }

    uassert(
        5346508, "The $_internalUnpackBucket stage requires a timeField parameter", hasTimeField);

    uassert(5510602,
            "The $_internalUnpackBucket stage requires a bucketMaxSpanSeconds parameter",
            hasBucketMaxSpanSeconds);

    return make_intrusive<DocumentSourceInternalUnpackBucket>(
        expCtx,
        BucketUnpacker{std::move(bucketSpec), unpackerBehavior},
        bucketMaxSpanSeconds,
        assumeClean);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalUnpackBucket::createFromBsonExternal(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5612400,
            str::stream() << "$_unpackBucket specification must be an object, got: "
                          << specElem.type(),
            specElem.type() == BSONType::Object);

    BucketSpec bucketSpec;
    auto hasTimeField = false;
    auto assumeClean = false;
    for (auto&& elem : specElem.embeddedObject()) {
        auto fieldName = elem.fieldNameStringData();
        // We only expose "timeField" and "metaField" as parameters in $_unpackBucket.
        if (fieldName == timeseries::kTimeFieldName) {
            uassert(5612401,
                    str::stream() << "timeField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::String);
            bucketSpec.setTimeField(elem.str());
            hasTimeField = true;
        } else if (fieldName == timeseries::kMetaFieldName) {
            uassert(5612402,
                    str::stream() << "metaField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::String);
            auto metaField = elem.str();
            uassert(5612403,
                    str::stream() << "metaField field must be a single-element field path",
                    metaField.find('.') == std::string::npos);
            bucketSpec.setMetaField(std::move(metaField));
        } else if (fieldName == kAssumeNoMixedSchemaData) {
            uassert(6067203,
                    str::stream() << "assumeClean field must be a bool, got: " << elem.type(),
                    elem.type() == BSONType::Bool);
            assumeClean = elem.boolean();
        } else {
            uasserted(5612404,
                      str::stream() << "unrecognized parameter to $_unpackBucket: " << fieldName);
        }
    }
    uassert(5612405,
            str::stream() << "The $_unpackBucket stage requires a timeField parameter",
            hasTimeField);

    return make_intrusive<DocumentSourceInternalUnpackBucket>(
        expCtx,
        BucketUnpacker{std::move(bucketSpec), BucketUnpacker::Behavior::kExclude},
        3600,
        assumeClean);
}

void DocumentSourceInternalUnpackBucket::serializeToArray(
    std::vector<Value>& array, boost::optional<ExplainOptions::Verbosity> explain) const {
    MutableDocument out;
    auto behavior =
        _bucketUnpacker.behavior() == BucketUnpacker::Behavior::kInclude ? kInclude : kExclude;
    const auto& spec = _bucketUnpacker.bucketSpec();
    std::vector<Value> fields;
    for (auto&& field : spec.fieldSet()) {
        fields.emplace_back(field);
    }
    if (((_bucketUnpacker.includeMetaField() &&
          _bucketUnpacker.behavior() == BucketUnpacker::Behavior::kInclude) ||
         (!_bucketUnpacker.includeMetaField() &&
          _bucketUnpacker.behavior() == BucketUnpacker::Behavior::kExclude && spec.metaField())) &&
        std::find(spec.computedMetaProjFields().cbegin(),
                  spec.computedMetaProjFields().cend(),
                  *spec.metaField()) == spec.computedMetaProjFields().cend())
        fields.emplace_back(*spec.metaField());

    out.addField(behavior, Value{std::move(fields)});
    out.addField(timeseries::kTimeFieldName, Value{spec.timeField()});
    if (spec.metaField()) {
        out.addField(timeseries::kMetaFieldName, Value{*spec.metaField()});
    }
    out.addField(kBucketMaxSpanSeconds, Value{_bucketMaxSpanSeconds});
    if (_assumeNoMixedSchemaData)
        out.addField(kAssumeNoMixedSchemaData, Value(_assumeNoMixedSchemaData));

    if (!spec.computedMetaProjFields().empty())
        out.addField("computedMetaProjFields", Value{[&] {
                         std::vector<Value> compFields;
                         std::transform(spec.computedMetaProjFields().cbegin(),
                                        spec.computedMetaProjFields().cend(),
                                        std::back_inserter(compFields),
                                        [](auto&& projString) { return Value{projString}; });
                         return compFields;
                     }()});

    if (!explain) {
        array.push_back(Value(DOC(getSourceName() << out.freeze())));
        if (_sampleSize) {
            auto sampleSrc = DocumentSourceSample::create(pExpCtx, *_sampleSize);
            sampleSrc->serializeToArray(array);
        }
    } else {
        if (_sampleSize) {
            out.addField("sample", Value{static_cast<long long>(*_sampleSize)});
            out.addField("bucketMaxCount", Value{_bucketMaxCount});
        }
        array.push_back(Value(DOC(getSourceName() << out.freeze())));
    }
}

DocumentSource::GetNextResult DocumentSourceInternalUnpackBucket::doGetNext() {
    tassert(5521502, "calling doGetNext() when '_sampleSize' is set is disallowed", !_sampleSize);

    // Otherwise, fallback to unpacking every measurement in all buckets until the child stage is
    // exhausted.
    if (_bucketUnpacker.hasNext()) {
        return _bucketUnpacker.getNext();
    }

    auto nextResult = pSource->getNext();
    if (nextResult.isAdvanced()) {
        auto bucket = nextResult.getDocument().toBson();
        _bucketUnpacker.reset(std::move(bucket));
        uassert(5346509,
                str::stream() << "A bucket with _id "
                              << _bucketUnpacker.bucket()[timeseries::kBucketIdFieldName].toString()
                              << " contains an empty data region",
                _bucketUnpacker.hasNext());
        return _bucketUnpacker.getNext();
    }

    return nextResult;
}

bool DocumentSourceInternalUnpackBucket::pushDownComputedMetaProjection(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    bool nextStageWasRemoved = false;
    if (std::next(itr) == container->end()) {
        return nextStageWasRemoved;
    }
    if (!_bucketUnpacker.bucketSpec().metaField()) {
        return nextStageWasRemoved;
    }

    if (auto nextTransform =
            dynamic_cast<DocumentSourceSingleDocumentTransformation*>(std::next(itr)->get());
        nextTransform &&
        (nextTransform->getType() == TransformerInterface::TransformerType::kInclusionProjection ||
         nextTransform->getType() == TransformerInterface::TransformerType::kComputedProjection)) {

        auto& metaName = _bucketUnpacker.bucketSpec().metaField().get();
        auto [addFieldsSpec, deleteStage] =
            nextTransform->extractComputedProjections(metaName,
                                                      timeseries::kBucketMetaFieldName.toString(),
                                                      BucketUnpacker::reservedBucketFieldNames);
        nextStageWasRemoved = deleteStage;

        if (!addFieldsSpec.isEmpty()) {
            // Extend bucket specification of this stage to include the computed meta projections
            // that are passed through.
            std::vector<StringData> computedMetaProjFields;
            for (auto&& elem : addFieldsSpec) {
                computedMetaProjFields.emplace_back(elem.fieldName());
            }
            _bucketUnpacker.addComputedMetaProjFields(computedMetaProjFields);
            // Insert extracted computed projections before the $_internalUnpackBucket.
            container->insert(
                itr,
                DocumentSourceAddFields::createFromBson(
                    BSON("$addFields" << addFieldsSpec).firstElement(), getContext()));
            // Remove the next stage if it became empty after the field extraction.
            if (deleteStage) {
                container->erase(std::next(itr));
            }
        }
    }
    return nextStageWasRemoved;
}

void DocumentSourceInternalUnpackBucket::internalizeProject(const BSONObj& project,
                                                            bool isInclusion) {
    // 'fields' are the top-level fields to be included/excluded by the unpacker. We handle the
    // special case of _id, which may be excluded in an inclusion $project (or vice versa), here.
    auto fields = project.getFieldNames<std::set<std::string>>();
    if (auto elt = project.getField("_id"); (elt.isBoolean() && elt.Bool() != isInclusion) ||
        (elt.isNumber() && (elt.Int() == 1) != isInclusion)) {
        fields.erase("_id");
    }

    // Update '_bucketUnpacker' state with the new fields and behavior.
    auto spec = _bucketUnpacker.bucketSpec();
    spec.setFieldSet(fields);
    _bucketUnpacker.setBucketSpecAndBehavior(std::move(spec),
                                             isInclusion ? BucketUnpacker::Behavior::kInclude
                                                         : BucketUnpacker::Behavior::kExclude);
}

std::pair<BSONObj, bool> DocumentSourceInternalUnpackBucket::extractOrBuildProjectToInternalize(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) const {
    if (std::next(itr) == container->end() || !_bucketUnpacker.bucketSpec().fieldSet().empty()) {
        // There is no project to internalize or there are already fields being included/excluded.
        return {BSONObj{}, false};
    }

    // Check for a viable inclusion $project after the $_internalUnpackBucket.
    auto [existingProj, isInclusion] = getIncludeExcludeProjectAndType(std::next(itr)->get());
    if (isInclusion && !existingProj.isEmpty() && canInternalizeProjectObj(existingProj)) {
        container->erase(std::next(itr));
        return {existingProj, isInclusion};
    }

    // Attempt to get an inclusion $project representing the root-level dependencies of the pipeline
    // after the $_internalUnpackBucket. If this $project is not empty, then the dependency set was
    // finite.
    Pipeline::SourceContainer restOfPipeline(std::next(itr), container->end());
    auto deps = Pipeline::getDependenciesForContainer(pExpCtx, restOfPipeline, boost::none);
    if (auto dependencyProj =
            deps.toProjectionWithoutMetadata(DepsTracker::TruncateToRootLevel::yes);
        !dependencyProj.isEmpty()) {
        return {dependencyProj, true};
    }

    // Check for a viable exclusion $project after the $_internalUnpackBucket.
    if (!existingProj.isEmpty() && canInternalizeProjectObj(existingProj)) {
        container->erase(std::next(itr));
        return {existingProj, isInclusion};
    }

    return {BSONObj{}, false};
}

std::unique_ptr<MatchExpression>
DocumentSourceInternalUnpackBucket::createPredicatesOnBucketLevelField(
    const MatchExpression* matchExpr) const {
    return BucketSpec::createPredicatesOnBucketLevelField(
        matchExpr,
        _bucketUnpacker.bucketSpec(),
        _bucketMaxSpanSeconds,
        pExpCtx->collationMatchesDefault,
        pExpCtx,
        haveComputedMetaField(),
        _bucketUnpacker.includeMetaField(),
        _assumeNoMixedSchemaData,
        BucketSpec::IneligiblePredicatePolicy::kIgnore);
}

std::pair<BSONObj, bool> DocumentSourceInternalUnpackBucket::extractProjectForPushDown(
    DocumentSource* src) const {
    if (auto nextProject = dynamic_cast<DocumentSourceSingleDocumentTransformation*>(src);
        _bucketUnpacker.bucketSpec().metaField() && nextProject &&
        nextProject->getType() == TransformerInterface::TransformerType::kExclusionProjection) {
        return nextProject->extractProjectOnFieldAndRename(
            _bucketUnpacker.bucketSpec().metaField().get(), timeseries::kBucketMetaFieldName);
    }

    return {BSONObj{}, false};
}

std::pair<bool, Pipeline::SourceContainer::iterator>
DocumentSourceInternalUnpackBucket::rewriteGroupByMinMax(Pipeline::SourceContainer::iterator itr,
                                                         Pipeline::SourceContainer* container) {
    const auto* groupPtr = dynamic_cast<DocumentSourceGroup*>(std::next(itr)->get());
    if (groupPtr == nullptr) {
        return {};
    }

    const auto& idFields = groupPtr->getIdFields();
    if (idFields.size() != 1 || !_bucketUnpacker.bucketSpec().metaField().has_value()) {
        return {};
    }

    const auto& exprId = idFields.cbegin()->second;
    const auto* exprIdPath = dynamic_cast<const ExpressionFieldPath*>(exprId.get());
    if (exprIdPath == nullptr) {
        return {};
    }

    const auto& idPath = exprIdPath->getFieldPath();
    if (idPath.getPathLength() < 2 ||
        idPath.getFieldName(1) != _bucketUnpacker.bucketSpec().metaField().get()) {
        return {};
    }

    bool suitable = true;
    std::vector<AccumulationStatement> accumulationStatements;
    for (const AccumulationStatement& stmt : groupPtr->getAccumulatedFields()) {
        const auto op = stmt.expr.name;
        const bool isMin = op == "$min";
        const bool isMax = op == "$max";

        // Rewrite is valid only for min and max aggregates.
        if (!isMin && !isMax) {
            suitable = false;
            break;
        }

        const auto* exprArg = stmt.expr.argument.get();
        if (const auto* exprArgPath = dynamic_cast<const ExpressionFieldPath*>(exprArg)) {
            const auto& path = exprArgPath->getFieldPath();
            if (path.getPathLength() <= 1 ||
                path.getFieldName(1) == _bucketUnpacker.bucketSpec().timeField()) {
                // Rewrite not valid for time field. We want to eliminate the bucket
                // unpack stage here.
                suitable = false;
                break;
            }

            // Update aggregates to reference the control field.
            std::ostringstream os;
            if (isMin) {
                os << timeseries::kControlMinFieldNamePrefix;
            } else {
                os << timeseries::kControlMaxFieldNamePrefix;
            }

            for (size_t index = 1; index < path.getPathLength(); index++) {
                if (index > 1) {
                    os << ".";
                }
                os << path.getFieldName(index);
            }

            const auto& newExpr = ExpressionFieldPath::createPathFromString(
                pExpCtx.get(), os.str(), pExpCtx->variablesParseState);

            AccumulationExpression accExpr = stmt.expr;
            accExpr.argument = newExpr;
            accumulationStatements.emplace_back(stmt.fieldName, std::move(accExpr));
        }
    }

    if (suitable) {
        std::ostringstream os;
        os << timeseries::kBucketMetaFieldName;
        for (size_t index = 2; index < idPath.getPathLength(); index++) {
            os << "." << idPath.getFieldName(index);
        }
        auto exprId1 = ExpressionFieldPath::createPathFromString(
            pExpCtx.get(), os.str(), pExpCtx->variablesParseState);

        auto newGroup = DocumentSourceGroup::create(pExpCtx,
                                                    std::move(exprId1),
                                                    std::move(accumulationStatements),
                                                    groupPtr->getMaxMemoryUsageBytes());

        // Erase current stage and following group stage, and replace with updated
        // group.
        container->erase(std::next(itr));
        *itr = std::move(newGroup);

        if (itr == container->begin()) {
            // Optimize group stage.
            return {true, itr};
        } else {
            // Give chance of the previous stage to optimize against group stage.
            return {true, std::prev(itr)};
        }
    }

    return {};
}

bool DocumentSourceInternalUnpackBucket::haveComputedMetaField() const {
    return _bucketUnpacker.bucketSpec().metaField() &&
        _bucketUnpacker.bucketSpec().fieldIsComputed(
            _bucketUnpacker.bucketSpec().metaField().get());
}

void addStagesToRetrieveEventLevelFields(Pipeline::SourceContainer& sources,
                                         const Pipeline::SourceContainer::const_iterator unpackIt,
                                         boost::intrusive_ptr<ExpressionContext> expCtx,
                                         boost::intrusive_ptr<DocumentSourceGroup> group,
                                         const boost::optional<std::string&> lastpointTimeField,
                                         bool timeAscending) {
    mongo::stdx::unordered_set<mongo::NamespaceString> nss;
    auto&& ns = expCtx->ns;
    nss.emplace(ns);
    expCtx->addResolvedNamespaces(nss);

    FieldPath metrics("metrics");
    auto lookup = DocumentSourceLookUp::createFromBson(
        BSON(DocumentSourceLookUp::kStageName << BSON(
                 DocumentSourceLookUp::kFromField
                 << ns.coll() << DocumentSourceLookUp::kLocalField << "bucket"
                 << DocumentSourceLookUp::kForeignField << "_id" << DocumentSourceLookUp::kAsField
                 << metrics.fullPath() << DocumentSourceLookUp::kPipelineField
                 << BSON_ARRAY(unpackIt->get()->serializeToBSONForDebug()
                               << BSON(DocumentSourceSort::kStageName << BSON(
                                           lastpointTimeField.get() << (timeAscending ? 1 : -1)))
                               << BSON(DocumentSourceLimit::kStageName << 1))))
            .firstElement(),
        expCtx);
    sources.insert(unpackIt, lookup);

    auto unwind = DocumentSourceUnwind::createFromBson(
        BSON(DocumentSourceUnwind::kStageName << metrics.fullPathWithPrefix()).firstElement(),
        expCtx);
    sources.insert(unpackIt, unwind);

    BSONObjBuilder fields;
    fields << "_id"
           << "$_id";
    for (auto&& accumulator : group->getAccumulatedFields()) {
        auto&& v = accumulator.expr.argument;
        if (auto expr = dynamic_cast<ExpressionFieldPath*>(v.get())) {
            auto&& newPath = metrics.concat(expr->getFieldPath().tail());
            // This is necessary to preserve $first, $last null-check semantics for handling
            // nullish fields, e.g. returning missing field paths as null.
            auto ifNullCheck = BSON(
                "$ifNull" << BSONArray(BSON("0" << ("$" + newPath.fullPath()) << "1" << BSONNULL)));
            fields << StringData{accumulator.fieldName} << ifNullCheck;
        }
    }

    auto replaceWithBson = BSON(DocumentSourceReplaceRoot::kAliasNameReplaceWith << fields.obj());
    auto replaceWith =
        DocumentSourceReplaceRoot::createFromBson(replaceWithBson.firstElement(), expCtx);
    sources.insert(unpackIt, replaceWith);
}

bool DocumentSourceInternalUnpackBucket::optimizeLastpoint(Pipeline::SourceContainer::iterator itr,
                                                           Pipeline::SourceContainer* container) {
    // A lastpoint-type aggregation must contain both a $sort and a $group stage, in that order.
    if (std::next(itr, 2) == container->end()) {
        return false;
    }

    auto sortStage = dynamic_cast<DocumentSourceSort*>(std::next(itr)->get());
    auto groupStage = dynamic_cast<DocumentSourceGroup*>(std::next(itr, 2)->get());

    if (!sortStage || !groupStage) {
        return false;
    }

    if (sortStage->hasLimit()) {
        // This $sort stage was previously followed by a $limit stage.
        return false;
    }

    auto spec = _bucketUnpacker.bucketSpec();
    auto maybeMetaField = spec.metaField();
    auto timeField = spec.timeField();
    if (!maybeMetaField || haveComputedMetaField()) {
        return false;
    }

    auto metaField = maybeMetaField.get();
    if (!checkMetadataSortReorder(sortStage->getSortKeyPattern(), metaField, timeField)) {
        return false;
    }

    auto groupIdFields = groupStage->getIdFields();
    if (groupIdFields.size() != 1) {
        return false;
    }

    auto groupId = dynamic_cast<ExpressionFieldPath*>(groupIdFields.cbegin()->second.get());
    if (!groupId || groupId->isVariableReference()) {
        return false;
    }

    const auto fieldPath = groupId->getFieldPath();
    if (fieldPath.getPathLength() <= 1 || fieldPath.tail().getFieldName(0) != metaField) {
        return false;
    }

    auto newFieldPath = FieldPath(timeseries::kBucketMetaFieldName);
    if (fieldPath.tail().getPathLength() > 1) {
        newFieldPath = newFieldPath.concat(fieldPath.tail().tail());
    }

    // Insert bucket-level $sort and $group stages before we unpack any buckets.
    boost::intrusive_ptr<DocumentSourceSort> newSort;
    auto insertBucketLevelSortAndGroup = [&](bool flipSort) {
        newSort = createMetadataSortForReorder(*sortStage, timeField, flipSort);
        auto newGroup = createGroupForReorder(pExpCtx, newFieldPath);
        container->insert(itr, newSort);
        container->insert(itr, newGroup);
    };

    auto accumulators = groupStage->getAccumulatedFields();
    auto groupOnlyUsesTargetAccum = [&](AccumulatorDocumentsNeeded targetAccum) {
        return std::all_of(accumulators.begin(), accumulators.end(), [&](auto&& accum) {
            return targetAccum == accum.makeAccumulator()->documentsNeeded();
        });
    };

    std::string newTimeField;
    if (groupOnlyUsesTargetAccum(AccumulatorDocumentsNeeded::kFirstDocument)) {
        insertBucketLevelSortAndGroup(false);
        newTimeField = timeseries::kControlMinFieldNamePrefix + timeField;
    } else if (groupOnlyUsesTargetAccum(AccumulatorDocumentsNeeded::kLastDocument)) {
        insertBucketLevelSortAndGroup(true);
        newTimeField = timeseries::kControlMaxFieldNamePrefix + timeField;
    } else {
        return false;
    }

    // Add $lookup, $unwind and $replaceWith stages.
    addStagesToRetrieveEventLevelFields(
        *container,
        itr,
        pExpCtx,
        groupStage,
        timeField,
        isLastpointSortTimeAscending(newSort->getSortKeyPattern(), newTimeField));

    // Remove the $sort, $group and $_internalUnpackBucket stages.
    tassert(6165401,
            "unexpected stage in lastpoint aggregate, expected $_internalUnpackBucket",
            itr->get()->getSourceName() == kStageNameInternal);
    itr = container->erase(itr);

    tassert(6165402,
            "unexpected stage in lastpoint aggregate, expected $sort",
            itr->get()->getSourceName() == DocumentSourceSort::kStageName);
    itr = container->erase(itr);

    tassert(6165403,
            "unexpected stage in lastpoint aggregate, expected $group",
            itr->get()->getSourceName() == DocumentSourceGroup::kStageName);
    container->erase(itr);

    return true;
}

Pipeline::SourceContainer::iterator DocumentSourceInternalUnpackBucket::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // Some optimizations may not be safe to do if we have computed the metaField via an $addFields
    // or a computed $project. We won't do those optimizations if 'haveComputedMetaField' is true.
    bool haveComputedMetaField = this->haveComputedMetaField();

    // Before any other rewrites for the current stage, consider reordering with $sort.
    if (auto sortPtr = dynamic_cast<DocumentSourceSort*>(std::next(itr)->get())) {
        if (auto metaField = _bucketUnpacker.bucketSpec().metaField();
            metaField && !haveComputedMetaField) {
            if (checkMetadataSortReorder(sortPtr->getSortKeyPattern(), metaField.get())) {
                // We have a sort on metadata field following this stage. Reorder the two stages
                // and return a pointer to the preceding stage.
                auto sortForReorder = createMetadataSortForReorder(*sortPtr);

                // If the original sort had a limit, we will not preserve that in the swapped sort.
                // Instead we will add a $limit to the end of the pipeline to keep the number of
                // expected results.
                if (auto limit = sortPtr->getLimit(); limit && *limit != 0) {
                    container->push_back(DocumentSourceLimit::create(pExpCtx, *limit));
                }

                // Reorder sort and current doc.
                *std::next(itr) = std::move(*itr);
                *itr = std::move(sortForReorder);

                if (itr == container->begin()) {
                    // Try to optimize the current stage again.
                    return std::next(itr);
                } else {
                    // Try to optimize the previous stage against $sort.
                    return std::prev(itr);
                }
            }
        }
    }

    // Attempt to push geoNear on the metaField past $_internalUnpackBucket.
    if (auto nextNear = dynamic_cast<DocumentSourceGeoNear*>(std::next(itr)->get())) {
        // Currently we only support geo indexes on the meta field, and we enforce this by
        // requiring the key field to be set so we can check before we try to look up indexes.
        auto keyField = nextNear->getKeyField();
        uassert(5892921,
                "Must specify 'key' option for $geoNear on a time-series collection",
                keyField);

        // Currently we do not support query for $geoNear on a bucket
        uassert(
            1938439,
            "Must not specify 'query' for $geoNear on a time-series collection; use $match instead",
            nextNear->getQuery().binaryEqual(BSONObj()));

        auto metaField = _bucketUnpacker.bucketSpec().metaField();
        if (metaField && *metaField == keyField->front()) {
            // Make sure we actually re-write the key field for the buckets collection so we can
            // locate the index.
            static const FieldPath baseMetaFieldPath{timeseries::kBucketMetaFieldName};
            nextNear->setKeyField(keyField->getPathLength() > 1
                                      ? baseMetaFieldPath.concat(keyField->tail())
                                      : baseMetaFieldPath);

            // Save the source, remove it, and then push it down.
            auto source = *std::next(itr);
            container->erase(std::next(itr));
            container->insert(itr, source);
            return std::prev(itr) == container->begin() ? std::prev(itr)
                                                        : std::prev(std::prev(itr));
        } else {
            // Don't push down query on measurements.
        }
    }

    // Optimize the pipeline after this stage to merge $match stages and push them forward, and to
    // take advantage of $expr rewrite optimizations.
    if (!_optimizedEndOfPipeline) {
        _optimizedEndOfPipeline = true;

        if (std::next(itr) == container->end()) {
            return container->end();
        }
        if (auto nextStage = dynamic_cast<DocumentSourceGeoNear*>(std::next(itr)->get())) {
            // If the end of the pipeline starts with a $geoNear stage, make sure it gets optimized
            // in a context where it knows there are other stages before it. It will split itself
            // up into separate $match and $sort stages. But it doesn't split itself up when it's
            // the first stage, because it expects to use a special DocumentSouceGeoNearCursor plan.
            nextStage->optimizeAt(std::next(itr), container);
        }

        Pipeline::optimizeEndOfPipeline(itr, container);
        if (std::next(itr) == container->end()) {
            return container->end();
        } else {
            // Kick back out to optimizing this stage again a level up, so any matches that were
            // moved to directly after this stage can be moved before it if possible.
            return itr;
        }
    }
    {
        // Check if we can avoid unpacking if we have a group stage with min/max aggregates.
        auto [success, result] = rewriteGroupByMinMax(itr, container);
        if (success) {
            return result;
        }
    }

    {
        // Check if the rest of the pipeline needs any fields. For example we might only be
        // interested in $count.
        auto deps = Pipeline::getDependenciesForContainer(
            pExpCtx, Pipeline::SourceContainer{std::next(itr), container->end()}, boost::none);
        if (deps.hasNoRequirements()) {
            _bucketUnpacker.setBucketSpecAndBehavior({_bucketUnpacker.bucketSpec().timeField(),
                                                      _bucketUnpacker.bucketSpec().metaField(),
                                                      {}},
                                                     BucketUnpacker::Behavior::kInclude);

            // Keep going for next optimization.
        }
    }

    // Attempt to optimize last-point type queries.
    if (feature_flags::gfeatureFlagLastPointQuery.isEnabled(
            serverGlobalParams.featureCompatibility) &&
        optimizeLastpoint(itr, container)) {
        // If we are able to rewrite the aggregation, give the resulting pipeline a chance to
        // perform further optimizations.
        return container->begin();
    };

    // Attempt to map predicates on bucketed fields to predicates on the control field.
    if (auto nextMatch = dynamic_cast<DocumentSourceMatch*>(std::next(itr)->get());
        nextMatch && !_triedBucketLevelFieldsPredicatesPushdown) {
        _triedBucketLevelFieldsPredicatesPushdown = true;

        if (auto match = createPredicatesOnBucketLevelField(nextMatch->getMatchExpression())) {
            BSONObjBuilder bob;
            match->serialize(&bob);
            container->insert(itr, DocumentSourceMatch::create(bob.obj(), pExpCtx));

            // Give other stages a chance to optimize with the new $match.
            return std::prev(itr) == container->begin() ? std::prev(itr)
                                                        : std::prev(std::prev(itr));
        }
    }

    // Attempt to push down a $project on the metaField past $_internalUnpackBucket.
    if (!haveComputedMetaField) {
        if (auto [metaProject, deleteRemainder] = extractProjectForPushDown(std::next(itr)->get());
            !metaProject.isEmpty()) {
            container->insert(itr,
                              DocumentSourceProject::createFromBson(
                                  BSON("$project" << metaProject).firstElement(), getContext()));

            if (deleteRemainder) {
                // We have pushed down the entire $project. Remove the old $project from the
                // pipeline, then attempt to optimize this stage again.
                container->erase(std::next(itr));
                return std::prev(itr) == container->begin() ? std::prev(itr)
                                                            : std::prev(std::prev(itr));
            }
        }
    }

    // Attempt to extract computed meta projections from subsequent $project, $addFields, or $set
    // and push them before the $_internalunpackBucket.
    if (pushDownComputedMetaProjection(itr, container)) {
        // We've pushed down and removed a stage after this one. Try to optimize the new stage.
        return std::prev(itr) == container->begin() ? std::prev(itr) : std::prev(std::prev(itr));
    }

    // Attempt to build a $project based on dependency analysis or extract one from the pipeline. We
    // can internalize the result so we can handle projections during unpacking.
    if (!_triedInternalizeProject) {
        if (auto [project, isInclusion] = extractOrBuildProjectToInternalize(itr, container);
            !project.isEmpty()) {
            _triedInternalizeProject = true;
            internalizeProject(project, isInclusion);

            // We may have removed a $project after this stage, so we try to optimize this stage
            // again.
            return itr;
        }
    }

    return container->end();
}

DocumentSource::GetModPathsReturn DocumentSourceInternalUnpackBucket::getModifiedPaths() const {
    if (_bucketUnpacker.includeMetaField()) {
        StringMap<std::string> renames;
        renames.emplace(*_bucketUnpacker.bucketSpec().metaField(),
                        timeseries::kBucketMetaFieldName);
        return {GetModPathsReturn::Type::kAllExcept, std::set<std::string>{}, std::move(renames)};
    }
    return {GetModPathsReturn::Type::kAllPaths, std::set<std::string>{}, {}};
}

}  // namespace mongo
