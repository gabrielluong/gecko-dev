// |reftest| skip-if(!(this.hasOwnProperty('getBuildConfiguration')&&getBuildConfiguration('explicit-resource-management'))) async -- explicit-resource-management is not enabled unconditionally
// Copyright (C) 2024 the V8 project authors. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
description: Test exception handling when dispose method throws.
includes: [asyncHelpers.js]
flags: [async]
features: [explicit-resource-management]
---*/

// Dispose method throws -----------------------------
asyncTest(async function() {
  async function TestDisposeMethodThrows() {
    await using x = {
      value: 1,
      [Symbol.asyncDispose]() {
        throw new Test262Error('Symbol.asyncDispose is throwing!');
      }
    };
  };
  await assert.throwsAsync(
      Test262Error, () => TestDisposeMethodThrows(),
      'Symbol.asyncDispose is throwing!');
});
