// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HEADLESS_LIB_BROWSER_CAPTURE_VIRTUAL_TIME_OWNER_H_
#define HEADLESS_LIB_BROWSER_CAPTURE_VIRTUAL_TIME_OWNER_H_

#include <map>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"

namespace content {
class WebContents;
}

namespace headless::capture {

// The engine's own DevTools client on the capturing target. It is the only
// client allowed to call Emulation.setVirtualTimePolicy once a capture is
// configured (DESIGN.md 4.2), because content's EmulationHandler has no
// virtual-time code and the command must reach the renderer's
// InspectorEmulationAgent through a session.
class VirtualTimeOwner : public content::DevToolsAgentHostClient {
 public:
  using ResultCallback =
      base::OnceCallback<void(std::optional<std::string> error,
                              base::DictValue result)>;
  using GrantCallback = base::OnceCallback<void(std::optional<std::string> error)>;

  explicit VirtualTimeOwner(content::WebContents* web_contents);
  VirtualTimeOwner(const VirtualTimeOwner&) = delete;
  VirtualTimeOwner& operator=(const VirtualTimeOwner&) = delete;
  ~VirtualTimeOwner() override;

  // Attaches and sends setVirtualTimePolicy(pause, initialVirtualTime).
  // The result carries `virtualTimeTicksBase` (milliseconds).
  void Enable(double initial_virtual_time_seconds, ResultCallback callback);

  // Grants `budget_ms` with pauseIfNetworkFetchesPending and no starvation
  // bypass; `callback` runs when virtualTimeBudgetExpired arrives.
  void Grant(double budget_ms, GrantCallback callback);

  bool attached() const { return attached_; }

  // content::DevToolsAgentHostClient:
  void DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* agent_host) override;
  bool IsTrusted() override;
  bool AllowUnsafeOperations() override;
  std::string GetTypeForMetrics() override;

 private:
  void Send(const std::string& method,
            base::DictValue params,
            ResultCallback callback);

  scoped_refptr<content::DevToolsAgentHost> agent_host_;
  bool attached_ = false;
  int next_id_ = 1;
  std::map<int, ResultCallback> pending_;
  GrantCallback grant_callback_;
  base::WeakPtrFactory<VirtualTimeOwner> weak_factory_{this};
};

}  // namespace headless::capture

#endif  // HEADLESS_LIB_BROWSER_CAPTURE_VIRTUAL_TIME_OWNER_H_
