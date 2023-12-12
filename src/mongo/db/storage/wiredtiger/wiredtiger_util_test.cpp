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

#include "mongo/platform/basic.h"

#include <sstream>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/system_clock_source.h"

namespace mongo {

using std::string;
using std::stringstream;

class WiredTigerConnection {
public:
    WiredTigerConnection(StringData dbpath, StringData extraStrings) : _conn(nullptr) {
        std::stringstream ss;
        ss << "create,";
        ss << extraStrings;
        string config = ss.str();
        _fastClockSource = std::make_unique<SystemClockSource>();
        int ret = wiredtiger_open(dbpath.toString().c_str(), nullptr, config.c_str(), &_conn);
        ASSERT_OK(wtRCToStatus(ret, nullptr));
        ASSERT(_conn);
    }
    ~WiredTigerConnection() {
        _conn->close(_conn, nullptr);
    }
    WT_CONNECTION* getConnection() const {
        return _conn;
    }
    ClockSource* getClockSource() {
        return _fastClockSource.get();
    }

private:
    WT_CONNECTION* _conn;
    std::unique_ptr<ClockSource> _fastClockSource;
};

class WiredTigerUtilHarnessHelper {
public:
    WiredTigerUtilHarnessHelper(StringData extraStrings)
        : _dbpath("wt_test"),
          _connection(_dbpath.path(), extraStrings),
          _sessionCache(_connection.getConnection(), _connection.getClockSource()) {}


    WiredTigerSessionCache* getSessionCache() {
        return &_sessionCache;
    }

    WiredTigerOplogManager* getOplogManager() {
        return &_oplogManager;
    }

    OperationContext* newOperationContext() {
        return new OperationContextNoop(
            new WiredTigerRecoveryUnit(getSessionCache(), &_oplogManager));
    }

private:
    unittest::TempDir _dbpath;
    WiredTigerConnection _connection;
    WiredTigerSessionCache _sessionCache;
    WiredTigerOplogManager _oplogManager;
};

class WiredTigerUtilMetadataTest : public mongo::unittest::Test {
public:
    virtual void setUp() {
        _harnessHelper.reset(new WiredTigerUtilHarnessHelper(""));
        _opCtx.reset(_harnessHelper->newOperationContext());
    }

    virtual void tearDown() {
        _opCtx.reset(nullptr);
        _harnessHelper.reset(nullptr);
    }

protected:
    const char* getURI() const {
        return "table:mytable";
    }

    OperationContext* getOperationContext() const {
        ASSERT(_opCtx.get());
        return _opCtx.get();
    }

    void createSession(const char* config) {
        WT_SESSION* wtSession =
            WiredTigerRecoveryUnit::get(_opCtx.get())->getSession()->getSession();
        ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, getURI(), config), wtSession));
    }

private:
    std::unique_ptr<WiredTigerUtilHarnessHelper> _harnessHelper;
    std::unique_ptr<OperationContext> _opCtx;
};

