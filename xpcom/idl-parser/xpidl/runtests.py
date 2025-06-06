#!/usr/bin/env python
#
# Any copyright is dedicated to the Public Domain.
# http://creativecommons.org/publicdomain/zero/1.0/
#
# Unit tests for xpidl.py

import json
import os
import sys

# Hack: the first entry in sys.path is the directory containing the script.
# This messes things up because that directory is the xpidl module, and that
# which conflicts with the xpidl submodule in the imports further below.
sys.path.pop(0)

import unittest

import mozunit

from xpidl import header, typescript, xpidl


class TestParser(unittest.TestCase):
    def setUp(self):
        self.p = xpidl.IDLParser()

    def testEmpty(self):
        i = self.p.parse("", filename="f")
        self.assertTrue(isinstance(i, xpidl.IDL))
        self.assertEqual([], i.productions)

    def testForwardInterface(self):
        i = self.p.parse("interface foo;", filename="f")
        self.assertTrue(isinstance(i, xpidl.IDL))
        self.assertTrue(isinstance(i.productions[0], xpidl.Forward))
        self.assertEqual("foo", i.productions[0].name)

    def testInterface(self):
        i = self.p.parse("[uuid(abc)] interface foo {};", filename="f")
        self.assertTrue(isinstance(i, xpidl.IDL))
        self.assertTrue(isinstance(i.productions[0], xpidl.Interface))
        self.assertEqual("foo", i.productions[0].name)

    def testAttributes(self):
        i = self.p.parse(
            "[scriptable, builtinclass, function, uuid(abc)] interface foo {};",
            filename="f",
        )
        self.assertTrue(isinstance(i, xpidl.IDL))
        self.assertTrue(isinstance(i.productions[0], xpidl.Interface))
        iface = i.productions[0]
        self.assertEqual("foo", iface.name)
        self.assertTrue(iface.attributes.scriptable)
        self.assertTrue(iface.attributes.builtinclass)
        self.assertTrue(iface.attributes.function)

        i = self.p.parse("[uuid(abc)] interface foo {};", filename="f")
        self.assertTrue(isinstance(i, xpidl.IDL))
        self.assertTrue(isinstance(i.productions[0], xpidl.Interface))
        iface = i.productions[0]
        self.assertEqual("foo", iface.name)
        self.assertFalse(iface.attributes.scriptable)

    def testMethod(self):
        i = self.p.parse(
            """[uuid(abc)] interface foo {
void bar();
};""",
            filename="f",
        )
        self.assertTrue(isinstance(i, xpidl.IDL))
        self.assertTrue(isinstance(i.productions[0], xpidl.Interface))
        iface = i.productions[0]
        m = iface.members[0]
        self.assertTrue(isinstance(m, xpidl.Method))
        self.assertEqual("bar", m.name)
        self.assertEqual(xpidl.TypeId("void"), m.type)

    def testMethodParams(self):
        i = self.p.parse(
            """
        [scriptable, uuid(aaa)] interface nsISupports {};
        [uuid(abc)] interface foo : nsISupports {
          long bar(in long a, in float b, [array] in long c);
        };""",
            filename="f",
        )
        i.resolve([], self.p, {})
        self.assertTrue(isinstance(i, xpidl.IDL))
        self.assertTrue(isinstance(i.productions[1], xpidl.Interface))
        iface = i.productions[1]
        m = iface.members[0]
        self.assertTrue(isinstance(m, xpidl.Method))
        self.assertEqual("bar", m.name)
        self.assertEqual(xpidl.TypeId("long"), m.type)
        self.assertEqual(3, len(m.params))
        self.assertEqual(xpidl.TypeId("long"), m.params[0].type)
        self.assertEqual("in", m.params[0].paramtype)
        self.assertEqual(xpidl.TypeId("float"), m.params[1].type)
        self.assertEqual("in", m.params[1].paramtype)
        self.assertEqual(xpidl.TypeId("long"), m.params[2].type)
        self.assertEqual("in", m.params[2].paramtype)
        self.assertTrue(isinstance(m.params[2].realtype, xpidl.LegacyArray))
        self.assertEqual("int32_t", m.params[2].realtype.type.name)

    def testAttribute(self):
        i = self.p.parse(
            """[uuid(abc)] interface foo {
attribute long bar;
};""",
            filename="f",
        )
        self.assertTrue(isinstance(i, xpidl.IDL))
        self.assertTrue(isinstance(i.productions[0], xpidl.Interface))
        iface = i.productions[0]
        a = iface.members[0]
        self.assertTrue(isinstance(a, xpidl.Attribute))
        self.assertEqual("bar", a.name)
        self.assertEqual(xpidl.TypeId("long"), a.type)

    def testOverloadedVirtual(self):
        i = self.p.parse(
            """
        [scriptable, uuid(00000000-0000-0000-0000-000000000000)] interface nsISupports {};
        [uuid(abc)] interface foo : nsISupports {
          attribute long bar;
          void getBar();
        };""",
            filename="f",
        )
        self.assertTrue(isinstance(i, xpidl.IDL))
        i.resolve([], self.p, {})

        class FdMock:
            def write(self, s):
                pass

        try:
            header.print_header(i, FdMock(), filename="f", relpath="f")
            self.assertTrue(False, "Header printing failed to fail")
        except Exception as e:
            self.assertEqual(
                e.args[0],
                "Unexpected overloaded virtual method GetBar in interface foo",
            )

    def testNotISupports(self):
        i = self.p.parse(
            """
        [uuid(abc)] interface foo {};
        """,
            filename="f",
        )
        self.assertTrue(isinstance(i, xpidl.IDL))
        try:
            i.resolve([], self.p, {})
            self.assertTrue(
                False, "Must check that interfaces inherit from nsISupports"
            )
        except xpidl.IDLError as e:
            self.assertEqual(e.args[0], "Interface 'foo' must inherit from nsISupports")

    def testBuiltinClassParent(self):
        i = self.p.parse(
            """
        [scriptable, uuid(aaa)] interface nsISupports {};
        [scriptable, builtinclass, uuid(abc)] interface foo : nsISupports {};
        [scriptable, uuid(def)] interface bar : foo {};
        """,
            filename="f",
        )
        self.assertTrue(isinstance(i, xpidl.IDL))
        try:
            i.resolve([], self.p, {})
            self.assertTrue(
                False, "non-builtinclasses can't inherit from builtinclasses"
            )
        except xpidl.IDLError as e:
            self.assertEqual(
                e.args[0],
                "interface 'bar' is not builtinclass but derives from builtinclass 'foo'",
            )

    def testScriptableNotXPCOM(self):
        # notxpcom method requires builtinclass on the interface
        i = self.p.parse(
            """
        [scriptable, uuid(aaa)] interface nsISupports {};
        [scriptable, uuid(abc)] interface nsIScriptableWithNotXPCOM : nsISupports {
          [notxpcom] void method2();
        };
        """,
            filename="f",
        )
        self.assertTrue(isinstance(i, xpidl.IDL))
        try:
            i.resolve([], self.p, {})
            self.assertTrue(
                False,
                "Resolve should fail for non-builtinclasses with notxpcom methods",
            )
        except xpidl.IDLError as e:
            self.assertEqual(
                e.args[0],
                (
                    "scriptable interface 'nsIScriptableWithNotXPCOM' "
                    "must be marked [builtinclass] because it contains a [notxpcom] "
                    "method 'method2'"
                ),
            )

        # notxpcom attribute requires builtinclass on the interface
        i = self.p.parse(
            """
        interface nsISomeInterface;
        [scriptable, uuid(aaa)] interface nsISupports {};
        [scriptable, uuid(abc)] interface nsIScriptableWithNotXPCOM : nsISupports {
          [notxpcom] attribute nsISomeInterface attrib;
        };
        """,
            filename="f",
        )
        self.assertTrue(isinstance(i, xpidl.IDL))
        try:
            i.resolve([], self.p, {})
            self.assertTrue(
                False,
                "Resolve should fail for non-builtinclasses with notxpcom attributes",
            )
        except xpidl.IDLError as e:
            self.assertEqual(
                e.args[0],
                (
                    "scriptable interface 'nsIScriptableWithNotXPCOM' must be marked "
                    "[builtinclass] because it contains a [notxpcom] attribute 'attrib'"
                ),
            )

    def testUndefinedConst(self):
        i = self.p.parse(
            """
        [scriptable, uuid(aaa)] interface nsISupports {};
        [scriptable, uuid(abc)] interface foo : nsISupports {
          const unsigned long X = Y + 1;
        };
        """,
            filename="f",
        )
        self.assertTrue(isinstance(i, xpidl.IDL))
        try:
            i.resolve([], self.p, {})
            self.assertTrue(False, "Must detect undefined symbols")
        except xpidl.IDLError as e:
            self.assertEqual(e.args[0], ("cannot find symbol 'Y'"))


