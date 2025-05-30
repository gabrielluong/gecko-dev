// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2020 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
description: Temporal.PlainDate.prototype.add throws a RangeError if any value in a property bag is -Infinity
esid: sec-temporal.plaindate.prototype.add
features: [Temporal]
---*/

const overflows = ["constrain", "reject"];
const fields = ["years", "months", "weeks", "days", "hours", "minutes", "seconds", "milliseconds", "microseconds", "nanoseconds"];

const instance = Temporal.PlainDate.from({ year: 2000, month: 5, day: 2 });

overflows.forEach((overflow) => {
  fields.forEach((field) => {
    assert.throws(RangeError, () => instance.add({ [field]: -Infinity }, { overflow }));
  });
});

let calls = 0;
const obj = {
  valueOf() {
    calls++;
    return -Infinity;
  }
};

overflows.forEach((overflow) => {
  fields.forEach((field) => {
    calls = 0;
    assert.throws(RangeError, () => instance.add({ [field]: obj }, { overflow }));
    assert.sameValue(calls, 1, "it fails after fetching the primitive value");
  });
});

reportCompare(0, 0);
