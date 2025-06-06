/*
 *  Copyright 2018 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef P2P_TEST_MOCK_ICE_CONTROLLER_H_
#define P2P_TEST_MOCK_ICE_CONTROLLER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "api/array_view.h"
#include "p2p/base/connection.h"
#include "p2p/base/ice_controller_factory_interface.h"
#include "p2p/base/ice_controller_interface.h"
#include "p2p/base/ice_switch_reason.h"
#include "p2p/base/ice_transport_internal.h"
#include "p2p/base/transport_description.h"
#include "test/gmock.h"

namespace webrtc {

class MockIceController : public cricket::IceControllerInterface {
 public:
  explicit MockIceController(const IceControllerFactoryArgs& /* args */) {}
  ~MockIceController() override = default;

  MOCK_METHOD(void, SetIceConfig, (const webrtc::IceConfig&), (override));
  MOCK_METHOD(void,
              SetSelectedConnection,
              (const cricket::Connection*),
              (override));
  MOCK_METHOD(void, AddConnection, (const cricket::Connection*), (override));
  MOCK_METHOD(void,
              OnConnectionDestroyed,
              (const cricket::Connection*),
              (override));
  MOCK_METHOD(rtc::ArrayView<const cricket::Connection* const>,
              GetConnections,
              (),
              (const, override));
  MOCK_METHOD(rtc::ArrayView<const cricket::Connection*>,
              connections,
              (),
              (const, override));
  MOCK_METHOD(bool, HasPingableConnection, (), (const, override));
  MOCK_METHOD(cricket::IceControllerInterface::PingResult,
              SelectConnectionToPing,
              (int64_t),
              (override));
  MOCK_METHOD(bool,
              GetUseCandidateAttr,
              (const cricket::Connection*,
               webrtc::NominationMode,
               cricket::IceMode),
              (const, override));
  MOCK_METHOD(const cricket::Connection*,
              FindNextPingableConnection,
              (),
              (override));
  MOCK_METHOD(void,
              MarkConnectionPinged,
              (const cricket::Connection*),
              (override));
  MOCK_METHOD(cricket::IceControllerInterface::SwitchResult,
              ShouldSwitchConnection,
              (cricket::IceSwitchReason, const cricket::Connection*),
              (override));
  MOCK_METHOD(cricket::IceControllerInterface::SwitchResult,
              SortAndSwitchConnection,
              (cricket::IceSwitchReason),
              (override));
  MOCK_METHOD(std::vector<const cricket::Connection*>,
              PruneConnections,
              (),
              (override));
};

class MockIceControllerFactory : public IceControllerFactoryInterface {
 public:
  ~MockIceControllerFactory() override = default;

  std::unique_ptr<cricket::IceControllerInterface> Create(
      const IceControllerFactoryArgs& args) override {
    RecordIceControllerCreated();
    return std::make_unique<MockIceController>(args);
  }

  MOCK_METHOD(void, RecordIceControllerCreated, ());
};

}  //  namespace webrtc

// Re-export symbols from the webrtc namespace for backwards compatibility.
// TODO(bugs.webrtc.org/4222596): Remove once all references are updated.
namespace cricket {
using ::webrtc::MockIceController;
using ::webrtc::MockIceControllerFactory;
}  // namespace cricket

#endif  // P2P_TEST_MOCK_ICE_CONTROLLER_H_
