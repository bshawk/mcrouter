/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mcrouter/lib/Operation.h"
#include "mcrouter/lib/Reply.h"

namespace facebook { namespace memcache {

/**
 * Returns the default reply for each request right away
 */
template <class RouteHandleIf>
struct NullRoute {
  using ContextPtr = typename RouteHandleIf::ContextPtr;

  static std::string routeName() {
    return "null";
  }

  template <class Operation, class Request>
  static std::vector<std::shared_ptr<RouteHandleIf>> couldRouteTo(
    const Request& req, Operation, const ContextPtr& ctx) {

    return {};
  }

  template <class Operation, class Request>
  static typename ReplyType<Operation, Request>::type route(
    const Request& req, Operation, const ContextPtr& ctx) {

    typedef typename ReplyType<Operation, Request>::type Reply;

    return Reply(DefaultReply, Operation());
  }
};

}}
