// |reftest| skip -- not a test file
// Copyright (C) 2025 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

globalThis.evaluations.push("tla start");

await Promise.resolve(0);

globalThis.evaluations.push("tla end");
