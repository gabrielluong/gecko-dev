// Copyright 2024 Mathias Bynens. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
author: Mathias Bynens
description: >
  Unicode property escapes for `Script=Warang_Citi`
info: |
  Generated by https://github.com/mathiasbynens/unicode-property-escapes-tests
  Unicode v16.0.0
esid: sec-static-semantics-unicodematchproperty-p
features: [regexp-unicode-property-escapes]
includes: [regExpUtils.js]
---*/

const matchSymbols = buildString({
  loneCodePoints: [
    0x0118FF
  ],
  ranges: [
    [0x0118A0, 0x0118F2]
  ]
});
testPropertyEscapes(
  /^\p{Script=Warang_Citi}+$/u,
  matchSymbols,
  "\\p{Script=Warang_Citi}"
);
testPropertyEscapes(
  /^\p{Script=Wara}+$/u,
  matchSymbols,
  "\\p{Script=Wara}"
);
testPropertyEscapes(
  /^\p{sc=Warang_Citi}+$/u,
  matchSymbols,
  "\\p{sc=Warang_Citi}"
);
testPropertyEscapes(
  /^\p{sc=Wara}+$/u,
  matchSymbols,
  "\\p{sc=Wara}"
);

const nonMatchSymbols = buildString({
  loneCodePoints: [],
  ranges: [
    [0x00DC00, 0x00DFFF],
    [0x000000, 0x00DBFF],
    [0x00E000, 0x01189F],
    [0x0118F3, 0x0118FE],
    [0x011900, 0x10FFFF]
  ]
});
testPropertyEscapes(
  /^\P{Script=Warang_Citi}+$/u,
  nonMatchSymbols,
  "\\P{Script=Warang_Citi}"
);
testPropertyEscapes(
  /^\P{Script=Wara}+$/u,
  nonMatchSymbols,
  "\\P{Script=Wara}"
);
testPropertyEscapes(
  /^\P{sc=Warang_Citi}+$/u,
  nonMatchSymbols,
  "\\P{sc=Warang_Citi}"
);
testPropertyEscapes(
  /^\P{sc=Wara}+$/u,
  nonMatchSymbols,
  "\\P{sc=Wara}"
);

reportCompare(0, 0);
