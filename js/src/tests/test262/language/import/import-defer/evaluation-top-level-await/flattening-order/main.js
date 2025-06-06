// |reftest| skip module async -- import-defer is not supported
// Copyright (C) 2024 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-innermoduleevaluation
description: >
  Async dependencies of deferred modules are executed in the right order
info: |
  16.2.1.5.3.1 InnerModuleEvaluation ( _module_, _stack_, _index_ )
    1. ...
    1. Let _evaluationList_ be a new empty List.
    1. For each ModuleRequest Record _required_ of _module_.[[RequestedModules]], do
        1. Let _requiredModule_ be GetImportedModule(_module_, _required_.[[Specifier]]).
        1. If _required_.[[Phase]] is ~defer~, then
            i. Let _additionalModules_ be GatherAsynchronousTransitiveDependencies(_requiredModule_).
            ii. For each Module Record _additionalModule_ of _additionalModules_, do
                1. If _evaluationList_ does not contain _additionalModule_, then
                    a. Append _additionalModule_ to _evaluationList_.
        1. Else if _evaluationList_ does not contain _requiredModule_, then
            i. Append _requiredModule_ to _evaluationList_.
    1. ...
    1. For each Module Record _requiredModule_ of _evaluationList_, do
      1. Set _index_ to ? InnerModuleEvaluation(_requiredModule_, _stack_, _index_).
      1. ...

  GatherAsynchronousTransitiveDependencies ( _module_, [ _seen_ ] )
    1. If _seen_ is not specified, let _seen_ be a new empty List.
    1. Let _result_ be a new empty List.
    1. If _seen_ contains _module_, return _result_.
    1. Append _module_ to _seen_.
    1. If _module_ is not a Cyclic Module Record, return _result_.
    1. If _module_.[[Status]] is either ~evaluating~ or ~evaluated~, return _result_.
    1. If _module_.[[HasTLA]] is *true*, then
      1. Append _module_ to _result_.
      1. Return _result_.
    1. For each ModuleRequest Record _required_ of _module_.[[RequestedModules]], do
      1. Let _requiredModule_ be GetImportedModule(_module_, _required_.[[Specifier]]).
      1. Let _additionalModules_ be GatherAsynchronousTransitiveDependencies(_requiredModule_, _seen_).
      1. For each Module Record _m_ of _additionalModules_, do
        1. If _result_ does not contain _m_, append _m_ to _result_.
    1. Return _result_.

flags: [module, async]
features: [import-defer, top-level-await]
includes: [compareArray.js]
---*/

import "./setup_FIXTURE.js";

import "./dep-1_FIXTURE.js";
import defer * as ns2 from "./dep-2_FIXTURE.js";
import "./dep-3_FIXTURE.js";
import defer * as ns4 from "./dep-4_FIXTURE.js";
import "./dep-5_FIXTURE.js";

assert.compareArray(globalThis.evaluations, [
  "1",
  "2.1.1 start",
  "2.2.1",
  "2.2 start",
  "3",
  "5",
  "2.1.1 end",
  "2.2 end",
  "4.1 start",
  "4.1 end"
]);

globalThis.evaluations = [];
ns2.x;
assert.compareArray(globalThis.evaluations, ["2.1", "2"]);

globalThis.evaluations = [];
ns4.x;
assert.compareArray(globalThis.evaluations, ["4"]);

$DONE();
