/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! A collection of invalidations due to changes in which stylesheets affect a
//! document.

#![deny(unsafe_code)]

use crate::context::QuirksMode;
use crate::dom::{TDocument, TElement, TNode};
use crate::invalidation::element::element_wrapper::{ElementSnapshot, ElementWrapper};
use crate::invalidation::element::restyle_hints::RestyleHint;
use crate::media_queries::Device;
use crate::selector_parser::{SelectorImpl, Snapshot, SnapshotMap};
use crate::shared_lock::SharedRwLockReadGuard;
use crate::stylesheets::{CssRule, StylesheetInDocument};
use crate::stylesheets::{EffectiveRules, EffectiveRulesIterator};
use crate::simple_buckets_map::SimpleBucketsMap;
use crate::values::AtomIdent;
use crate::LocalName as SelectorLocalName;
use selectors::parser::{Component, LocalName, Selector};

/// The kind of change that happened for a given rule.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, Hash, MallocSizeOf, PartialEq)]
pub enum RuleChangeKind {
    /// Some change in the rule which we don't know about, and could have made
    /// the rule change in any way.
    Generic = 0,
    /// The rule was inserted.
    Insertion,
    /// The rule was removed.
    Removal,
    /// A change in the declarations of a style rule.
    StyleRuleDeclarations,
}

/// A style sheet invalidation represents a kind of element or subtree that may
/// need to be restyled. Whether it represents a whole subtree or just a single
/// element is determined by the given InvalidationKind in
/// StylesheetInvalidationSet's maps.
#[derive(Debug, Eq, Hash, MallocSizeOf, PartialEq)]
enum Invalidation {
    /// An element with a given id.
    ID(AtomIdent),
    /// An element with a given class name.
    Class(AtomIdent),
    /// An element with a given local name.
    LocalName {
        name: SelectorLocalName,
        lower_name: SelectorLocalName,
    },
}

impl Invalidation {
    fn is_id(&self) -> bool {
        matches!(*self, Invalidation::ID(..))
    }

    fn is_id_or_class(&self) -> bool {
        matches!(*self, Invalidation::ID(..) | Invalidation::Class(..))
    }
}

/// Whether we should invalidate just the element, or the whole subtree within
/// it.
#[derive(Clone, Copy, Debug, Eq, MallocSizeOf, Ord, PartialEq, PartialOrd)]
enum InvalidationKind {
    None = 0,
    Element,
    Scope,
}

impl std::ops::BitOrAssign for InvalidationKind {
    #[inline]
    fn bitor_assign(&mut self, other: Self) {
        *self = std::cmp::max(*self, other);
    }
}

impl InvalidationKind {
    #[inline]
    fn is_scope(self) -> bool {
        matches!(self, Self::Scope)
    }

    #[inline]
    fn add(&mut self, other: Option<&InvalidationKind>) {
        if let Some(other) = other {
            *self |= *other;
        }
    }
}

/// A set of invalidations due to stylesheet additions.
///
/// TODO(emilio): We might be able to do the same analysis for media query
/// changes too (or even selector changes?).
#[derive(Debug, Default, MallocSizeOf)]
pub struct StylesheetInvalidationSet {
    buckets: SimpleBucketsMap<InvalidationKind>,
    fully_invalid: bool,
}

impl StylesheetInvalidationSet {
    /// Create an empty `StylesheetInvalidationSet`.
    pub fn new() -> Self {
        Default::default()
    }

    /// Mark the DOM tree styles' as fully invalid.
    pub fn invalidate_fully(&mut self) {
        debug!("StylesheetInvalidationSet::invalidate_fully");
        self.clear();
        self.fully_invalid = true;
    }

    fn shrink_if_needed(&mut self) {
        if self.fully_invalid {
            return;
        }
        self.buckets.shrink_if_needed();
    }

