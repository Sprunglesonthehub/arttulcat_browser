/*
 *  Copyright 2004 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef P2P_BASE_STUN_PORT_H_
#define P2P_BASE_STUN_PORT_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "absl/memory/memory.h"
#include "absl/strings/string_view.h"
#include "api/async_dns_resolver.h"
#include "api/candidate.h"
#include "api/field_trials_view.h"
#include "api/packet_socket_factory.h"
#include "api/task_queue/task_queue_base.h"
#include "p2p/base/connection.h"
#include "p2p/base/port.h"
#include "p2p/base/port_interface.h"
#include "p2p/base/stun_request.h"
#include "rtc_base/async_packet_socket.h"
#include "rtc_base/dscp.h"
#include "rtc_base/network.h"
#include "rtc_base/network/received_packet.h"
#include "rtc_base/network/sent_packet.h"
#include "rtc_base/network_constants.h"
#include "rtc_base/socket.h"
#include "rtc_base/socket_address.h"
#include "rtc_base/system/rtc_export.h"

namespace cricket {

// Lifetime chosen for STUN ports on low-cost networks.
static const int INFINITE_LIFETIME = -1;
// Lifetime for STUN ports on high-cost networks: 2 minutes
static const int HIGH_COST_PORT_KEEPALIVE_LIFETIME = 2 * 60 * 1000;

// Communicates using the address on the outside of a NAT.
class RTC_EXPORT UDPPort : public Port {
 public:
  static std::unique_ptr<UDPPort> Create(
      const PortParametersRef& args,
      webrtc::AsyncPacketSocket* socket,
      bool emit_local_for_anyaddress,
      std::optional<int> stun_keepalive_interval) {
    // Using `new` to access a non-public constructor.
    auto port =
        absl::WrapUnique(new UDPPort(args, webrtc::IceCandidateType::kHost,
                                     socket, emit_local_for_anyaddress));
    port->set_stun_keepalive_delay(stun_keepalive_interval);
    if (!port->Init()) {
      return nullptr;
    }
    return port;
  }
  [[deprecated("Pass arguments using PortParametersRef")]] static std::
      unique_ptr<UDPPort>
      Create(webrtc::TaskQueueBase* thread,
             webrtc::PacketSocketFactory* factory,
             const rtc::Network* network,
             webrtc::AsyncPacketSocket* socket,
             absl::string_view username,
             absl::string_view password,
             bool emit_local_for_anyaddress,
             std::optional<int> stun_keepalive_interval,
             const webrtc::FieldTrialsView* field_trials = nullptr) {
    return Create({.network_thread = thread,
                   .socket_factory = factory,
                   .network = network,
                   .ice_username_fragment = username,
                   .ice_password = password,
                   .field_trials = field_trials},
                  socket, emit_local_for_anyaddress, stun_keepalive_interval);
  }

  static std::unique_ptr<UDPPort> Create(
      const PortParametersRef& args,
      uint16_t min_port,
      uint16_t max_port,
      bool emit_local_for_anyaddress,
      std::optional<int> stun_keepalive_interval) {
    // Using `new` to access a non-public constructor.
    auto port = absl::WrapUnique(
        new UDPPort(args, webrtc::IceCandidateType::kHost, min_port, max_port,
                    emit_local_for_anyaddress));
    port->set_stun_keepalive_delay(stun_keepalive_interval);
    if (!port->Init()) {
      return nullptr;
    }
    return port;
  }
  [[deprecated("Pass arguments using PortParametersRef")]] static std::
      unique_ptr<UDPPort>
      Create(webrtc::TaskQueueBase* thread,
             webrtc::PacketSocketFactory* factory,
             const rtc::Network* network,
             uint16_t min_port,
             uint16_t max_port,
             absl::string_view username,
             absl::string_view password,
             bool emit_local_for_anyaddress,
             std::optional<int> stun_keepalive_interval,
             const webrtc::FieldTrialsView* field_trials = nullptr) {
    return Create({.network_thread = thread,
                   .socket_factory = factory,
                   .network = network,
                   .ice_username_fragment = username,
                   .ice_password = password,
                   .field_trials = field_trials},
                  min_port, max_port, emit_local_for_anyaddress,
                  stun_keepalive_interval);
  }

  ~UDPPort() override;

  webrtc::SocketAddress GetLocalAddress() const {
    return socket_->GetLocalAddress();
  }

  const ServerAddresses& server_addresses() const { return server_addresses_; }
  void set_server_addresses(const ServerAddresses& addresses) {
    server_addresses_ = addresses;
  }

  void PrepareAddress() override;

  Connection* CreateConnection(const webrtc::Candidate& address,
                               CandidateOrigin origin) override;
  int SetOption(webrtc::Socket::Option opt, int value) override;
  int GetOption(webrtc::Socket::Option opt, int* value) override;
  int GetError() override;

  bool HandleIncomingPacket(webrtc::AsyncPacketSocket* socket,
                            const rtc::ReceivedPacket& packet) override;

  bool SupportsProtocol(absl::string_view protocol) const override;
  webrtc::ProtocolType GetProtocol() const override;

  void GetStunStats(std::optional<StunStats>* stats) override;

  void set_stun_keepalive_delay(const std::optional<int>& delay);
  int stun_keepalive_delay() const { return stun_keepalive_delay_; }

  // Visible for testing.
  int stun_keepalive_lifetime() const { return stun_keepalive_lifetime_; }
  void set_stun_keepalive_lifetime(int lifetime) {
    stun_keepalive_lifetime_ = lifetime;
  }

  StunRequestManager& request_manager() { return request_manager_; }

 protected:
  UDPPort(const PortParametersRef& args,
          webrtc::IceCandidateType type,
          webrtc::AsyncPacketSocket* socket,
          bool emit_local_for_anyaddress);
  UDPPort(const PortParametersRef& args,
          webrtc::IceCandidateType type,
          uint16_t min_port,
          uint16_t max_port,
          bool emit_local_for_anyaddress);
  bool Init();

  int SendTo(const void* data,
             size_t size,
             const webrtc::SocketAddress& addr,
             const rtc::PacketOptions& options,
             bool payload) override;

  void UpdateNetworkCost() override;

  rtc::DiffServCodePoint StunDscpValue() const override;

  void OnLocalAddressReady(webrtc::AsyncPacketSocket* socket,
                           const webrtc::SocketAddress& address);

  void PostAddAddress(bool is_final) override;

  void OnReadPacket(webrtc::AsyncPacketSocket* socket,
                    const rtc::ReceivedPacket& packet);

  void OnSentPacket(webrtc::AsyncPacketSocket* socket,
                    const rtc::SentPacket& sent_packet) override;

  void OnReadyToSend(webrtc::AsyncPacketSocket* socket);

  // This method will send STUN binding request if STUN server address is set.
  void MaybePrepareStunCandidate();

  void SendStunBindingRequests();

  // Helper function which will set `addr`'s IP to the default local address if
  // `addr` is the "any" address and `emit_local_for_anyaddress_` is true. When
  // returning false, it indicates that the operation has failed and the
  // address shouldn't be used by any candidate.
  bool MaybeSetDefaultLocalAddress(webrtc::SocketAddress* addr) const;

 private:
  // A helper class which can be called repeatedly to resolve multiple
  // addresses, as opposed to rtc::AsyncDnsResolverInterface, which can only
  // resolve one address per instance.
  class AddressResolver {
   public:
    explicit AddressResolver(
        webrtc::PacketSocketFactory* factory,
        std::function<void(const webrtc::SocketAddress&, int)> done_callback);

    void Resolve(const webrtc::SocketAddress& address,
                 int family,
                 const webrtc::FieldTrialsView& field_trials);
    bool GetResolvedAddress(const webrtc::SocketAddress& input,
                            int family,
                            webrtc::SocketAddress* output) const;

   private:
    typedef std::map<webrtc::SocketAddress,
                     std::unique_ptr<webrtc::AsyncDnsResolverInterface>>
        ResolverMap;

    webrtc::PacketSocketFactory* socket_factory_;
    // The function is called when resolving the specified address is finished.
    // The first argument is the input address, the second argument is the error
    // or 0 if it succeeded.
    std::function<void(const webrtc::SocketAddress&, int)> done_;
    // Resolver may fire callbacks that refer to done_, so ensure
    // that all resolvers are destroyed first.
    ResolverMap resolvers_;
  };

  // DNS resolution of the STUN server.
  void ResolveStunAddress(const webrtc::SocketAddress& stun_addr);
  void OnResolveResult(const webrtc::SocketAddress& input, int error);

  // Send a STUN binding request to the given address. Calling this method may
  // cause the set of known server addresses to be modified, eg. by replacing an
  // unresolved server address with a resolved address.
  void SendStunBindingRequest(const webrtc::SocketAddress& stun_addr);

  // Below methods handles binding request responses.
  void OnStunBindingRequestSucceeded(
      int rtt_ms,
      const webrtc::SocketAddress& stun_server_addr,
      const webrtc::SocketAddress& stun_reflected_addr);
  void OnStunBindingOrResolveRequestFailed(
      const webrtc::SocketAddress& stun_server_addr,
      int error_code,
      absl::string_view reason);

  // Sends STUN requests to the server.
  void OnSendPacket(const void* data, size_t size, StunRequest* req);

  // TODO(mallinaht) - Move this up to cricket::Port when SignalAddressReady is
  // changed to SignalPortReady.
  void MaybeSetPortCompleteOrError();

  bool HasStunCandidateWithAddress(const webrtc::SocketAddress& addr) const;

  // If this is a low-cost network, it will keep on sending STUN binding
  // requests indefinitely to keep the NAT binding alive. Otherwise, stop
  // sending STUN binding requests after HIGH_COST_PORT_KEEPALIVE_LIFETIME.
  int GetStunKeepaliveLifetime() {
    return (network_cost() >= webrtc::kNetworkCostHigh)
               ? HIGH_COST_PORT_KEEPALIVE_LIFETIME
               : INFINITE_LIFETIME;
  }

  ServerAddresses server_addresses_;
  ServerAddresses bind_request_succeeded_servers_;
  ServerAddresses bind_request_failed_servers_;
  StunRequestManager request_manager_;
  webrtc::AsyncPacketSocket* socket_;
  int error_;
  int send_error_count_ = 0;
  std::unique_ptr<AddressResolver> resolver_;
  bool ready_;
  int stun_keepalive_delay_;
  int stun_keepalive_lifetime_ = INFINITE_LIFETIME;
  rtc::DiffServCodePoint dscp_;

  StunStats stats_;

  // This is true by default and false when
  // PORTALLOCATOR_DISABLE_DEFAULT_LOCAL_CANDIDATE is specified.
  bool emit_local_for_anyaddress_;

  friend class StunBindingRequest;
};

class StunPort : public UDPPort {
 public:
  static std::unique_ptr<StunPort> Create(
      const PortParametersRef& args,
      uint16_t min_port,
      uint16_t max_port,
      const ServerAddresses& servers,
      std::optional<int> stun_keepalive_interval);
  [[deprecated("Pass arguments using PortParametersRef")]] static std::
      unique_ptr<StunPort>
      Create(webrtc::TaskQueueBase* thread,
             webrtc::PacketSocketFactory* factory,
             const rtc::Network* network,
             uint16_t min_port,
             uint16_t max_port,
             absl::string_view username,
             absl::string_view password,
             const ServerAddresses& servers,
             std::optional<int> stun_keepalive_interval,
             const webrtc::FieldTrialsView* field_trials);
  void PrepareAddress() override;

 protected:
  StunPort(const PortParametersRef& args,
           uint16_t min_port,
           uint16_t max_port,
           const ServerAddresses& servers);
};

}  // namespace cricket

#endif  // P2P_BASE_STUN_PORT_H_
