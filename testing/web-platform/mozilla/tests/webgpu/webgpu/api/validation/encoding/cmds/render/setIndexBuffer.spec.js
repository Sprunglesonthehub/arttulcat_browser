/**
* AUTO-GENERATED - DO NOT EDIT. Source: https://github.com/gpuweb/cts
**/export const description = `
Validation tests for setIndexBuffer on render pass and render bundle.
`;import { makeTestGroup } from '../../../../../../common/framework/test_group.js';
import { GPUConst } from '../../../../../constants.js';
import { kResourceStates, AllFeaturesMaxLimitsGPUTest } from '../../../../../gpu_test.js';
import * as vtu from '../../../validation_test_utils.js';

import { kRenderEncodeTypeParams, buildBufferOffsetAndSizeOOBTestParams } from './render.js';

export const g = makeTestGroup(AllFeaturesMaxLimitsGPUTest);

g.test('index_buffer_state').
desc(
  `
Tests index buffer must be valid.
  `
).
paramsSubcasesOnly(kRenderEncodeTypeParams.combine('state', kResourceStates)).
fn((t) => {
  const { encoderType, state } = t.params;
  const indexBuffer = vtu.createBufferWithState(t, state, {
    size: 16,
    usage: GPUBufferUsage.INDEX
  });

  const { encoder, validateFinishAndSubmitGivenState } = t.createEncoder(encoderType);
  encoder.setIndexBuffer(indexBuffer, 'uint32');
  validateFinishAndSubmitGivenState(state);
});

g.test('index_buffer,device_mismatch').
desc('Tests setIndexBuffer cannot be called with an index buffer created from another device').
paramsSubcasesOnly(kRenderEncodeTypeParams.combine('mismatched', [true, false])).
beforeAllSubcases((t) => t.usesMismatchedDevice()).
fn((t) => {
  const { encoderType, mismatched } = t.params;
  const sourceDevice = mismatched ? t.mismatchedDevice : t.device;

  const indexBuffer = t.trackForCleanup(
    sourceDevice.createBuffer({
      size: 16,
      usage: GPUBufferUsage.INDEX
    })
  );

  const { encoder, validateFinish } = t.createEncoder(encoderType);
  encoder.setIndexBuffer(indexBuffer, 'uint32');
  validateFinish(!mismatched);
});

g.test('index_buffer_usage').
desc(
  `
Tests index buffer must have 'Index' usage.
  `
).
paramsSubcasesOnly(
  kRenderEncodeTypeParams.combine('usage', [
  GPUConst.BufferUsage.INDEX, // control case
  GPUConst.BufferUsage.COPY_DST,
  GPUConst.BufferUsage.COPY_DST | GPUConst.BufferUsage.INDEX]
  )
).
fn((t) => {
  const { encoderType, usage } = t.params;
  const indexBuffer = t.createBufferTracked({
    size: 16,
    usage
  });

  const { encoder, validateFinish } = t.createEncoder(encoderType);
  encoder.setIndexBuffer(indexBuffer, 'uint32');
  validateFinish((usage & GPUBufferUsage.INDEX) !== 0);
});

g.test('offset_alignment').
desc(
  `
Tests offset must be a multiple of index format’s byte size.
  `
).
paramsSubcasesOnly(
  kRenderEncodeTypeParams.
  combine('indexFormat', ['uint16', 'uint32']).
  expand('offset', (p) => {
    return p.indexFormat === 'uint16' ? [0, 1, 2] : [0, 2, 4];
  })
).
fn((t) => {
  const { encoderType, indexFormat, offset } = t.params;
  const indexBuffer = t.createBufferTracked({
    size: 16,
    usage: GPUBufferUsage.INDEX
  });

  const { encoder, validateFinish } = t.createEncoder(encoderType);
  encoder.setIndexBuffer(indexBuffer, indexFormat, offset);

  const alignment =
  indexFormat === 'uint16' ? Uint16Array.BYTES_PER_ELEMENT : Uint32Array.BYTES_PER_ELEMENT;
  validateFinish(offset % alignment === 0);
});

g.test('offset_and_size_oob').
desc(
  `
Tests offset and size cannot be larger than index buffer size.
  `
).
paramsSubcasesOnly(buildBufferOffsetAndSizeOOBTestParams(4, 256)).
fn((t) => {
  const { encoderType, offset, size, _valid } = t.params;
  const indexBuffer = t.createBufferTracked({
    size: 256,
    usage: GPUBufferUsage.INDEX
  });

  const { encoder, validateFinish } = t.createEncoder(encoderType);
  encoder.setIndexBuffer(indexBuffer, 'uint32', offset, size);
  validateFinish(_valid);
});