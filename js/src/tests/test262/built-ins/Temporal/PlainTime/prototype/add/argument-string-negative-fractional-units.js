// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2021 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.plaintime.prototype.add
description: Strings with fractional duration units are treated with the correct sign
includes: [temporalHelpers.js]
features: [Temporal]
---*/

const instance = new Temporal.PlainTime();

const resultHours = instance.add("-PT24.567890123H");
TemporalHelpers.assertPlainTime(resultHours, 23, 25, 55, 595, 557, 200, "negative fractional hours");

const resultMinutes = instance.add("-PT1440.567890123M");
TemporalHelpers.assertPlainTime(resultMinutes, 23, 59, 25, 926, 592, 620, "negative fractional minutes");

reportCompare(0, 0);
