// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2024 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.now.plaintimeiso
description: >
  Time zone parsing from ISO strings does not accept sub-minute UTC offset as
  time zone identifier
features: [Temporal]
---*/

for (const timeZone of ["-12:12:59.9", "2021-08-19T17:30:45.123456789-12:12:59.9[-12:12:59.9]"]) {
  assert.throws(
    RangeError,
    () => Temporal.Now.plainTimeISO(timeZone),
    `${timeZone} is not a valid time zone string`
  );
}

reportCompare(0, 0);
