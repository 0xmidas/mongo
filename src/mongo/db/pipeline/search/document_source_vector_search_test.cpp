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

#include "mongo/db/pipeline/search/document_source_vector_search.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"


namespace mongo {
namespace {

using boost::intrusive_ptr;
using DocumentSourceVectorSearchTest = AggregationContextFixture;

TEST_F(DocumentSourceVectorSearchTest, NotAllowedInTransaction) {
    auto expCtx = getExpCtx();
    expCtx->uuid = UUID::gen();
    expCtx->opCtx->setInMultiDocumentTransaction();


    auto spec = fromjson(R"({
        $vectorSearch: {
            queryVector: [1.0, 2.0],
            path: "x",
            numCandidates: 100,
            limit: 10
        }
    })");

    auto vectorStage = DocumentSourceVectorSearch::createFromBson(spec.firstElement(), expCtx);
    ASSERT_THROWS_CODE(Pipeline::create({vectorStage}, expCtx),
                       AssertionException,
                       ErrorCodes::OperationNotSupportedInTransaction);
}

TEST_F(DocumentSourceVectorSearchTest, NotAllowedInvalidFilter) {
    // The only invalid filter is invalid MQL.
    auto spec = fromjson(R"({
        $vectorSearch: {
            queryVector: [1.0, 2.0],
            path: "x",
            numCandidates: 100,
            limit: 10,
            filter: {
                x: {
                    "$gibberish": false
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceVectorSearch::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceVectorSearchTest, NotAllowedLimitIncorrectType) {
    // Limit argument must be correct type
    auto spec = fromjson(R"({
        $vectorSearch: {
            queryVector: [1.0, 2.0],
            path: "x",
            numCandidates: 5,
            limit: "str"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceVectorSearch::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       8575100);
}

TEST_F(DocumentSourceVectorSearchTest, UnexpectedOrMissingArgumentsAllowed) {
    auto spec = fromjson(R"({
        $vectorSearch: {
            queryVector: [1.0, 2.0],
            numCandidates: 100,
            path: "x",
            filter: {
                x: {
                    "$gt": 5
                }
            }
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceVectorSearch::createFromBson(spec.firstElement(), getExpCtx()));

    spec = fromjson(R"({
        $vectorSearch: {
            queryVector: [1.0, 2.0],
            numCandidates: 100,
            path: "x",
            limit: 10,
            extra: "Here!",
            filter: {
                x: {
                    "$gt": 5
                }
            }
        }
    })");
    ASSERT_DOES_NOT_THROW(
        DocumentSourceVectorSearch::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceVectorSearchTest, UnexpectedArgumentIsSerialized) {
    auto spec = fromjson(R"({
        $vectorSearch: {
            queryVector: [1.0, 2.0],
            numCandidates: 100,
            path: "x",
            limit: 10,
            extra: "Here!",
            filter: {
                x: {
                    "$gt": 5
                }
            }
        }
    })");
    auto dsVectorSearch =
        DocumentSourceVectorSearch::createFromBson(spec.firstElement(), getExpCtx());
    std::vector<Value> vec;
    dsVectorSearch.front()->serializeToArray(vec);
    ASSERT(
        !vec[0].getDocument().getField("$vectorSearch").getDocument().getField("extra").missing());
}

TEST_F(DocumentSourceVectorSearchTest, EOFWhenCollDoesNotExist) {
    auto expCtx = getExpCtx();

    auto spec = fromjson(R"({
        $vectorSearch: {
            queryVector: [1.0, 2.0],
            path: "x",
            numCandidates: 100,
            limit: 10
        }
    })");

    auto vectorStage = DocumentSourceVectorSearch::createFromBson(spec.firstElement(), expCtx);
    ASSERT_TRUE(vectorStage.front()->getNext().isEOF());
}

TEST_F(DocumentSourceVectorSearchTest, HasTheCorrectStagesWhenCreated) {
    // We want the mock to return true for isExpectedToExecuteQueries() since that will enable
    // insertion of the idLookup stage. That means we also need mongotHost to be configured to
    // avoid the uassert with SearchNotEnabled error.
    RAIIServerParameterControllerForTest controller("mongotHost", "localhost:27017");
    auto expCtx = getExpCtx();
    struct MockMongoInterface final : public StubMongoProcessInterface {
        bool inShardedEnvironment(OperationContext* opCtx) const override {
            return false;
        }

        bool isExpectedToExecuteQueries() override {
            return true;
        }
    };
    expCtx->mongoProcessInterface = std::make_unique<MockMongoInterface>();

    auto spec = fromjson(R"({
        $vectorSearch: {
            queryVector: [1.0, 2.0],
            path: "x",
            numCandidates: 100,
            limit: 10
        }
    })");

    auto vectorStage = DocumentSourceVectorSearch::createFromBson(spec.firstElement(), expCtx);
    ASSERT_EQUALS(vectorStage.size(), 2UL);

    const auto* vectorSearchStage =
        dynamic_cast<DocumentSourceVectorSearch*>(vectorStage.front().get());
    ASSERT(vectorSearchStage);

    const auto* idLookupStage =
        dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(vectorStage.back().get());
    ASSERT(idLookupStage);
}

TEST_F(DocumentSourceVectorSearchTest, RedactsCorrectly) {
    auto spec = fromjson(R"({
        $vectorSearch: {
            queryVector: [1.0, 2.0],
            path: "x",
            numCandidates: 100,
            limit: 10,
            index: "x_index",
            filter: {
                x: {
                    "$gt": 0
                }
            }
        }
    })");

    auto vectorStage = DocumentSourceVectorSearch::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$vectorSearch":"?object"
        })",
        redact(*(vectorStage.front())));
}

}  // namespace
}  // namespace mongo
