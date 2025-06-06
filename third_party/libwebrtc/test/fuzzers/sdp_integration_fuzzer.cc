/*
 *  Copyright (c) 2021 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <utility>

#include "absl/strings/string_view.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "api/test/rtc_error_matchers.h"
#include "pc/test/integration_test_helpers.h"
#include "pc/test/mock_peer_connection_observers.h"
#include "rtc_base/checks.h"
#include "test/gmock.h"
#include "test/wait_until.h"

namespace webrtc {

class FuzzerTest : public PeerConnectionIntegrationBaseTest {
 public:
  FuzzerTest()
      : PeerConnectionIntegrationBaseTest(SdpSemantics::kUnifiedPlan) {}

  void RunNegotiateCycle(absl::string_view message) {
    CreatePeerConnectionWrappers();
    // Note - we do not do test.ConnectFakeSignaling(); all signals
    // generated are discarded.

    auto srd_observer =
        rtc::make_ref_counted<FakeSetRemoteDescriptionObserver>();

    SdpParseError error;
    std::unique_ptr<SessionDescriptionInterface> sdp =
        CreateSessionDescription(SdpType::kOffer, std::string(message), &error);
    caller()->pc()->SetRemoteDescription(std::move(sdp), srd_observer);
    // Wait a short time for observer to be called. Timeout is short
    // because the fuzzer should be trying many branches.
    EXPECT_THAT(
        WaitUntil([&] { return srd_observer->called(); }, ::testing::IsTrue()),
        IsRtcOk());

    // If set-remote-description was successful, try to answer.
    auto sld_observer =
        rtc::make_ref_counted<FakeSetLocalDescriptionObserver>();
    if (srd_observer->error().ok()) {
      caller()->pc()->SetLocalDescription(sld_observer);
      EXPECT_THAT(WaitUntil([&] { return sld_observer->called(); },
                            ::testing::IsTrue()),
                  IsRtcOk());
    }
    // If there is an EXPECT failure, die here.
    RTC_CHECK(!HasFailure());
  }

  // This test isn't using the test definition macros, so we have to
  // define the TestBody() function, even though we don't need it.
  void TestBody() override {}
};

void FuzzOneInput(const uint8_t* data, size_t size) {
  if (size > 16384) {
    return;
  }

  FuzzerTest test;
  test.RunNegotiateCycle(
      absl::string_view(reinterpret_cast<const char*>(data), size));
}

}  // namespace webrtc