    /// Analyze the given stylesheet, and collect invalidations from their
    /// rules, in order to avoid doing a full restyle when we style the document
    /// next time.
    pub fn collect_invalidations_for<S>(
        &mut self,
        device: &Device,
        stylesheet: &S,
        guard: &SharedRwLockReadGuard,
    ) where
        S: StylesheetInDocument,
    {
        debug!("StylesheetInvalidationSet::collect_invalidations_for");
        if self.fully_invalid {
            debug!(" > Fully invalid already");
            return;
        }

        if !stylesheet.enabled() || !stylesheet.is_effective_for_device(device, guard) {
            debug!(" > Stylesheet was not effective");
            return; // Nothing to do here.
        }

        let quirks_mode = device.quirks_mode();
        for rule in stylesheet.effective_rules(device, guard) {
            self.collect_invalidations_for_rule(
                rule,
                guard,
                device,
                quirks_mode,
                /* is_generic_change = */ false,
            );
            if self.fully_invalid {
                break;
            }
        }

        self.shrink_if_needed();

        debug!(" > resulting class invalidations: {:?}", self.buckets.classes);
        debug!(" > resulting id invalidations: {:?}", self.buckets.ids);
        debug!(
            " > resulting local name invalidations: {:?}",
            self.buckets.local_names
        );
        debug!(" > fully_invalid: {}", self.fully_invalid);
    }

    /// Clears the invalidation set, invalidating elements as needed if
    /// `document_element` is provided.
    ///
    /// Returns true if any invalidations ocurred.
    pub fn flush<E>(&mut self, document_element: Option<E>, snapshots: Option<&SnapshotMap>) -> bool
    where
        E: TElement,
    {
        debug!(
            "Stylist::flush({:?}, snapshots: {})",
            document_element,
            snapshots.is_some()
        );
        let have_invalidations = match document_element {
            Some(e) => self.process_invalidations(e, snapshots),
            None => false,
        };
        self.clear();
        have_invalidations
    }

    /// Returns whether there's no invalidation to process.
    pub fn is_empty(&self) -> bool {
        !self.fully_invalid &&
            self.buckets.is_empty()
    }

    fn invalidation_kind_for<E>(
        &self,
        element: E,
        snapshot: Option<&Snapshot>,
        quirks_mode: QuirksMode,
    ) -> InvalidationKind
    where
        E: TElement,
    {
        debug_assert!(!self.fully_invalid);

        let mut kind = InvalidationKind::None;

        if !self.buckets.classes.is_empty() {
            element.each_class(|c| {
                kind.add(self.buckets.classes.get(c, quirks_mode));
            });

            if kind.is_scope() {
                return kind;
            }

            if let Some(snapshot) = snapshot {
                snapshot.each_class(|c| {
                    kind.add(self.buckets.classes.get(c, quirks_mode));
                });

                if kind.is_scope() {
                    return kind;
                }
            }
        }

        if !self.buckets.ids.is_empty() {
            if let Some(ref id) = element.id() {
                kind.add(self.buckets.ids.get(id, quirks_mode));
                if kind.is_scope() {
                    return kind;
                }
            }

            if let Some(ref old_id) = snapshot.and_then(|s| s.id_attr()) {
                kind.add(self.buckets.ids.get(old_id, quirks_mode));
                if kind.is_scope() {
                    return kind;
                }
            }
        }

        if !self.buckets.local_names.is_empty() {
            kind.add(self.buckets.local_names.get(element.local_name()));
        }

        kind
    }

    /// Clears the invalidation set without processing.
    pub fn clear(&mut self) {
        self.buckets.clear();
        self.fully_invalid = false;
        debug_assert!(self.is_empty());
    }