class TestTypescript(unittest.TestCase):
    """A few basic smoke tests for typescript generation."""

    dir = os.path.dirname(__file__)
    src = os.path.join(dir, "..", "..", "..")

    # We use the xpctest.xpt *.idl files from:
    tests_dir = os.path.join(src, "js/xpconnect/tests/idl")
    files = [
        "xpctest_attributes.idl",
        "xpctest_bug809674.idl",
        "xpctest_cenums.idl",
        "xpctest_interfaces.idl",
        "xpctest_params.idl",
        "xpctest_returncode.idl",
        "xpctest_utils.idl",
    ]

    fixtures = os.path.join(dir, "fixtures")
    inc_dirs = [os.path.join(src, "xpcom/base")]

    def setUp(self):
        self.parser = xpidl.IDLParser()

    def test_d_json(self):
        mods = []
        for file in self.files:
            path = os.path.join(self.tests_dir, file)
            idl = self.parser.parse(open(path).read(), path)
            idl.resolve(self.inc_dirs, self.parser, {})
            mods.append(typescript.ts_source(idl))

        # Add a trailing newline, so that it matches the additional newline
        # in xpctest.d.json. This makes it simpler for editors which automatically
        # add a newline.
        result = json.dumps(mods, indent=2, sort_keys=True) + "\n"
        expected = open(os.path.join(self.fixtures, "xpctest.d.json")).read()
        self.assertEqual(result, expected, "types data json does not match")


if __name__ == "__main__":
    mozunit.main(runwith="unittest")
