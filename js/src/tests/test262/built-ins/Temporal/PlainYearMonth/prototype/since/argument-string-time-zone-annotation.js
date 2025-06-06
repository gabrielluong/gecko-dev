// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2022 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.plainyearmonth.prototype.since
description: Various forms of time zone annotation; critical flag has no effect
features: [Temporal]
includes: [temporalHelpers.js]
---*/

const tests = [
  ["2019-12-15T15:23[Asia/Kolkata]", "named, with no offset"],
  ["2019-12-15T15:23[!Europe/Vienna]", "named, with ! and no offset"],
  ["2019-12-15T15:23[+00:00]", "numeric, with no offset"],
  ["2019-12-15T15:23[!-02:30]", "numeric, with ! and no offset"],
  ["2019-12-15T15:23+00:00[UTC]", "named, with offset"],
  ["2019-12-15T15:23+00:00[!Africa/Abidjan]", "named, with offset and !"],
  ["2019-12-15T15:23+00:00[+01:00]", "numeric, with offset"],
  ["2019-12-15T15:23+00:00[!-08:00]", "numeric, with offset and !"],
];

const instance = new Temporal.PlainYearMonth(2019, 12);

tests.forEach(([arg, description]) => {
  const result = instance.since(arg);

  TemporalHelpers.assertDuration(
    result,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    `time zone annotation (${description})`
  );
});

reportCompare(0, 0);
