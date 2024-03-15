/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Functionality provided for the JSM component loader in Gecko, that requires
 * its own unique manner of global environment and currently requires assistance
 * from SpiderMonkey to do so.
 *
 * Embedders who aren't Gecko can ignore this header.
 */

#ifndef js_friend_JSMEnvironment_h
#define js_friend_JSMEnvironment_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/GCVector.h"  // JS::StackGCVector
#include "js/TypeDecls.h"

// A 'JSMEnvironment' refers to an environment chain constructed for JSM loading
// in a shared global. Internally it is a NonSyntacticVariablesObject with a
// corresponding extensible LexicalEnvironmentObject that is accessible by
// JS_ExtensibleLexicalEnvironment. The |this| value of that lexical environment
// is the NSVO itself.
//
// Normal global environment (ES6):     JSM "global" environment:
//
//                                      * - extensible lexical environment
//                                      |   (code runs in this environment;
//                                      |    `let/const` bindings go here)
//                                      |
//                                      * - JSMEnvironment (=== `this`)
//                                      |   (`var` bindings go here)
//                                      |
// * - extensible lexical environment   * - extensible lexical environment
// |   (code runs in this environment;  |   (empty)
// |    `let/const` bindings go here)   |
// |                                    |
// * - actual global (=== `this`)       * - shared JSM global
//     (var bindings go here; and           (Object, Math, etc. live here)
//      Object, Math, etc. live here)

namespace JS {

/**
 * Allocate a new environment in the current compartment that is compatible with
 * JSM shared loading.
 */
extern JS_PUBLIC_API JSObject* NewJSMEnvironment(JSContext* cx);

/**
 * Execute the given script (copied into the current compartment if necessary)
 * in the given JSMEnvironment.  The script must have been compiled for
 * hasNonSyntacticScope.  The |jsmEnv| must have been previously allocated by
 * |NewJSMEnvironment|.
 *
 * NOTE: The associated extensible lexical environment is reused.
 */
extern JS_PUBLIC_API bool ExecuteInJSMEnvironment(JSContext* cx,
                                                  Handle<JSScript*> script,
                                                  Handle<JSObject*> jsmEnv);

// Additionally, target objects may be specified as required by the Gecko
// subscript loader. These are wrapped in non-syntactic WithEnvironments and
// temporarily placed on the environment chain.
extern JS_PUBLIC_API bool ExecuteInJSMEnvironment(
    JSContext* cx, Handle<JSScript*> script, Handle<JSObject*> jsmEnv,
    Handle<StackGCVector<JSObject*>> targetObj);

// Used by native methods to determine the JSMEnvironment of caller if possible
// by looking at stack frames. Returns nullptr if top frame isn't a scripted
// caller in a JSM.
//
// NOTE: This may find NonSyntacticVariablesObject generated by other embedding
// such as a Gecko FrameScript. Caller can check the compartment if needed.
extern JS_PUBLIC_API JSObject* GetJSMEnvironmentOfScriptedCaller(JSContext* cx);

/**
 * Determine if obj is a JSMEnvironment
 *
 * NOTE: This may return true for an NonSyntacticVariablesObject generated by
 * other embedding such as a Gecko FrameScript. Caller can check compartment.
 */
extern JS_PUBLIC_API bool IsJSMEnvironment(JSObject* obj);

}  // namespace JS

#endif  // js_friend_JSMEnvironment_h
