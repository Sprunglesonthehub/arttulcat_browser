// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2022 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.plaindatetime.compare
description: Negative zero, as an extended year, is rejected
features: [Temporal, arrow-function]
---*/

const invalidStrings = [
  "-000000-10-31",
  "-000000-10-31T17:45",
  "-000000-10-31T17:45Z",
  "-000000-10-31T17:45+01:00",
  "-000000-10-31T17:45+00:00[UTC]",
];

invalidStrings.forEach((arg) => {
  assert.throws(
    RangeError,
    () => Temporal.PlainDateTime.compare(arg, new Temporal.PlainDateTime(1976, 11, 18)),
    "reject minus zero as extended year (first argument)"
  );
  assert.throws(
    RangeError,
    () => Temporal.PlainDateTime.compare(new Temporal.PlainDateTime(1976, 11, 18), arg),
    "reject minus zero as extended year (second argument)"
  );
});

reportCompare(0, 0);
