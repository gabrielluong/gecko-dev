// Copyright 2024 Mathias Bynens. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
author: Mathias Bynens
description: >
  Extended character classes enabled by the RegExp `v` flag support
  properties of strings, string literals, and set operations
info: |
  Generated by https://github.com/mathiasbynens/unicode-property-escapes-tests
  Unicode v16.0.0
esid: sec-patterns
features: [regexp-v-flag]
includes: [regExpUtils.js]
---*/

testExtendedCharacterClass({
  regExp: /^[__]+$/v,
  expression: "[__]",
  matchStrings: [
    "_"
  ],
  nonMatchStrings: [
    "6\uFE0F\u20E3",
    "7",
    "9\uFE0F\u20E3",
    "C",
    "\u2603",
    "\u{1D306}",
    "\u{1F1E7}\u{1F1EA}"
  ],
});

reportCompare(0, 0);