TEST_F(WiredTigerUtilMetadataTest, GetMetadataCreateInvalid) {
    StatusWith<std::string> result =
        WiredTigerUtil::getMetadataCreate(getOperationContext(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetMetadataCreateNull) {
    const char* config = nullptr;
    createSession(config);
    StatusWith<std::string> result =
        WiredTigerUtil::getMetadataCreate(getOperationContext(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().empty());
}

TEST_F(WiredTigerUtilMetadataTest, GetMetadataCreateStringSimple) {
    const char* config = "app_metadata=(abc=123)";
    createSession(config);
    StatusWith<std::string> result =
        WiredTigerUtil::getMetadataCreate(getOperationContext(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_STRING_CONTAINS(result.getValue(), config);
}

TEST_F(WiredTigerUtilMetadataTest, GetConfigurationStringInvalidURI) {
    StatusWith<std::string> result = WiredTigerUtil::getMetadata(getOperationContext(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetConfigurationStringNull) {
    const char* config = nullptr;
    createSession(config);
    StatusWith<std::string> result = WiredTigerUtil::getMetadata(getOperationContext(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().empty());
}

TEST_F(WiredTigerUtilMetadataTest, GetConfigurationStringSimple) {
    const char* config = "app_metadata=(abc=123)";
    createSession(config);
    StatusWith<std::string> result = WiredTigerUtil::getMetadata(getOperationContext(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_STRING_CONTAINS(result.getValue(), config);
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataInvalidURI) {
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(getOperationContext(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataNull) {
    const char* config = nullptr;
    createSession(config);
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(getOperationContext(), getURI());
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().isEmpty());
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataString) {
    const char* config = "app_metadata=\"abc\"";
    createSession(config);
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(getOperationContext(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataDuplicateKeys) {
    const char* config = "app_metadata=(abc=123,abc=456)";
    createSession(config);
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(getOperationContext(), getURI());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(50998, result.getStatus().code());
}

TEST_F(WiredTigerUtilMetadataTest, GetApplicationMetadataTypes) {
    const char* config =
        "app_metadata=(stringkey=\"abc\",boolkey1=true,boolkey2=false,"
        "idkey=def,numkey=123,"
        "structkey=(k1=v2,k2=v2))";
    createSession(config);
    StatusWith<BSONObj> result =
        WiredTigerUtil::getApplicationMetadata(getOperationContext(), getURI());
    ASSERT_OK(result.getStatus());
    const BSONObj& obj = result.getValue();

    BSONElement stringElement = obj.getField("stringkey");
    ASSERT_EQUALS(mongo::String, stringElement.type());
    ASSERT_EQUALS("abc", stringElement.String());

    BSONElement boolElement1 = obj.getField("boolkey1");
    ASSERT_TRUE(boolElement1.isBoolean());
    ASSERT_TRUE(boolElement1.boolean());

    BSONElement boolElement2 = obj.getField("boolkey2");
    ASSERT_TRUE(boolElement2.isBoolean());
    ASSERT_FALSE(boolElement2.boolean());

    BSONElement identifierElement = obj.getField("idkey");
    ASSERT_EQUALS(mongo::String, identifierElement.type());
    ASSERT_EQUALS("def", identifierElement.String());

    BSONElement numberElement = obj.getField("numkey");
    ASSERT_TRUE(numberElement.isNumber());
    ASSERT_EQUALS(123, numberElement.numberInt());

    BSONElement structElement = obj.getField("structkey");
    ASSERT_EQUALS(mongo::String, structElement.type());
    ASSERT_EQUALS("(k1=v2,k2=v2)", structElement.String());
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatVersionMissingKey) {
    createSession("app_metadata=(abc=123)");
    ASSERT_OK(WiredTigerUtil::checkApplicationMetadataFormatVersion(
        getOperationContext(), getURI(), 1, 1));
    ASSERT_NOT_OK(WiredTigerUtil::checkApplicationMetadataFormatVersion(
        getOperationContext(), getURI(), 2, 2));
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatVersionString) {
    createSession("app_metadata=(formatVersion=\"bar\")");
    ASSERT_NOT_OK(WiredTigerUtil::checkApplicationMetadataFormatVersion(
        getOperationContext(), getURI(), 1, 1));
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatVersionNumber) {
    createSession("app_metadata=(formatVersion=2)");
    ASSERT_EQUALS(
        WiredTigerUtil::checkApplicationMetadataFormatVersion(getOperationContext(), getURI(), 2, 3)
            .getValue(),
        2);
    ASSERT_NOT_OK(WiredTigerUtil::checkApplicationMetadataFormatVersion(
        getOperationContext(), getURI(), 1, 1));
    ASSERT_NOT_OK(WiredTigerUtil::checkApplicationMetadataFormatVersion(
        getOperationContext(), getURI(), 3, 3));
}

TEST_F(WiredTigerUtilMetadataTest, CheckApplicationMetadataFormatInvalidURI) {
    createSession("\"");
    Status result =
        WiredTigerUtil::checkApplicationMetadataFormatVersion(getOperationContext(), getURI(), 0, 3)
            .getStatus();
    ASSERT_NOT_OK(result);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, result.code());
}

TEST(WiredTigerUtilTest, GetStatisticsValueMissingTable) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    WiredTigerRecoveryUnit recoveryUnit(harnessHelper.getSessionCache(),
                                        harnessHelper.getOplogManager());
    WiredTigerSession* session = recoveryUnit.getSession();
    auto result = WiredTigerUtil::getStatisticsValue(session->getSession(),
                                                     "statistics:table:no_such_table",
                                                     "statistics=(fast)",
                                                     WT_STAT_DSRC_BLOCK_SIZE);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::CursorNotFound, result.getStatus().code());
}

TEST(WiredTigerUtilTest, GetStatisticsValueStatisticsDisabled) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(none)");
    WiredTigerRecoveryUnit recoveryUnit(harnessHelper.getSessionCache(),
                                        harnessHelper.getOplogManager());
    WiredTigerSession* session = recoveryUnit.getSession();
    WT_SESSION* wtSession = session->getSession();
    ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, "table:mytable", nullptr), wtSession));
    auto result = WiredTigerUtil::getStatisticsValue(session->getSession(),
                                                     "statistics:table:mytable",
                                                     "statistics=(fast)",
                                                     WT_STAT_DSRC_BLOCK_SIZE);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::CursorNotFound, result.getStatus().code());
}

TEST(WiredTigerUtilTest, GetStatisticsValueInvalidKey) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    WiredTigerRecoveryUnit recoveryUnit(harnessHelper.getSessionCache(),
                                        harnessHelper.getOplogManager());
    WiredTigerSession* session = recoveryUnit.getSession();
    WT_SESSION* wtSession = session->getSession();
    ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, "table:mytable", nullptr), wtSession));
    // Use connection statistics key which does not apply to a table.
    auto result = WiredTigerUtil::getStatisticsValue(session->getSession(),
                                                     "statistics:table:mytable",
                                                     "statistics=(fast)",
                                                     WT_STAT_CONN_SESSION_OPEN);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, result.getStatus().code());
}