    fn process_invalidations<E>(&self, element: E, snapshots: Option<&SnapshotMap>) -> bool
    where
        E: TElement,
    {
        debug!("Stylist::process_invalidations({:?}, {:?})", element, self);

        {
            let mut data = match element.mutate_data() {
                Some(data) => data,
                None => return false,
            };

            if self.fully_invalid {
                debug!("process_invalidations: fully_invalid({:?})", element);
                data.hint.insert(RestyleHint::restyle_subtree());
                return true;
            }
        }

        if self.is_empty() {
            debug!("process_invalidations: empty invalidation set");
            return false;
        }

        let quirks_mode = element.as_node().owner_doc().quirks_mode();
        self.process_invalidations_in_subtree(element, snapshots, quirks_mode)
    }

    /// Process style invalidations in a given subtree. This traverses the
    /// subtree looking for elements that match the invalidations in our hash
    /// map members.
    ///
    /// Returns whether it invalidated at least one element's style.
    #[allow(unsafe_code)]
    fn process_invalidations_in_subtree<E>(
        &self,
        element: E,
        snapshots: Option<&SnapshotMap>,
        quirks_mode: QuirksMode,
    ) -> bool
    where
        E: TElement,
    {
        debug!("process_invalidations_in_subtree({:?})", element);
        let mut data = match element.mutate_data() {
            Some(data) => data,
            None => return false,
        };

        if !data.has_styles() {
            return false;
        }

        if data.hint.contains_subtree() {
            debug!(
                "process_invalidations_in_subtree: {:?} was already invalid",
                element
            );
            return false;
        }

        let element_wrapper = snapshots.map(|s| ElementWrapper::new(element, s));
        let snapshot = element_wrapper.as_ref().and_then(|e| e.snapshot());

        match self.invalidation_kind_for(element, snapshot, quirks_mode) {
            InvalidationKind::None => {},
            InvalidationKind::Element => {
                debug!(
                    "process_invalidations_in_subtree: {:?} matched self",
                    element
                );
                data.hint.insert(RestyleHint::RESTYLE_SELF);
            },
            InvalidationKind::Scope => {
                debug!(
                    "process_invalidations_in_subtree: {:?} matched subtree",
                    element
                );
                data.hint.insert(RestyleHint::restyle_subtree());
                return true;
            },
        }

        let mut any_children_invalid = false;

        for child in element.traversal_children() {
            let child = match child.as_element() {
                Some(e) => e,
                None => continue,
            };

            any_children_invalid |=
                self.process_invalidations_in_subtree(child, snapshots, quirks_mode);
        }

        if any_children_invalid {
            debug!(
                "Children of {:?} changed, setting dirty descendants",
                element
            );
            unsafe { element.set_dirty_descendants() }
        }

        data.hint.contains(RestyleHint::RESTYLE_SELF) || any_children_invalid
    }

    /// TODO(emilio): Reuse the bucket stuff from selectormap? That handles
    /// :is() / :where() etc.
    fn scan_component(
        component: &Component<SelectorImpl>,
        invalidation: &mut Option<Invalidation>,
    ) {
        match *component {
            Component::LocalName(LocalName {
                ref name,
                ref lower_name,
            }) => {
                if invalidation.is_none() {
                    *invalidation = Some(Invalidation::LocalName {
                        name: name.clone(),
                        lower_name: lower_name.clone(),
                    });
                }
            },
            Component::Class(ref class) => {
                if invalidation.as_ref().map_or(true, |s| !s.is_id_or_class()) {
                    *invalidation = Some(Invalidation::Class(class.clone()));
                }
            },
            Component::ID(ref id) => {
                if invalidation.as_ref().map_or(true, |s| !s.is_id()) {
                    *invalidation = Some(Invalidation::ID(id.clone()));
                }
            },
            _ => {
                // Ignore everything else, at least for now.
            },
        }
    }

