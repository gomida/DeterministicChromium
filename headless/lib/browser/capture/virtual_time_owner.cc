// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "headless/lib/browser/capture/virtual_time_owner.h"

#include <utility>

#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "content/public/browser/web_contents.h"

namespace headless::capture {

VirtualTimeOwner::VirtualTimeOwner(content::WebContents* web_contents)
    : agent_host_(content::DevToolsAgentHost::GetOrCreateFor(web_contents)) {}

VirtualTimeOwner::~VirtualTimeOwner() {
  if (attached_ && agent_host_) {
    agent_host_->DetachClient(this);
  }
}

void VirtualTimeOwner::Enable(double initial_virtual_time_seconds,
                              ResultCallback callback) {
  if (!attached_) {
    attached_ = agent_host_->AttachClient(this);
    if (!attached_) {
      std::move(callback).Run("VirtualTimeOwned: could not attach the engine's DevTools client",
                              base::DictValue());
      return;
    }
  }
  base::DictValue params;
  params.Set("policy", "pause");
  params.Set("initialVirtualTime", initial_virtual_time_seconds);
  Send("Emulation.setVirtualTimePolicy", std::move(params), std::move(callback));
}

void VirtualTimeOwner::Grant(double budget_ms, GrantCallback callback) {
  if (grant_callback_) {
    std::move(callback).Run("VirtualTimeStall: a grant is already pending");
    return;
  }
  grant_callback_ = std::move(callback);
  base::DictValue params;
  params.Set("policy", "pauseIfNetworkFetchesPending");
  params.Set("budget", budget_ms);
  params.Set("maxVirtualTimeTaskStarvationCount", 0);
  Send("Emulation.setVirtualTimePolicy", std::move(params),
       base::BindOnce(
           [](base::WeakPtr<VirtualTimeOwner> self,
              std::optional<std::string> error, base::DictValue) {
             if (self && error && self->grant_callback_) {
               std::move(self->grant_callback_).Run(error);
             }
           },
           weak_factory_.GetWeakPtr()));
}

void VirtualTimeOwner::Send(const std::string& method,
                            base::DictValue params,
                            ResultCallback callback) {
  const int id = next_id_++;
  base::DictValue message;
  message.Set("id", id);
  message.Set("method", method);
  message.Set("params", std::move(params));
  std::string json;
  base::JSONWriter::Write(message, &json);
  pending_[id] = std::move(callback);
  agent_host_->DispatchProtocolMessage(this, base::as_byte_span(json));
}

void VirtualTimeOwner::DispatchProtocolMessage(
    content::DevToolsAgentHost* agent_host,
    base::span<const uint8_t> message) {
  std::optional<base::Value> parsed = base::JSONReader::Read(
      std::string_view(reinterpret_cast<const char*>(message.data()),
                       message.size()),
      base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return;
  }
  base::DictValue& dict = parsed->GetDict();
  if (std::optional<int> id = dict.FindInt("id")) {
    auto it = pending_.find(*id);
    if (it == pending_.end()) {
      return;
    }
    ResultCallback callback = std::move(it->second);
    pending_.erase(it);
    if (base::DictValue* error = dict.FindDict("error")) {
      const std::string* text = error->FindString("message");
      std::move(callback).Run(text ? *text : "protocol error", base::DictValue());
      return;
    }
    base::DictValue* result = dict.FindDict("result");
    std::move(callback).Run(std::nullopt,
                            result ? std::move(*result) : base::DictValue());
    return;
  }
  const std::string* method = dict.FindString("method");
  if (method && *method == "Emulation.virtualTimeBudgetExpired" &&
      grant_callback_) {
    std::move(grant_callback_).Run(std::nullopt);
  }
}

void VirtualTimeOwner::AgentHostClosed(content::DevToolsAgentHost* agent_host) {
  attached_ = false;
  if (grant_callback_) {
    std::move(grant_callback_).Run("TargetDetached: agent host closed");
  }
  for (auto& [id, callback] : pending_) {
    std::move(callback).Run("TargetDetached: agent host closed", base::DictValue());
  }
  pending_.clear();
}

bool VirtualTimeOwner::IsTrusted() {
  return true;
}

bool VirtualTimeOwner::AllowUnsafeOperations() {
  return true;
}

std::string VirtualTimeOwner::GetTypeForMetrics() {
  return "Other";
}

}  // namespace headless::capture
