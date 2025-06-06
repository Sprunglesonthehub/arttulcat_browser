/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_FenceD3D11_H
#define MOZILLA_GFX_FenceD3D11_H

#include <unordered_map>

#include "mozilla/gfx/FileHandleWrapper.h"
#include "mozilla/layers/Fence.h"

struct ID3D11Device;
struct ID3D11Fence;

namespace mozilla {
namespace layers {

//
// A class for wrapping ID3D11Fence.
//
// The class can be used for singaling fence and waiting fence. When the class
// is created by Create(), it can be used for singaling fence and waiting
// fence. When the class is created by CreateFromHandle() it can be used only
// for waiting fence.
//
// There is a case that ID3D12Fence is used for fence signaling. In this case,
// the class can be used for waitng fence by using CreateFromHandle().
//
// IncrementAndSignal() is used for signaling fence. The fence will be updated
// after all previous work has completed.
//
// For waiting fence, Update() is used to update the target value of the
// waiting. Wait() is then used to wait for the fence.
//
class FenceD3D11 final : public Fence {
 public:
  static RefPtr<FenceD3D11> Create(ID3D11Device* aDevice);
  static RefPtr<FenceD3D11> CreateFromHandle(
      RefPtr<gfx::FileHandleWrapper> aHandle,
      const RefPtr<ID3D11Device> aDevice);

  FenceD3D11* AsFenceD3D11() override { return this; }

  // Check if ID3D11Device suppors ID3D11Fence creation.
  static bool IsSupported(ID3D11Device* aDevice);

  RefPtr<FenceD3D11> CloneFromHandle();

  // Updates mSignalFence to incremented value after all previous work has
  // completed. Used only when FenceD3D11 is created by FenceD3D11::Create().
  bool IncrementAndSignal();

  // Update FenceValue to the specified value.
  // Used only when FenceD3D11 is created by FenceD3D11::CreateFromHandle().
  void Update(uint64_t aFenceValue);

  // Wait for fence until it reaches or exceeds mFenceValue.
  bool Wait(ID3D11Device* aDevice);

  uint64_t GetFenceValue() const { return mFenceValue; }

  enum class OwnsFence : bool { No, Yes };

  const OwnsFence mOwnsFence;
  // Device that is used for creating mSignalFence.
  const RefPtr<ID3D11Device> mDevice;
  // Fence that is used for updating fence value.
  // Valid only when created with FenceD3D11::Create().
  const RefPtr<ID3D11Fence> mSignalFence;
  const RefPtr<gfx::FileHandleWrapper> mHandle;

 protected:
  FenceD3D11(const OwnsFence aOwnsFence, const RefPtr<ID3D11Device> aDevice,
             const RefPtr<ID3D11Fence> aSignalFence,
             const RefPtr<gfx::FileHandleWrapper>& aHandle);
  virtual ~FenceD3D11();

  uint64_t mFenceValue = 0;
  // Fences that are used for waiting.
  // They are opened for each D3D11 device that the fence is waited on.
  // XXX change to LRU cache
  std::unordered_map<const ID3D11Device*, RefPtr<ID3D11Fence>> mWaitFenceMap;
};

}  // namespace layers
}  // namespace mozilla

#endif  // MOZILLA_GFX_FenceD3D11_H