    /// Collect invalidations for a given selector.
    ///
    /// We look at the outermost local name, class, or ID selector to the left
    /// of an ancestor combinator, in order to restyle only a given subtree.
    ///
    /// If the selector has no ancestor combinator, then we do the same for
    /// the only sequence it has, but record it as an element invalidation
    /// instead of a subtree invalidation.
    ///
    /// We prefer IDs to classs, and classes to local names, on the basis
    /// that the former should be more specific than the latter. We also
    /// prefer to generate subtree invalidations for the outermost part
    /// of the selector, to reduce the amount of traversal we need to do
    /// when flushing invalidations.
    fn collect_invalidations(
        &mut self,
        selector: &Selector<SelectorImpl>,
        quirks_mode: QuirksMode,
    ) {
        debug!(
            "StylesheetInvalidationSet::collect_invalidations({:?})",
            selector
        );

        let mut element_invalidation: Option<Invalidation> = None;
        let mut subtree_invalidation: Option<Invalidation> = None;

        let mut scan_for_element_invalidation = true;
        let mut scan_for_subtree_invalidation = false;

        let mut iter = selector.iter();

        loop {
            for component in &mut iter {
                if scan_for_element_invalidation {
                    Self::scan_component(component, &mut element_invalidation);
                } else if scan_for_subtree_invalidation {
                    Self::scan_component(component, &mut subtree_invalidation);
                }
            }
            match iter.next_sequence() {
                None => break,
                Some(combinator) => {
                    scan_for_subtree_invalidation = combinator.is_ancestor();
                },
            }
            scan_for_element_invalidation = false;
        }

        if let Some(s) = subtree_invalidation {
            debug!(" > Found subtree invalidation: {:?}", s);
            if self.insert_invalidation(s, InvalidationKind::Scope, quirks_mode) {
                return;
            }
        }

        if let Some(s) = element_invalidation {
            debug!(" > Found element invalidation: {:?}", s);
            if self.insert_invalidation(s, InvalidationKind::Element, quirks_mode) {
                return;
            }
        }

        // The selector was of a form that we can't handle. Any element could
        // match it, so let's just bail out.
        debug!(" > Can't handle selector or OOMd, marking fully invalid");
        self.invalidate_fully()
    }

    fn insert_invalidation(
        &mut self,
        invalidation: Invalidation,
        kind: InvalidationKind,
        quirks_mode: QuirksMode,
    ) -> bool {
        match invalidation {
            Invalidation::Class(c) => {
                let entry = match self.buckets.classes.try_entry(c.0, quirks_mode) {
                    Ok(e) => e,
                    Err(..) => return false,
                };
                *entry.or_insert(InvalidationKind::None) |= kind;
            },
            Invalidation::ID(i) => {
                let entry = match self.buckets.ids.try_entry(i.0, quirks_mode) {
                    Ok(e) => e,
                    Err(..) => return false,
                };
                *entry.or_insert(InvalidationKind::None) |= kind;
            },
            Invalidation::LocalName { name, lower_name } => {
                let insert_lower = name != lower_name;
                if self.buckets.local_names.try_reserve(1).is_err() {
                    return false;
                }
                let entry = self.buckets.local_names.entry(name);
                *entry.or_insert(InvalidationKind::None) |= kind;
                if insert_lower {
                    if self.buckets.local_names.try_reserve(1).is_err() {
                        return false;
                    }
                    let entry = self.buckets.local_names.entry(lower_name);
                    *entry.or_insert(InvalidationKind::None) |= kind;
                }
            },
        }

        true
    }

