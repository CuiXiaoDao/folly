/*
 * Copyright 2016-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/io/async/Request.h>
#include <folly/tracing/StaticTracepoint.h>

#include <glog/logging.h>

#include <folly/MapUtil.h>
#include <folly/SingletonThreadLocal.h>

namespace folly {

void RequestData::DestructPtr::operator()(RequestData* ptr) {
  if (ptr) {
    auto keepAliveCounter =
        ptr->keepAliveCounter_.fetch_sub(1, std::memory_order_acq_rel);
    // Note: this is the value before decrement, hence == 1 check
    DCHECK(keepAliveCounter > 0);
    if (keepAliveCounter == 1) {
      delete ptr;
    }
  }
}

/* static */ RequestData::SharedPtr RequestData::constructPtr(
    RequestData* ptr) {
  if (ptr) {
    auto keepAliveCounter =
        ptr->keepAliveCounter_.fetch_add(1, std::memory_order_relaxed);
    DCHECK(keepAliveCounter >= 0);
  }
  return SharedPtr(ptr);
}

bool RequestContext::doSetContextData(
    const std::string& val,
    std::unique_ptr<RequestData>& data,
    DoSetBehaviour behaviour) {
  auto ulock = state_.ulock();

  bool conflict = false;
  auto it = ulock->requestData_.find(val);
  if (it != ulock->requestData_.end()) {
    if (behaviour == DoSetBehaviour::SET_IF_ABSENT) {
      return false;
    } else if (behaviour == DoSetBehaviour::SET) {
      LOG_FIRST_N(WARNING, 1) << "Calling RequestContext::setContextData for "
                              << val << " but it is already set";
    }
    conflict = true;
  }

  auto wlock = ulock.moveFromUpgradeToWrite();
  if (conflict) {
    if (it->second) {
      if (it->second->hasCallback()) {
        it->second->onUnset();
        wlock->callbackData_.erase(it->second.get());
      }
      it->second.reset(nullptr);
    }
    if (behaviour == DoSetBehaviour::SET) {
      return true;
    }
  }

  if (data && data->hasCallback()) {
    wlock->callbackData_.insert(data.get());
    data->onSet();
  }
  wlock->requestData_[val] = RequestData::constructPtr(data.release());

  return true;
}

void RequestContext::setContextData(
    const std::string& val,
    std::unique_ptr<RequestData> data) {
  doSetContextData(val, data, DoSetBehaviour::SET);
}

bool RequestContext::setContextDataIfAbsent(
    const std::string& val,
    std::unique_ptr<RequestData> data) {
  return doSetContextData(val, data, DoSetBehaviour::SET_IF_ABSENT);
}

void RequestContext::overwriteContextData(
    const std::string& val,
    std::unique_ptr<RequestData> data) {
  doSetContextData(val, data, DoSetBehaviour::OVERWRITE);
}

bool RequestContext::hasContextData(const std::string& val) const {
  return state_.rlock()->requestData_.count(val);
}

RequestData* RequestContext::getContextData(const std::string& val) {
  const RequestData::SharedPtr dflt{nullptr};
  return get_ref_default(state_.rlock()->requestData_, val, dflt).get();
}

const RequestData* RequestContext::getContextData(
    const std::string& val) const {
  const RequestData::SharedPtr dflt{nullptr};
  return get_ref_default(state_.rlock()->requestData_, val, dflt).get();
}

void RequestContext::onSet() {
  auto rlock = state_.rlock();
  for (const auto& data : rlock->callbackData_) {
    data->onSet();
  }
}

void RequestContext::onUnset() {
  auto rlock = state_.rlock();
  for (const auto& data : rlock->callbackData_) {
    data->onUnset();
  }
}

void RequestContext::clearContextData(const std::string& val) {
  RequestData::SharedPtr requestData;
  // Delete the RequestData after giving up the wlock just in case one of the
  // RequestData destructors will try to grab the lock again.
  {
    auto ulock = state_.ulock();
    auto it = ulock->requestData_.find(val);
    if (it == ulock->requestData_.end()) {
      return;
    }

    auto wlock = ulock.moveFromUpgradeToWrite();
    if (it->second && it->second->hasCallback()) {
      it->second->onUnset();
      wlock->callbackData_.erase(it->second.get());
    }

    requestData = std::move(it->second);
    wlock->requestData_.erase(it);
  }
}

namespace {
// Execute functor exec for all RequestData in data, which are not in other
// Similar to std::set_difference but avoid intermediate data structure
template <typename TExec>
void exec_set_difference(
    const std::set<RequestData*>& data,
    const std::set<RequestData*>& other,
    TExec&& exec) {
  auto diter = data.begin();
  auto dend = data.end();
  auto oiter = other.begin();
  auto oend = other.end();
  while (diter != dend) {
    if (oiter == oend || *diter < *oiter) {
      exec(*diter);
      ++diter;
    } else if (*oiter < *diter) {
      ++oiter;
    } else {
      ++diter;
      ++oiter;
    }
  }
}
} // namespace

std::shared_ptr<RequestContext> RequestContext::setContext(
    std::shared_ptr<RequestContext> newCtx) {
  auto& staticCtx = getStaticContext();
  if (newCtx == staticCtx) {
    return newCtx;
  }

  FOLLY_SDT(
      folly, request_context_switch_before, staticCtx.get(), newCtx.get());

  auto curCtx = staticCtx;
  if (newCtx && curCtx) {
    // Only call set/unset for all request data that differs
    auto newLock = newCtx->state_.rlock();
    auto curLock = curCtx->state_.rlock();
    auto& newData = newLock->callbackData_;
    auto& curData = curLock->callbackData_;
    exec_set_difference(
        curData, newData, [](RequestData* data) { data->onUnset(); });
    staticCtx = newCtx;
    exec_set_difference(
        newData, curData, [](RequestData* data) { data->onSet(); });
  } else {
    if (curCtx) {
      curCtx->onUnset();
    }
    staticCtx = newCtx;
    if (newCtx) {
      newCtx->onSet();
    }
  }
  return curCtx;
}

std::shared_ptr<RequestContext>& RequestContext::getStaticContext() {
  using SingletonT = SingletonThreadLocal<std::shared_ptr<RequestContext>>;
  return SingletonT::get();
}

/* static */ std::shared_ptr<RequestContext>
RequestContext::setShallowCopyContext() {
  auto& parent = getStaticContext();
  auto child = std::make_shared<RequestContext>();

  if (parent) {
    auto parentLock = parent->state_.rlock();
    auto childLock = child->state_.wlock();
    childLock->callbackData_ = parentLock->callbackData_;
    for (const auto& entry : parentLock->requestData_) {
      childLock->requestData_[entry.first] =
          RequestData::constructPtr(entry.second.get());
    }
  }

  // Do not use setContext to avoid global set/unset
  std::swap(child, parent);
  return child;
}

RequestContext* RequestContext::get() {
  auto& context = getStaticContext();
  if (!context) {
    static RequestContext defaultContext;
    return std::addressof(defaultContext);
  }
  return context.get();
}
} // namespace folly
