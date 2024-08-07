// Copyright (c) 2012 Ecma International.  All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
es5id: 15.2.3.6-3-145
description: >
    Object.defineProperty - 'Attributes' is a Date object that uses
    Object's [[Get]] method to access the 'value' property  (8.10.5
    step 5.a)
---*/

var obj = {};

var dateObj = new Date(0);

dateObj.value = "Date";

Object.defineProperty(obj, "property", dateObj);

assert.sameValue(obj.property, "Date", 'obj.property');

reportCompare(0, 0);
