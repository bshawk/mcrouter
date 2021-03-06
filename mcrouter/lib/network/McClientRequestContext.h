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

#include <typeindex>

#include <folly/IntrusiveList.h>
#include <folly/experimental/fibers/Baton.h>

#include "mcrouter/lib/McOperation.h"
#include "mcrouter/lib/network/FBTrace.h"
#include "mcrouter/lib/network/ClientMcParser.h"
#include "mcrouter/lib/network/McSerializedRequest.h"

namespace facebook { namespace memcache {

class AsyncMcClientImpl;
class McClientRequestContextQueue;

/**
 * Class for storing per request data that is required for proper requests
 * processing inside of AsyncMcClient.
 */
class McClientRequestContextBase {
 public:
  using InitializerFuncPtr =
    void (*)(ClientMcParser<AsyncMcClientImpl>&);

  McSerializedRequest reqContext;
  uint64_t id;

  McClientRequestContextBase(const McClientRequestContextBase&) = delete;
  McClientRequestContextBase& operator=(const McClientRequestContextBase& other)
    = delete;
  McClientRequestContextBase(McClientRequestContextBase&&) = delete;
  McClientRequestContextBase& operator=(McClientRequestContextBase&& other)
    = delete;

  /**
   * Returns fake data (specific to this request and operation) that can be used
   * to simulate a reply from network
   */
  virtual const char* fakeReply() const = 0;

  /**
   * Propagate an error to the user.
   *
   * Should be called only when the request is not in a queue.
   */
  virtual void replyError(mc_res_t result) = 0;

 protected:
  enum class ReqState {
    NONE,
    PENDING_QUEUE,
    WRITE_QUEUE,
    WRITE_QUEUE_CANCELED,
    PENDING_REPLY_QUEUE,
    COMPLETE,
  };

  virtual ~McClientRequestContextBase();

  template <class Operation, class Request>
  McClientRequestContextBase(
    Operation, const Request& request, uint64_t reqid, mc_protocol_t protocol,
    std::shared_ptr<AsyncMcClientImpl> client,
    folly::Optional<typename ReplyType<Operation,Request>::type>& replyStorage,
    McClientRequestContextQueue& queue, InitializerFuncPtr initializer);


  virtual void sendTraceOnReply() = 0;

  folly::fibers::Baton baton_;
  McClientRequestContextQueue& queue_;
  ReqState state_{ReqState::NONE};

 private:
  friend class McClientRequestContextQueue;

  std::shared_ptr<AsyncMcClientImpl> client_;
  std::type_index replyType_;
  folly::SafeIntrusiveListHook hook_;
  void* replyStorage_;
  InitializerFuncPtr initializer_;

  void cancelAndWait();

  /**
   * Notify context that request was canceled in AsyncMcClientImpl
   */
  void canceled();

  /**
   * Entry point for propagating reply to the user.
   *
   * Typechecks the reply and propagates it to the proper subclass.
   *
   * @return false iff the reply type didn't match with expected.
   */
  template <class Reply>
  bool reply(Reply&& r);

 public:
  using Queue = folly::CountedIntrusiveList<McClientRequestContextBase,
                                            &McClientRequestContextBase::hook_>;
};

template <class Operation, class Request>
class McClientRequestContext : public McClientRequestContextBase {
 public:
  using Reply = typename ReplyType<Operation, Request>::type;

  McClientRequestContext(
    const Request& request, uint64_t reqid, mc_protocol_t protocol,
    std::shared_ptr<AsyncMcClientImpl> client,
    McClientRequestContextQueue& queue,
    McClientRequestContextBase::InitializerFuncPtr);

  void replyError(mc_res_t result) override;
  const char* fakeReply() const override;

