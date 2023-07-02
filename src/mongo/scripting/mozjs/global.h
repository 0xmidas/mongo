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

#include <js/Class.h>
#include <js/PropertySpec.h>

#include "mongo/scripting/mozjs/base.h"
#include "mongo/scripting/mozjs/wraptype.h"

namespace mongo {
namespace mozjs {

/**
 * The global object for all of our JS.
 *
 * This function is super special and it's properties are the globally visible
 * symbol for JS execution.
 */
struct GlobalInfo : public BaseInfo {
    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(sleep);
        MONGO_DECLARE_JS_FUNCTION(gc);
        MONGO_DECLARE_JS_FUNCTION(print);
        MONGO_DECLARE_JS_FUNCTION(version);
        MONGO_DECLARE_JS_FUNCTION(buildInfo);
        MONGO_DECLARE_JS_FUNCTION(getJSHeapLimitMB);
    };

    static const JSFunctionSpec freeFunctions[7];

    static const char* const className;
    static const unsigned classFlags = JSCLASS_GLOBAL_FLAGS;
};

}  // namespace mozjs
}  // namespace mongo
