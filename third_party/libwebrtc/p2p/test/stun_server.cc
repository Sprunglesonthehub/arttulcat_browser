/*
 *  Copyright 2004 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "p2p/test/stun_server.h"

#include <memory>
#include <string>
#include <utility>

#include "absl/strings/string_view.h"
#include "api/sequence_checker.h"
#include "api/transport/stun.h"
#include "rtc_base/async_packet_socket.h"
#include "rtc_base/async_udp_socket.h"
#include "rtc_base/byte_buffer.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/network/received_packet.h"
#include "rtc_base/socket_address.h"

namespace webrtc {

StunServer::StunServer(AsyncUDPSocket* socket) : socket_(socket) {
  socket_->RegisterReceivedPacketCallback(
      [&](rtc::AsyncPacketSocket* socket, const rtc::ReceivedPacket& packet) {
        OnPacket(socket, packet);
      });
}

StunServer::~StunServer() {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  socket_->DeregisterReceivedPacketCallback();
}

void StunServer::OnPacket(AsyncPacketSocket* socket,
                          const rtc::ReceivedPacket& packet) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  // Parse the STUN message; eat any messages that fail to parse.
  rtc::ByteBufferReader bbuf(packet.payload());
  cricket::StunMessage msg;
  if (!msg.Read(&bbuf)) {
    return;
  }

  // TODO(?): If unknown non-optional (<= 0x7fff) attributes are found,
  // send a
  //          420 "Unknown Attribute" response.

  // Send the message to the appropriate handler function.
  switch (msg.type()) {
    case cricket::STUN_BINDING_REQUEST:
      OnBindingRequest(&msg, packet.source_address());
      break;

    default:
      SendErrorResponse(msg, packet.source_address(), 600,
                        "Operation Not Supported");
  }
}

void StunServer::OnBindingRequest(cricket::StunMessage* msg,
                                  const SocketAddress& remote_addr) {
  cricket::StunMessage response(cricket::STUN_BINDING_RESPONSE,
                                msg->transaction_id());
  GetStunBindResponse(msg, remote_addr, &response);
  SendResponse(response, remote_addr);
}

void StunServer::SendErrorResponse(const cricket::StunMessage& msg,
                                   const SocketAddress& addr,
                                   int error_code,
                                   absl::string_view error_desc) {
  cricket::StunMessage err_msg(cricket::GetStunErrorResponseType(msg.type()),
                               msg.transaction_id());

  auto err_code = cricket::StunAttribute::CreateErrorCode();
  err_code->SetCode(error_code);
  err_code->SetReason(std::string(error_desc));
  err_msg.AddAttribute(std::move(err_code));

  SendResponse(err_msg, addr);
}

void StunServer::SendResponse(const cricket::StunMessage& msg,
                              const SocketAddress& addr) {
  rtc::ByteBufferWriter buf;
  msg.Write(&buf);
  rtc::PacketOptions options;
  if (socket_->SendTo(buf.Data(), buf.Length(), addr, options) < 0)
    RTC_LOG_ERR(LS_ERROR) << "sendto";
}

void StunServer::GetStunBindResponse(cricket::StunMessage* message,
                                     const SocketAddress& remote_addr,
                                     cricket::StunMessage* response) const {
  RTC_DCHECK_EQ(response->type(), cricket::STUN_BINDING_RESPONSE);
  RTC_DCHECK_EQ(response->transaction_id(), message->transaction_id());

  // Tell the user the address that we received their message from.
  std::unique_ptr<cricket::StunAddressAttribute> mapped_addr;
  if (message->IsLegacy()) {
    mapped_addr = cricket::StunAttribute::CreateAddress(
        cricket::STUN_ATTR_MAPPED_ADDRESS);
  } else {
    mapped_addr = cricket::StunAttribute::CreateXorAddress(
        cricket::STUN_ATTR_XOR_MAPPED_ADDRESS);
  }
  mapped_addr->SetAddress(remote_addr);
  response->AddAttribute(std::move(mapped_addr));
}

}  // namespace webrtc
