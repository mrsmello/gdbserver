// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <array>
#include <memory>
#include <queue>

#include "lib/ftl/files/unique_fd.h"
#include "lib/ftl/macros.h"
#include "lib/ftl/strings/string_view.h"

#include "inferior-control/exception-port.h"
#include "inferior-control/process.h"
#include "inferior-control/server.h"
#include "inferior-control/thread.h"

#include "cmd-handler.h"
#include "io-loop.h"

namespace debugserver {

// Server for Remote Serial Protocol support.
// This implements the main loop and handles commands received over a TCP port
// (from gdb or lldb, or any other debugger that supports RSP really).
//
// NOTE: This class is generally not thread safe. Care must be taken when
// calling methods such as set_current_process(), SetCurrentThread(), and
// QueueNotification() which modify its internal state.
class RspServer final : public Server {
 public:
  // The default timeout interval used when sending notifications.
  constexpr static int64_t kDefaultTimeoutSeconds = 30;

  explicit RspServer(uint16_t port);

  // Starts the main loop. This will first block and wait for an incoming
  // connection. Once there is a connection, this will start an event loop for
  // handling commands.
  bool Run() override;

  // Queue a notification packet and send it out if there are no currently
  // queued notifications. The GDB Remote Protocol defines a specific
  // control-flow for notification packets, such that each notification packet
  // will be pending until the remote end acknowledges it. There can be only
  // one pending notification at a time.
  //
  // A notification will time out if the remote end does not acknowledge it
  // within |timeout|. If a notification times out, it will be sent again.
  void QueueNotification(
      const ftl::StringView& name,
      const ftl::StringView& event,
      const ftl::TimeDelta& timeout =
          ftl::TimeDelta::FromSeconds(kDefaultTimeoutSeconds));

  // Wrapper of QueueNotification for "Stop" notifications.
  void QueueStopNotification(
      const ftl::StringView& event,
      const ftl::TimeDelta& timeout =
          ftl::TimeDelta::FromSeconds(kDefaultTimeoutSeconds));

  // Set |parameter| to |value|. Return true if success.
  bool SetParameter(const ftl::StringView& parameter,
                    const ftl::StringView& value);

  // Store the value of |parameter| in |*value|. Return true if success.
  bool GetParameter(const ftl::StringView& parameter,
                    std::string* value);

 private:
  // Maximum number of characters in the outbound buffer.
  constexpr static size_t kMaxBufferSize = 4096;

  // Represents a pending notification packet.
  struct PendingNotification {
    PendingNotification(const ftl::StringView& name,
                        const ftl::StringView& event,
                        const ftl::TimeDelta& timeout);

    std::string name;
    std::string event;
    ftl::TimeDelta timeout;
  };

  RspServer() = default;

  // Listens for incoming connections on port |port_|. Once a connection is
  // accepted, returns true and stores the client socket in |client_sock_|,
  // and the server socket in |server_sock_|. Returns false if an error occurs.
  bool Listen();

  // Send an acknowledgment packet. If |ack| is true, then a '+' ACK will be
  // sent to indicate that a packet was received correctly, or '-' to request
  // retransmission. This method blocks until the syscall to write to the socket
  // returns.
  void SendAck(bool ack);

  // Posts an asynchronous task on the message loop to send a packet over the
  // wire. |data| will be wrapped in a GDB Remote Protocol packet after
  // computing the checksum. If |notify| is true, then a notification packet
  // will be sent (where the first byte of the packet equals '%'), otherwise a
  // regular packet will be sent (first byte is '$').
  void PostWriteTask(bool notify, const ftl::StringView& data);

  // Convenience helpers for PostWriteTask
  void PostPacketWriteTask(const ftl::StringView& data);
  void PostPendingNotificationWriteTask();

  // If |pending_notification_| is NULL, this pops the next lined-up
  // notification from |notify_queue_| and assigns it as the new pending
  // notification and sends it to the remote device.
  //
  // Returns true, if the next notification was posted. Returns false if the
  // next notification was not posted because either there is still a pending
  // unacknowledged notification or the notification queue is empty.
  bool TryPostNextNotification();

  // Post a timeout handler for |pending_notification_|.
  void PostNotificationTimeoutHandler();

  // IOLoop::Delegate overrides.
  void OnBytesRead(const ftl::StringView& bytes) override;
  void OnDisconnected() override;
  void OnIOError() override;

  // Process::Delegate overrides.
  void OnThreadStarting(Process* process,
                       Thread* thread,
                       const mx_exception_context_t& context) override;
  void OnThreadExiting(Process* process,
                       Thread* thread,
                       const mx_excp_type_t type,
                       const mx_exception_context_t& context) override;
  void OnProcessExit(Process* process,
                     const mx_excp_type_t type,
                     const mx_exception_context_t& context) override;
  void OnArchitecturalException(Process* process,
                                Thread* thread,
                                const mx_excp_type_t type,
                                const mx_exception_context_t& context) override;

  // TCP port number that we will listen on.
  uint16_t port_;

  // File descriptor for the socket used for listening for incoming
  // connections (e.g. from gdb or lldb).
  ftl::UniqueFD server_sock_;

  // Buffer used for writing outgoing bytes.
  std::array<char, kMaxBufferSize> out_buffer_;

  // The CommandHandler that is responsible for interpreting received command
  // packets and routing them to the correct handler.
  CommandHandler command_handler_;

  // The current queue of notifications that have not been sent out yet.
  std::queue<std::unique_ptr<PendingNotification>> notify_queue_;

  // The currently pending notification that has been sent out but has NOT been
  // acknowledged by the remote end yet.
  std::unique_ptr<PendingNotification> pending_notification_;

  FTL_DISALLOW_COPY_AND_ASSIGN(RspServer);
};

}  // namespace debugserver