  Reply waitForReply(std::chrono::milliseconds timeout);
 private:
  folly::Optional<Reply> replyStorage_;

#ifndef LIBMC_FBTRACE_DISABLE
  const mc_fbtrace_info_s* fbtraceInfo_;
#endif
  void sendTraceOnReply() override;
};

class McClientRequestContextQueue {
 public:
  explicit McClientRequestContextQueue(bool outOfOrder) noexcept;

  McClientRequestContextQueue(const McClientRequestContextQueue&) = delete;
  McClientRequestContextQueue& operator=(
    const McClientRequestContextQueue& other) = delete;
  McClientRequestContextQueue(McClientRequestContextQueue&&) = delete;
  McClientRequestContextQueue& operator=(
    McClientRequestContextQueue&& other) = delete;

  size_t getPendingRequestCount() const noexcept;
  size_t getInflightRequestCount() const noexcept;

  /**
   * Fails all requests that were already sent (i.e. pending reply) with a given
   * error code.
   */
  void failAllSent(mc_res_t error);

  /**
   * Fails all requests that were not sent yet (i.e. pending) with a given error
   * code.
   */
  void failAllPending(mc_res_t error);

  /**
   * Return an id of the first pending request.
   *
   * Note: it's the caller responsibility to ensure that there's at least one
   *       pending request.
   */
  size_t getFirstId() const;

  /**
   * Adds request into pending queue.
   */
  void markAsPending(McClientRequestContextBase& req);

  /**
   * Moves the first request from pending queue into sending queue.
   *
   * @return a reference to the request that was marked as sending.
   */
  McClientRequestContextBase& markNextAsSending();

  /**
   * Marks the first request from sending queue as sent.
   *
   * May result in one of the following:
   *   - if request was cancelled before, request will be removed from queue
   *   - otherwise, request will be moved into pending reply queue.
   *
   * @return a reference to the request that was marked as sent.
   */
  McClientRequestContextBase& markNextAsSent();

  /**
   * Reply request with given id with the provided reply.
   * In case of in order protocol the id is ignored.
   *
   * Does nothing if the request was already removed from the queue.
   */
  template <class Reply>
  void reply(uint64_t id, Reply&& reply);

  /**
   * Obtain a function that should be used to initialize parser for given
   * request.
   *
   * @param reqId  id of request to lookup, ignored for in order protocol.
   *
   * Note: for out of order may return nullptr in case a request with given id
   *       was cancelled.
   */
  McClientRequestContextBase::InitializerFuncPtr
  getParserInitializer(uint64_t reqId = 0);

 private:
  // Friend to allow access to remove* mothods.
  template<class Operation, class Request>
  friend class McClientRequestContext;

  using State = McClientRequestContextBase::ReqState;

  bool outOfOrder_{false};
  // Queue of requests, that are queued to be sent.
  McClientRequestContextBase::Queue pendingQueue_;
  // Queue of requests, that are currently being written to the socket.
  McClientRequestContextBase::Queue writeQueue_;
  // Queue of requests, that are already sent and are waiting for replies.
  McClientRequestContextBase::Queue pendingReplyQueue_;
  // Id to request map. Used only in case of out-of-order protocol for fast
  // request lookup.
  std::unordered_map<uint64_t, McClientRequestContextBase*> idMap_;

  // Storage for parser initializers for timed out requests.
  std::queue<McClientRequestContextBase::InitializerFuncPtr>
  timedOutInitializers_;

  void failQueue(McClientRequestContextBase::Queue& queue, mc_res_t error);

  void removeFromMap(uint64_t id);

  /**
   * Removes given request from pending queue and id map.
   */
  void removePending(McClientRequestContextBase& req);

  /**
   * Removes given request from pending reply queue and from id map.
   *
   * Calling this method indicates that this request wasn't replied, but
   * we should expect a reply from network.
   */
  void removePendingReply(McClientRequestContextBase& req);

  /**
   * Should be called whenever the network communication channel gets closed.
   */
  void clearStoredInitializers();
};

}}  // facebook::memcache

#include "McClientRequestContext-inl.h"
