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

#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {
class Atomic64Metric;

template <>
struct BSONObjAppendFormat<Atomic64Metric> : FormatKind<NumberLong> {};

class ServerStatusMetric {
public:
    /**
     * @param name is a dotted path of a counter name
     *             if name starts with . its treated as a path from the serverStatus root
     *             otherwise it will live under the "counters" namespace
     *             so foo.bar would be serverStatus().counters.foo.bar
     */
    ServerStatusMetric(const std::string& name);
    virtual ~ServerStatusMetric() {}

    std::string getMetricName() const {
        return _name;
    }

    virtual void appendAtLeaf(BSONObjBuilder& b) const = 0;

protected:
    static std::string _parseLeafName(const std::string& name);

    const std::string _name;
    const std::string _leafName;
};

/**
 * usage
 *
 * declared once
 *    Counter counter;
 *    ServerStatusMetricField myAwesomeCounterDisplay( "path.to.counter", &counter );
 *
 * call
 *    counter.hit();
 *
 * will show up in db.serverStatus().metrics.path.to.counter
 */
template <typename T>
class ServerStatusMetricField : public ServerStatusMetric {
public:
    ServerStatusMetricField(const std::string& name, const T* t)
        : ServerStatusMetric(name), _t(t) {}

    virtual void appendAtLeaf(BSONObjBuilder& b) const {
        b.append(_leafName, *_t);
    }

private:
    const T* _t;
};

/**
 * Atomic wrapper for long long type for Metrics.  This is for values which are set rather than
 * just incremented or decremented; if you want a counter, use Counter64.
 */
class Atomic64Metric {
public:
    /** Set _value to the max of the current or newMax. */
    void setIfMax(long long newMax) {
        /*  Note: compareAndSwap will load into val most recent value. */
        for (long long val = _value.load(); val < newMax && !_value.compareAndSwap(&val, newMax);) {
        }
    }

    /** store val into value. */
    void set(long long val) {
        _value.storeRelaxed(val);
    }

    /** Return the current value. */
    long long get() const {
        return _value.loadRelaxed();
    }

    /** TODO: SERVER-73806 Avoid implicit conversion to long long */
    operator long long() const {
        return get();
    }

private:
    mongo::AtomicWord<long long> _value;
};
}  // namespace mongo
