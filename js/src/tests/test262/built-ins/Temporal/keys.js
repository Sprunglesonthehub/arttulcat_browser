// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2021 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal-objects
description: Temporal has no enumerable properties
includes: [compareArray.js]
features: [Temporal]
---*/

const keys = Object.keys(Temporal);
assert.compareArray(keys, []);

reportCompare(0, 0);
