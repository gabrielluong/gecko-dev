// Copyright (c) 2012 Ecma International.  All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
es5id: 15.2.3.6-3-153
description: >
    Object.defineProperty - 'writable' property in 'Attributes' is not
    present  (8.10.5 step 6)
includes: [propertyHelper.js]
---*/

var obj = {};

var attr = {
  value: 100
};

Object.defineProperty(obj, "property", attr);

verifyProperty(obj, "property", {
  writable: false,
});

reportCompare(0, 0);
