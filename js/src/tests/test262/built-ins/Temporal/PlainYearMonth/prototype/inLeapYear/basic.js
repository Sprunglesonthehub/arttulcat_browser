// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2022 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-get-temporal.plainyearmonth.prototype.inleapyear
description: Basic test for inLeapYear
features: [Temporal]
---*/

assert.sameValue((new Temporal.PlainYearMonth(1976, 11)).inLeapYear,
  true, "leap year");
assert.sameValue((new Temporal.PlainYearMonth(1977, 11)).inLeapYear,
  false, "non-leap year");

reportCompare(0, 0);