TEST(WiredTigerUtilTest, GetStatisticsValueValidKey) {
    WiredTigerUtilHarnessHelper harnessHelper("statistics=(all)");
    WiredTigerRecoveryUnit recoveryUnit(harnessHelper.getSessionCache(),
                                        harnessHelper.getOplogManager());
    WiredTigerSession* session = recoveryUnit.getSession();
    WT_SESSION* wtSession = session->getSession();
    ASSERT_OK(wtRCToStatus(wtSession->create(wtSession, "table:mytable", nullptr), wtSession));
    // Use connection statistics key which does not apply to a table.
    auto result = WiredTigerUtil::getStatisticsValue(session->getSession(),
                                                     "statistics:table:mytable",
                                                     "statistics=(fast)",
                                                     WT_STAT_DSRC_LSM_CHUNK_COUNT);
    ASSERT_OK(result.getStatus());
    // Expect statistics value to be zero for a LSM key on a Btree.
    ASSERT_EQUALS(0U, result.getValue());
}

TEST(WiredTigerUtilTest, RemoveEncryptionFromConfigString) {
    {  // Found at the middle.
        std::string input{
            "debug_mode=(table_logging=true,checkpoint_retention=4),encryption=(name=AES256-CBC,"
            "keyid="
            "\".system\"),extensions=[local={entry=mongo_addWiredTigerEncryptors,early_load=true},,"
            "],"};
        const std::string expectedOutput{
            "debug_mode=(table_logging=true,checkpoint_retention=4),extensions=[local={entry=mongo_"
            "addWiredTigerEncryptors,early_load=true},,],"};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
    {  // Found at start.
        std::string input{
            "encryption=(name=AES256-CBC,keyid=\".system\"),extensions=[local={entry=mongo_"
            "addWiredTigerEncryptors,early_load=true},,],"};
        const std::string expectedOutput{
            "extensions=[local={entry=mongo_addWiredTigerEncryptors,early_load=true},,],"};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
    {  // Found at the end.
        std::string input{
            "debug_mode=(table_logging=true,checkpoint_retention=4),encryption=(name=AES256-CBC,"
            "keyid=\".system\")"};
        const std::string expectedOutput{"debug_mode=(table_logging=true,checkpoint_retention=4),"};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
    {  // Matches full configString.
        std::string input{"encryption=(name=AES256-CBC,keyid=\".system\")"};
        const std::string expectedOutput{""};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
    {  // Matches full configString, trailing comma.
        std::string input{"encryption=(name=AES256-CBC,keyid=\".system\"),"};
        const std::string expectedOutput{""};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
    {  // No match.
        std::string input{"debug_mode=(table_logging=true,checkpoint_retention=4)"};
        const std::string expectedOutput{"debug_mode=(table_logging=true,checkpoint_retention=4)"};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
    {  // No match, empty.
        std::string input{""};
        const std::string expectedOutput{""};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
    {  // Removes multiple instances.
        std::string input{
            "encryption=(name=AES256-CBC,keyid=\".system\"),debug_mode=(table_logging=true,"
            "checkpoint_retention=4),encryption=(name=AES256-CBC,keyid=\".system\")"};
        const std::string expectedOutput{"debug_mode=(table_logging=true,checkpoint_retention=4),"};
        WiredTigerUtil::removeEncryptionFromConfigString(&input);
        ASSERT_EQUALS(input, expectedOutput);
    }
}

TEST(WiredTigerUtilTest, GetSanitizedStorageOptionsForSecondaryReplication) {
    {  // Empty storage options.
        auto input = BSONObj();
        auto expectedOutput = input;
        auto output = WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(input);
        ASSERT_BSONOBJ_EQ(output, expectedOutput);
    }
    {
        // Preserve WT config string without encryption options.
        auto input = BSON("wiredTiger" << BSON("configString"
                                               << "split_pct=88"));
        auto expectedOutput = input;
        auto output = WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(input);
        ASSERT_BSONOBJ_EQ(output, expectedOutput);
    }
    {
        // Remove encryption options from WT config string in results.
        auto input = BSON(
            "wiredTiger" << BSON("configString"
                                 << "encryption=(name=AES256-CBC,keyid=\".system\"),split_pct=88"));
        auto expectedOutput = BSON("wiredTiger" << BSON("configString"
                                                        << "split_pct=88"));
        auto output = WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(input);
        ASSERT_BSONOBJ_EQ(output, expectedOutput);
    }
    {
        // Leave non-WT settings intact.
        auto input = BSON("inMemory" << BSON("configString"
                                             << "split_pct=66"));
        auto expectedOutput = input;
        auto output = WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(input);
        ASSERT_BSONOBJ_EQ(output, expectedOutput);
    }
    {
        // Change only WT settings in storage options containing a mix of WT and non-WT settings.
        auto input = BSON(
            "inMemory" << BSON("configString"
                               << "split_pct=66")
                       << "wiredTiger"
                       << BSON("configString"
                               << "encryption=(name=AES256-CBC,keyid=\".system\"),split_pct=88"));
        auto expectedOutput = BSON("inMemory" << BSON("configString"
                                                      << "split_pct=66")
                                              << "wiredTiger"
                                              << BSON("configString"
                                                      << "split_pct=88"));
        auto output = WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(input);
        ASSERT_BSONOBJ_EQ(output, expectedOutput);
    }
}

}  // namespace mongo
