/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

<%namespace name="helpers" file="/helpers.mako.rs" />

${helpers.predefined_type(
    "text-overflow",
    "TextOverflow",
    "computed::TextOverflow::get_initial_value()",
    engines="gecko servo",
    servo_pref="layout.unimplemented",
    animation_type="discrete",
    boxed=True,
    spec="https://drafts.csswg.org/css-ui/#propdef-text-overflow",
    servo_restyle_damage="rebuild_and_reflow",
    affects="paint",
)}

${helpers.single_keyword(
    "unicode-bidi",
    "normal embed isolate bidi-override isolate-override plaintext",
    engines="gecko servo",
    gecko_enum_prefix="StyleUnicodeBidi",
    animation_type="none",
    spec="https://drafts.csswg.org/css-writing-modes/#propdef-unicode-bidi",
    servo_restyle_damage="rebuild_and_reflow",
    affects="layout",
)}

${helpers.predefined_type(
    "text-decoration-line",
    "TextDecorationLine",
    "specified::TextDecorationLine::none()",
    engines="gecko servo",
    initial_specified_value="specified::TextDecorationLine::none()",
    animation_type="discrete",
    spec="https://drafts.csswg.org/css-text-decor/#propdef-text-decoration-line",
    servo_restyle_damage="rebuild_and_reflow",
    affects="overflow",
)}

${helpers.single_keyword(
    "text-decoration-style",
    "solid double dotted dashed wavy -moz-none",
    engines="gecko servo",
    gecko_enum_prefix="StyleTextDecorationStyle",
    animation_type="discrete",
    spec="https://drafts.csswg.org/css-text-decor/#propdef-text-decoration-style",
    affects="overflow",
)}

${helpers.predefined_type(
    "text-decoration-color",
    "Color",
    "computed_value::T::currentcolor()",
    engines="gecko servo",
    initial_specified_value="specified::Color::currentcolor()",
    ignored_when_colors_disabled=True,
    spec="https://drafts.csswg.org/css-text-decor/#propdef-text-decoration-color",
    affects="paint",
)}

${helpers.predefined_type(
    "initial-letter",
    "InitialLetter",
    "computed::InitialLetter::normal()",
    engines="gecko",
    initial_specified_value="specified::InitialLetter::normal()",
    animation_type="discrete",
    gecko_pref="layout.css.initial-letter.enabled",
    spec="https://drafts.csswg.org/css-inline/#sizing-drop-initials",
    affects="layout",
)}

${helpers.predefined_type(
    "text-decoration-thickness",
    "TextDecorationLength",
    "generics::text::GenericTextDecorationLength::Auto",
    engines="gecko",
    initial_specified_value="generics::text::GenericTextDecorationLength::Auto",
    spec="https://drafts.csswg.org/css-text-decor-4/#text-decoration-width-property",
    affects="overflow",
)}