    /// Collects invalidations for a given CSS rule, if not fully invalid
    /// already.
    ///
    /// TODO(emilio): we can't check whether the rule is inside a non-effective
    /// subtree, we potentially could do that.
    pub fn rule_changed<S>(
        &mut self,
        stylesheet: &S,
        rule: &CssRule,
        guard: &SharedRwLockReadGuard,
        device: &Device,
        quirks_mode: QuirksMode,
        change_kind: RuleChangeKind,
    ) where
        S: StylesheetInDocument,
    {
        debug!("StylesheetInvalidationSet::rule_changed");
        if self.fully_invalid {
            return;
        }

        if !stylesheet.enabled() || !stylesheet.is_effective_for_device(device, guard) {
            debug!(" > Stylesheet was not effective");
            return; // Nothing to do here.
        }

        // If the change is generic, we don't have the old rule information to know e.g., the old
        // media condition, or the old selector text, so we might need to invalidate more
        // aggressively. That only applies to the changed rules, for other rules we can just
        // collect invalidations as normal.
        let is_generic_change = change_kind == RuleChangeKind::Generic;
        self.collect_invalidations_for_rule(rule, guard, device, quirks_mode, is_generic_change);
        if self.fully_invalid {
            return;
        }

        if !is_generic_change && !EffectiveRules::is_effective(guard, device, quirks_mode, rule) {
            return;
        }

        let rules = EffectiveRulesIterator::effective_children(device, quirks_mode, guard, rule);
        for rule in rules {
            self.collect_invalidations_for_rule(
                rule,
                guard,
                device,
                quirks_mode,
                /* is_generic_change = */ false,
            );
            if self.fully_invalid {
                break;
            }
        }
    }

    /// Collects invalidations for a given CSS rule.
    fn collect_invalidations_for_rule(
        &mut self,
        rule: &CssRule,
        guard: &SharedRwLockReadGuard,
        device: &Device,
        quirks_mode: QuirksMode,
        is_generic_change: bool,
    ) {
        use crate::stylesheets::CssRule::*;
        debug!("StylesheetInvalidationSet::collect_invalidations_for_rule");
        debug_assert!(!self.fully_invalid, "Not worth being here!");

        match *rule {
            Style(ref lock) => {
                if is_generic_change {
                    // TODO(emilio): We need to do this for selector / keyframe
                    // name / font-face changes, because we don't have the old
                    // selector / name.  If we distinguish those changes
                    // specially, then we can at least use this invalidation for
                    // style declaration changes.
                    return self.invalidate_fully();
                }

                let style_rule = lock.read_with(guard);
                for selector in style_rule.selectors.slice() {
                    self.collect_invalidations(selector, quirks_mode);
                    if self.fully_invalid {
                        return;
                    }
                }
            },
            NestedDeclarations(..) => {
                // Our containing style rule would handle invalidation for us.
            },
            Namespace(..) => {
                // It's not clear what handling changes for this correctly would
                // look like.
            },
            LayerStatement(..) => {
                // Layer statement insertions might alter styling order, so we need to always
                // invalidate fully.
                return self.invalidate_fully();
            },
            Document(..) | Import(..) | Media(..) | Supports(..) | Container(..) |
            LayerBlock(..) | StartingStyle(..) => {
                // Do nothing, relevant nested rules are visited as part of rule iteration.
            },
            FontFace(..) => {
                // Do nothing, @font-face doesn't affect computed style information on it's own.
                // We'll restyle when the font face loads, if needed.
            },
            Page(..) | Margin(..) => {
                // Do nothing, we don't support OM mutations on print documents, and page rules
                // can't affect anything else.
            },
            Keyframes(ref lock) => {
                if is_generic_change {
                    return self.invalidate_fully();
                }
                let keyframes_rule = lock.read_with(guard);
                if device.animation_name_may_be_referenced(&keyframes_rule.name) {
                    debug!(
                        " > Found @keyframes rule potentially referenced \
                         from the page, marking the whole tree invalid."
                    );
                    self.invalidate_fully();
                } else {
                    // Do nothing, this animation can't affect the style of existing elements.
                }
            },
            CounterStyle(..) | Property(..) | FontFeatureValues(..) | FontPaletteValues(..) => {
                debug!(" > Found unsupported rule, marking the whole subtree invalid.");
                self.invalidate_fully();
            },
            Scope(..) => {
                // Addition/removal of @scope requires re-evaluation of scope proximity to properly
                // figure out the styling order.
                self.invalidate_fully();
            },
            PositionTry(..) => {
                // Potential change in sizes/positions of anchored elements. TODO(dshin, bug 1910616):
                // We should probably make an effort to see if this position-try is referenced.
                self.invalidate_fully();
            },
        }
    }
}
