/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

/**
 * Test document with body[role=application] and a top-level group
 */
addAccessibleTask(
  `<div role="grouping" id="group"><p>hello</p><p>world</p></div>`,
  async (browser, accDoc) => {
    let doc = accDoc.nativeInterface.QueryInterface(
      Ci.nsIAccessibleMacInterface
    );

    is(
      doc.getAttributeValue("AXRole"),
      "AXWebArea",
      "doc still has web area role"
    );
    is(
      doc.getAttributeValue("AXRoleDescription"),
      "HTML Content",
      "doc has correct role description"
    );
    ok(
      !doc.attributeNames.includes("AXSubrole"),
      "sub role not available on web area"
    );

    let rootGroup = doc.getAttributeValue("AXChildren")[0];
    is(
      rootGroup.getAttributeValue("AXIdentifier"),
      "root-group",
      "Is generated root group"
    );
    is(
      rootGroup.getAttributeValue("AXRole"),
      "AXGroup",
      "root group has AXGroup role"
    );
    is(
      rootGroup.getAttributeValue("AXSubrole"),
      "AXLandmarkApplication",
      "root group has application subrole"
    );
    is(
      rootGroup.getAttributeValue("AXRoleDescription"),
      "application",
      "root group has application role description"
    );
  },
  { contentDocBodyAttrs: { role: "application" } }
);

/**
 * Test document with dialog role and heading
 */
addAccessibleTask(
  `<body role="dialog" aria-labelledby="h">
    <h1 id="h">
      We're building a richer search experience
    </h1>
  </body>`,
  async (browser, accDoc) => {
    let doc = accDoc.nativeInterface.QueryInterface(
      Ci.nsIAccessibleMacInterface
    );
    let docChildren = doc.getAttributeValue("AXChildren");
    is(docChildren.length, 1, "The document contains a root group");

    let rootGroup = docChildren[0];
    is(
      rootGroup.getAttributeValue("AXIdentifier"),
      "root-group",
      "Is generated root group"
    );

    is(rootGroup.getAttributeValue("AXRole"), "AXGroup", "Inherits role");

    is(
      rootGroup.getAttributeValue("AXSubrole"),
      "AXApplicationDialog",
      "Inherits subrole"
    );
    let rootGroupChildren = rootGroup.getAttributeValue("AXChildren");
    is(rootGroupChildren.length, 1, "Root group has one child");

    is(
      rootGroupChildren[0].getAttributeValue("AXRole"),
      "AXHeading",
      "Heading is child of root group"
    );

    // From bottom-up
    let heading = getNativeInterface(accDoc, "h");
    rootGroup = heading.getAttributeValue("AXParent");
    is(
      rootGroup.getAttributeValue("AXIdentifier"),
      "root-group",
      "Parent is generated root group"
    );
  }
);
