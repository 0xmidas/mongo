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

#pragma once

#include <memory>
#include <utility>

#include <js/Class.h>
#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

#include "mongo/client/dbclient_cursor.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/mozjs/base.h"
#include "mongo/scripting/mozjs/wraptype.h"

namespace mongo {
namespace mozjs {

/**
 * Wraps a DBClientCursor in javascript
 *
 * Note that the install is private, so this class should only be constructible
 * from C++. Current callers are all via the Mongo object.
 */
struct CursorInfo : public BaseInfo {
    enum Slots { CursorHolderSlot, CursorInfoSlotCount };

    static void finalize(JS::GCContext* gcCtx, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(close);
        MONGO_DECLARE_JS_FUNCTION(hasNext);
        MONGO_DECLARE_JS_FUNCTION(isClosed);
        MONGO_DECLARE_JS_FUNCTION(next);
        MONGO_DECLARE_JS_FUNCTION(objsLeftInBatch);
        MONGO_DECLARE_JS_FUNCTION(readOnly);
        MONGO_DECLARE_JS_FUNCTION(getId);
        MONGO_DECLARE_JS_FUNCTION(hasMoreToCome);
    };

    static const JSFunctionSpec methods[9];

    static const char* const className;
    static const unsigned classFlags =
        JSCLASS_HAS_RESERVED_SLOTS(CursorInfoSlotCount) | BaseInfo::finalizeFlag;
    static const InstallType installType = InstallType::Private;

    /**
     * We need this because the DBClientBase can go out of scope before all of
     * its children (as in global shutdown). So we have to manage object
     * lifetimes in C++ land.
     */
    struct CursorHolder {
        CursorHolder(std::unique_ptr<DBClientCursor> cursor, std::shared_ptr<DBClientBase> client)
            : client(std::move(client)), cursor(std::move(cursor)) {}

        std::shared_ptr<DBClientBase> client;
        std::unique_ptr<DBClientCursor> cursor;
    };
};

}  // namespace mozjs
}  // namespace mongo
