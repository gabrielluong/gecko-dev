/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Documentation.  Code starts about 670 lines down from here.               //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

// [SMDOC] An overview of Ion's register allocator
//
// The intent of this documentation is to give maintainers a map with which to
// navigate the allocator.  As further understanding is obtained, it should be
// added to this overview.
//
// Where possible, invariants are stated and are marked "(INVAR)".  Many
// details are omitted because their workings are currently unknown.  In
// particular, this overview doesn't explain how Intel-style "modify" (tied)
// operands are handled.  Facts or invariants that are speculative -- believed
// to be true, but not verified at the time of writing -- are marked "(SPEC)".
//
// The various concepts are interdependent, so a single forwards reading of the
// following won't make much sense.  Many concepts are explained only after
// they are mentioned.
//
// Where possible examples are shown.  Without those the description is
// excessively abstract.
//
// Names of the form ::name mean BacktrackingAllocator::name.
//
// The description falls into two sections:
//
// * Section 1: A tour of the data structures
// * Section 2: The core allocation loop, and bundle splitting
//
// The allocator sometimes produces poor allocations, with excessive spilling
// and register-to-register moves (bugs 1752520, bug 1714280 bug 1746596).
// Work in bug 1752582 shows we can get better quality allocations from this
// framework without having to make any large (conceptual) changes, by having
// better splitting heuristics.
//
// At https://bugzilla.mozilla.org/show_bug.cgi?id=1758274#c17
// (https://bugzilla.mozilla.org/attachment.cgi?id=9288467) is a document
// written at the same time as these comments.  It describes some improvements
// we could make to our splitting heuristics, particularly in the presence of
// loops and calls, and shows why the current implementation sometimes produces
// excessive spilling.  It builds on the commentary in this SMDOC.
//
//
// Top level pipeline
// ~~~~~~~~~~~~~~~~~~
// There are three major phases in allocation.  They run sequentially, at a
// per-function granularity.
//
// (1) Liveness analysis and bundle formation
// (2) Bundle allocation and last-chance allocation
// (3) Rewriting the function to create MoveGroups and to "install"
//     the allocation
//
// The input language (LIR) is in SSA form, and phases (1) and (3) depend on
// that SSAness.  Without it the allocator wouldn't work.
//
// The top level function is ::go.  The phases are divided into functions as
// follows:
//
// (1) ::buildLivenessInfo, ::mergeAndQueueRegisters
// (2) ::processBundle, ::tryAllocatingRegistersForSpillBundles,
//     ::pickStackSlots
// (3) ::createMoveGroupsFromLiveRangeTransitions, ::installAllocationsInLIR,
//     ::populateSafepoints, ::annotateMoveGroups
//
// The code in this file is structured as much as possible in the same sequence
// as flow through the pipeline.  Hence, top level function ::go is right at
// the end.  Where a function depends on helper function(s), the helpers appear
// first.
//
//
// ========================================================================
// ====                                                                ====
// ==== Section 1: A tour of the data structures                       ====
// ====                                                                ====
// ========================================================================
//
// Here are the key data structures necessary for understanding what follows.
//
// Some basic data structures
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// CodePosition
// ------------
// A CodePosition is an unsigned 32-bit int that indicates an instruction index
// in the incoming LIR.  Each LIR actually has two code positions, one to
// denote the "input" point (where, one might imagine, the operands are read,
// at least useAtStart ones) and the "output" point, where operands are
// written.  Eg:
//
//    Block 0 [successor 2] [successor 1]
//      2-3 WasmParameter [def v1<g>:r14]
//      4-5 WasmCall [use v1:F:r14]
//      6-7 WasmLoadTls [def v2<o>] [use v1:R]
//      8-9 WasmNullConstant [def v3<o>]
//      10-11 Compare [def v4<i>] [use v2:R] [use v3:A]
//      12-13 TestIAndBranch [use v4:R]
//
// So for example the WasmLoadTls insn has its input CodePosition as 6 and
// output point as 7.  Input points are even numbered, output points are odd
// numbered.  CodePositions 0 and 1 never appear, because LIR instruction IDs
// start at 1.  Indeed, CodePosition 0 is assumed to be invalid and hence is
// used as a marker for "unusual conditions" in various places.
//
// Phi nodes exist in the instruction stream too.  They always appear at the
// start of blocks (of course) (SPEC), but their start and end points are
// printed for the group as a whole.  This is to emphasise that they are really
// parallel assignments and that printing them sequentially would misleadingly
// imply that they are executed sequentially.  Example:
//
//    Block 6 [successor 7] [successor 8]
//      56-59 Phi [def v19<o>] [use v2:A] [use v5:A] [use v13:A]
//      56-59 Phi [def v20<o>] [use v7:A] [use v14:A] [use v12:A]
//      60-61 WasmLoadSlot [def v21<o>] [use v1:R]
//      62-63 Compare [def v22<i>] [use v20:R] [use v21:A]
//      64-65 TestIAndBranch [use v22:R]
//
// See that both Phis are printed with limits 56-59, even though they are
// stored in the LIR array like regular LIRs and so have code points 56-57 and
// 58-59 in reality.
//
// The process of allocation adds MoveGroup LIRs to the function.  Each
// incoming LIR has its own private list of MoveGroups (actually, 3 lists; two
// for moves that conceptually take place before the instruction, and one for
// moves after it).  Hence the CodePositions for LIRs (the "62-63", etc, above)
// do not change as a result of allocation.
//
// Virtual registers (vregs) in LIR
// --------------------------------
// The MIR from which the LIR is derived, is standard SSA.  That SSAness is
// carried through into the LIR (SPEC).  In the examples here, LIR SSA names
// (virtual registers, a.k.a. vregs) are printed as "v<number>".  v0 never
// appears and is presumed to be a special value, perhaps "invalid" (SPEC).
//
// The allocator core has a type VirtualRegister, but this is private to the
// allocator and not part of the LIR.  It carries far more information than
// merely the name of the vreg.  The allocator creates one VirtualRegister
// structure for each vreg in the LIR.
//
// LDefinition and LUse
// --------------------
// These are part of the incoming LIR.  Each LIR instruction defines zero or
// more values, and contains one LDefinition for each defined value (SPEC).
// Each instruction has zero or more input operands, each of which has its own
// LUse (SPEC).
//
// Both LDefinition and LUse hold both a virtual register name and, in general,
// a real (physical) register identity.  The incoming LIR has the real register
// fields unset, except in places where the incoming LIR has fixed register
// constraints (SPEC).  Phase 3 of allocation will visit all of the
// LDefinitions and LUses so as to write into the real register fields the
// decisions made by the allocator.  For LUses, this is done by overwriting the
// complete LUse with a different LAllocation, for example LStackSlot.  That's
// possible because LUse is a child class of LAllocation.
//
// This action of reading and then later updating LDefinition/LUses is the core
// of the allocator's interface to the outside world.
//
// To make visiting of LDefinitions/LUses possible, the allocator doesn't work
// with LDefinition and LUse directly.  Rather it has pointers to them
// (VirtualRegister::def_, UsePosition::use_).  Hence Phase 3 can modify the
// LIR in-place.
//
// (INVARs, all SPEC):
//
// - The collective VirtualRegister::def_ values should be unique, and there
//   should be a 1:1 mapping between the VirtualRegister::def_ values and the
//   LDefinitions in the LIR.  (So that the LIR LDefinition has exactly one
//   VirtualRegister::def_ to track it).  But only for the valid LDefinitions.
//   If isBogusTemp() is true, the definition is invalid and doesn't have a
//   vreg.
//
// - The same for uses: there must be a 1:1 correspondence between the
//   CodePosition::use_ values and the LIR LUses.
//
// - The allocation process must preserve these 1:1 mappings.  That implies
//   (weaker) that the number of VirtualRegisters and of UsePositions must
//   remain constant through allocation.  (Eg: losing them would mean that some
//   LIR def or use would necessarily not get annotated with its final
//   allocation decision.  Duplicating them would lead to the possibility of
//   conflicting allocation decisions.)
//
// Other comments regarding LIR
// ----------------------------
// The incoming LIR is structured into basic blocks and a CFG, as one would
// expect.  These (insns, block boundaries, block edges etc) are available
// through the BacktrackingAllocator object.  They are important for Phases 1
// and 3 but not for Phase 2.
//
// Phase 3 "rewrites" the input LIR so as to "install" the final allocation.
// It has to insert MoveGroup instructions, but that isn't done by pushing them
// into the instruction array.  Rather, each LIR has 3 auxiliary sets of
// MoveGroups (SPEC): two that "happen" conceptually before the LIR, and one
// that happens after it.  The rewriter inserts MoveGroups into one of these 3
// sets, and later code generation phases presumably insert the sets (suitably
// translated) into the final machine code (SPEC).
//
//
// Key data structures: LiveRange, VirtualRegister and LiveBundle
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// These three have central roles in allocation.  Of them, LiveRange is the
// most central.  VirtualRegister is conceptually important throughout, but
// appears less frequently in the allocator code.  LiveBundle is important only
// in Phase 2 (where it is central) and at the end of Phase 1, but plays no
// role in Phase 3.
//
// It's important to understand that LiveRange and VirtualRegister correspond
// to concepts visible in the incoming LIR, which is in SSA form.  LiveBundle
// by comparison is related to neither the structure of LIR nor its SSA
// properties.  Instead, LiveBundle is an essentially adminstrative structure
// used to accelerate allocation and to implement a crude form of
// move-coalescing.
//
// VirtualRegisters and LiveRanges are (almost) static throughout the process,
// because they reflect aspects of the incoming LIR, which does not change.
// LiveBundles by contrast come and go; they are created, but may be split up
// into new bundles, and old ones abandoned.
//
// Each LiveRange is a member of two different lists.
//
// A VirtualRegister (described in detail below) has a vector of LiveRanges that
// it "owns".
//
// A LiveBundle (also described below) has a linked list of LiveRanges that it
// "owns".
//
// Hence each LiveRange is "owned" by one VirtualRegister and one LiveBundle.
// LiveRanges may have their owning LiveBundle changed as a result of
// splitting.  By contrast a LiveRange stays with its "owning" VirtualRegister
// for ever.
//
// A few LiveRanges have no VirtualRegister.  This is used to implement
// register spilling for calls.  Each physical register that's not preserved
// across a call has a small range that covers the call.  It is
// ::buildLivenessInfo that adds these small ranges.
//
// Iterating over every VirtualRegister in the system is a common operation and
// is straightforward because (somewhat redundantly?) the LIRGraph knows the
// number of vregs, and more importantly because BacktrackingAllocator::vregs
// is a vector of all VirtualRegisters.  By contrast iterating over every
// LiveBundle in the system is more complex because there is no top-level
// registry of them.  It is still possible though.  See ::dumpLiveRangesByVReg
// and ::dumpLiveRangesByBundle for example code.
//
// LiveRange
// ---------
// Fundamentally, a LiveRange (often written just "range") is a request for
// storage of a LIR vreg for some contiguous sequence of LIRs.  A LiveRange
// generally covers only a fraction of a vreg's overall lifetime, so multiple
// LiveRanges are generally needed to cover the whole lifetime.
//
// A LiveRange contains (amongst other things):
//
// * the vreg for which it is for, as a VirtualRegister*
//
// * the range of CodePositions for which it is for, as a LiveRange::Range
//
// * auxiliary information:
//
//   - a boolean that indicates whether this LiveRange defines a value for the
//     vreg.  If so, that definition is regarded as taking place at the first
//     CodePoint of the range.
//
//   - a linked list of uses of the vreg within this range.  Each use is a pair
//     of a CodePosition and an LUse*.  (INVAR): the uses are maintained in
//     increasing order of CodePosition.  Multiple uses at the same
//     CodePosition are permitted, since that is necessary to represent an LIR
//     that uses the same vreg in more than one of its operand slots.
//
// Some important facts about LiveRanges are best illustrated with examples:
//
//   v25 75-82 { 75_def:R 78_v25:A 82_v25:A }
//
// This LiveRange is for vreg v25.  The value is defined at CodePosition 75,
// with the LIR requiring that it be in a register.  It is used twice at
// positions 78 and 82, both with no constraints (A meaning "any").  The range
// runs from position 75 to 82 inclusive.  Note however that LiveRange::Range
// uses non-inclusive range ends; hence its .to field would be 83, not 82.
//
//   v26 84-85 { 85_v26:R }
//
// This LiveRange is for vreg v26.  Here, there's only a single use of it at
// position 85.  Presumably it is defined in some other LiveRange.
//
//   v19 74-79 { }
//
// This LiveRange is for vreg v19.  There is no def and no uses, so at first
// glance this seems redundant.  But it isn't: it still expresses a request for
// storage for v19 across 74-79, because Phase 1 regards v19 as being live in
// this range (meaning: having a value that, if changed in this range, would
// cause the program to fail).
//
// Other points:
//
// * (INVAR) Each vreg/VirtualRegister has at least one LiveRange.
//
// * (INVAR) Exactly one LiveRange of a vreg gives a definition for the value.
//   All other LiveRanges must consist only of uses (including zero uses, for a
//   "flow-though" range as mentioned above).  This requirement follows from
//   the fact that LIR is in SSA form.
//
// * It follows from this, that the LiveRanges for a VirtualRegister must form
//   a tree, where the parent-child relationship is "control flows directly
//   from a parent LiveRange (anywhere in the LiveRange) to a child LiveRange
//   (start)".  The entire tree carries only one value.  This is a use of
//   SSAness in the allocator which is fundamental: without SSA input, this
//   design would not work.
//
//   The root node (LiveRange) in the tree must be one that defines the value,
//   and all other nodes must only use or be flow-throughs for the value.  It's
//   OK for LiveRanges in the tree to overlap, providing that there is a unique
//   root node -- otherwise it would be unclear which LiveRange provides the
//   value.
//
//   The function ::createMoveGroupsFromLiveRangeTransitions runs after all
//   LiveBundles have been allocated.  It visits each VirtualRegister tree in
//   turn.  For every parent->child edge in a tree, it creates a MoveGroup that
//   copies the value from the parent into the child -- this is how the
//   allocator decides where to put MoveGroups.  There are various other
//   details not described here.
//
// * It's important to understand that a LiveRange carries no meaning about
//   control flow beyond that implied by the SSA (hence, dominance)
//   relationship between a def and its uses.  In particular, there's no
//   implication that execution "flowing into" the start of the range implies
//   that it will "flow out" of the end.  Or that any particular use will or
//   will not be executed.
//
// * (very SPEC) Indeed, even if a range has a def, there's no implication that
//   a use later in the range will have been immediately preceded by execution
//   of the def.  It could be that the def is executed, flow jumps somewhere
//   else, and later jumps back into the middle of the range, where there are
//   then some uses.
//
// * Uses of a vreg by a phi node are not mentioned in the use list of a
//   LiveRange.  The reasons for this are unknown, but it is speculated that
//   this is because we don't need to know about phi uses where we use the list
//   of positions.  See comments on VirtualRegister::usedByPhi_.
//
// * Similarly, a definition of a vreg by a phi node is not regarded as being a
//   definition point (why not?), at least as the view of
//   LiveRange::hasDefinition_.
//
// * LiveRanges that nevertheless include a phi-defined value have their first
//   point set to the first of the block of phis, even if the var isn't defined
//   by that specific phi.  Eg:
//
//    Block 6 [successor 7] [successor 8]
//      56-59 Phi [def v19<o>] [use v2:A] [use v5:A] [use v13:A]
//      56-59 Phi [def v20<o>] [use v7:A] [use v14:A] [use v12:A]
//      60-61 WasmLoadSlot [def v21<o>] [use v1:R]
//      62-63 Compare [def v22<i>] [use v20:R] [use v21:A]
//
//   The relevant live range for v20 is
//
//    v20 56-65 { 63_v20:R }
//
//   Observe that it starts at 56, not 58.
//
// VirtualRegister
// ---------------
// Each VirtualRegister is associated with an SSA value created by the LIR.
// Fundamentally it is a container to hold all of the LiveRanges that together
// indicate where the value must be kept live.  The live ranges are stored in a
// vector, VirtualRegister::ranges_.  The set of LiveRanges must logically form
// a tree, rooted at the LiveRange which defines the value.
//
// The live ranges are sorted in order of decreasing start point by construction
// after buildLivenessInfo until the main register allocation loops.  At that
// point they can become unsorted and this is important for performance because
// it allows the core allocation code to add/remove ranges more efficiently.
// After all bundles are allocated, sortVirtualRegisterRanges is called to
// ensure the ranges are sorted again.
//
// There are various auxiliary fields, most importantly the LIR node and the
// associated LDefinition that define the value.
//
// It is OK, and quite common, for LiveRanges of a VirtualRegister to overlap.
// The effect will be that, in an overlapped area, there are two storage
// locations holding the value.  This is OK -- although wasteful of storage
// resources -- because the SSAness means the value must be the same in both
// locations.  Hence there's no questions like "which LiveRange holds the most
// up-to-date value?", since it's all just one value anyway.
//
// Note by contrast, it is *not* OK for the LiveRanges of a LiveBundle to
// overlap.
//
// LiveBundle
// ----------
// Similar to VirtualRegister, a LiveBundle is also, fundamentally, a container
// for a set of LiveRanges.  The set is stored as a linked list, rooted at
// LiveBundle::ranges_.
//
// However, the similarity ends there:
//
// * The LiveRanges in a LiveBundle absolutely must not overlap.  They must
//   indicate disjoint sets of CodePositions, and must be stored in the list in
//   order of increasing CodePosition.  Because of the no-overlap requirement,
//   these ranges form a total ordering regardless of whether one uses the
//   LiveRange::Range::from_ or ::to_ fields for comparison.
//
// * The LiveRanges in a LiveBundle can otherwise be entirely arbitrary and
//   unrelated.  They can be from different VirtualRegisters and can have no
//   particular mutual significance w.r.t. the SSAness or structure of the
//   input LIR.
//
// LiveBundles are the fundamental unit of allocation.  The allocator attempts
// to find a single storage location that will work for all LiveRanges in the
// bundle.  That's why the ranges must not overlap.  If no such location can be
// found, the allocator may decide to split the bundle into multiple smaller
// bundles.  Each of those may be allocated independently.
//
// The other really important field is LiveBundle::alloc_, indicating the
// chosen storage location.
//
// Here's an example, for a LiveBundle before it has been allocated:
//
//   LB2(parent=none v3 8-21 { 16_v3:A } ## v3 24-25 { 25_v3:F:xmm0 })
//
// LB merely indicates "LiveBundle", and the 2 is the id_ value (see
// below).  This bundle has two LiveRanges
//
//   v3 8-21 { 16_v3:A }
//   v3 24-25 { 25_v3:F:xmm0 }
//
// both of which (coincidentally) are for the same VirtualRegister, v3.The
// second LiveRange has a fixed use in `xmm0`, whilst the first one doesn't
// care (A meaning "any location") so the allocator *could* choose `xmm0` for
// the bundle as a whole.
//
// One might ask: why bother with LiveBundle at all?  After all, it would be
// possible to get correct allocations by allocating each LiveRange
// individually, then leaving ::createMoveGroupsFromLiveRangeTransitions to add
// MoveGroups to join up LiveRanges that form each SSA value tree (that is,
// LiveRanges belonging to each VirtualRegister).
//
// There are two reasons:
//
// (1) By putting multiple LiveRanges into each LiveBundle, we can end up with
//     many fewer LiveBundles than LiveRanges.  Since the cost of allocating a
//     LiveBundle is substantially less than the cost of allocating each of its
//     LiveRanges individually, the allocator will run faster.
//
// (2) It provides a crude form of move-coalescing.  There are situations where
//     it would be beneficial, although not mandatory, to have two LiveRanges
//     assigned to the same storage unit.  Most importantly: (a) LiveRanges
//     that form all of the inputs, and the output, of a phi node.  (b)
//     LiveRanges for the output and first-operand values in the case where we
//     are targetting Intel-style instructions.
//
//     In such cases, if the bundle can be allocated as-is, then no extra moves
//     are necessary.  If not (and the bundle is split), then
//     ::createMoveGroupsFromLiveRangeTransitions will later fix things up by
//     inserting MoveGroups in the right places.
//
// Merging of LiveRanges into LiveBundles is done in Phase 1, by
// ::mergeAndQueueRegisters, after liveness analysis (which generates only
// LiveRanges).
//
// For the bundle mentioned above, viz
//
//   LB2(parent=none v3 8-21 { 16_v3:A } ## v3 24-25 { 25_v3:F:xmm0 })
//
// the allocator did not in the end choose to allocate it to `xmm0`.  Instead
// it was split into two bundles, LB6 (a "spill parent", or root node in the
// trees described above), and LB9, a leaf node, that points to its parent LB6:
//
//   LB6(parent=none v3 8-21 %xmm1.s { 16_v3:A } ## v3 24-25 %xmm1.s { })
//   LB9(parent=LB6 v3 24-25 %xmm0.s { 25_v3:F:rax })
//
// Note that both bundles now have an allocation, and that is printed,
// redundantly, for each LiveRange in the bundle -- hence the repeated
// `%xmm1.s` in the lines above.  Since all LiveRanges in a LiveBundle must be
// allocated to the same storage location, we never expect to see output like
// this
//
//   LB6(parent=none v3 8-21 %xmm1.s { 16_v3:A } ## v3 24-25 %xmm2.s { })
//
// and that is in any case impossible, since a LiveRange doesn't have an
// LAllocation field.  Instead it has a pointer to its LiveBundle, and the
// LAllocation lives in the LiveBundle.
//
// For the resulting allocation (LB6 and LB9), all the LiveRanges are use-only
// or flow-through.  There are no definitions.  But they are all for the same
// VirtualReg, v3, so they all have the same value.  An important question is
// where they "get their value from".  The answer is that
// ::createMoveGroupsFromLiveRangeTransitions will have to insert suitable
// MoveGroups so that each LiveRange for v3 can "acquire" the value from a
// previously-executed LiveRange, except for the range that defines it.  The
// defining LiveRange is not visible here; either it is in some other
// LiveBundle, not shown, or (more likely) the value is defined by a phi-node,
// in which case, as mentioned previously, it is not shown as having an
// explicit definition in any LiveRange.
//
// LiveBundles also have a `SpillSet* spill_` field (see below) and a
// `LiveBundle* spillParent_`.  The latter is used to ensure that all bundles
// originating from an "original" bundle share the same spill slot.  The
// `spillParent_` pointers can be thought of creating a 1-level tree, where
// each node points at its parent.  Since the tree can be only 1-level, the
// following invariant (INVAR) must be maintained:
//
// * if a LiveBundle has a non-null spillParent_ field, then it is a leaf node,
//   and no other LiveBundle can point at this one.
//
// * else (it has a null spillParent_ field) it is a root node, and so other
//   LiveBundles may point at it.
//
// LiveBundle has a 32-bit `id_` field. This is used for debug printing, and
// makes it easier to see parent-child relationships induced by the
// `spillParent_` pointers. It's also used for sorting live ranges in
// VirtualRegister::sortRanges.
//
// The "life cycle" of LiveBundles is described in Section 2 below.
//
//
// Secondary data structures: SpillSet, Requirement
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// SpillSet
// --------
// A SpillSet is a container for a set of LiveBundles that have been spilled,
// all of which are assigned the same spill slot.  The set is represented as a
// vector of points to LiveBundles.  SpillSet also contains the identity of the
// spill slot (its LAllocation).
//
// A LiveBundle, if it is to be spilled, has a pointer to the relevant
// SpillSet, and the SpillSet in turn has a pointer back to the LiveBundle in
// its vector thereof.  So LiveBundles (that are to be spilled) and SpillSets
// point at each other.
//
// (SPEC) LiveBundles that are not to be spilled (or for which the decision has
// yet to be made, have their SpillSet pointers as null.  (/SPEC)
//
// Requirement
// -----------
// Requirements are used transiently during the main allocation loop.  It
// summarises the set of constraints on storage location (must be any register,
// must be this specific register, must be stack, etc) for a LiveBundle.  This
// is so that the main allocation loop knows what kind of storage location it
// must choose in order to satisfy all of the defs and uses within the bundle.
//
// What Requirement provides is (a) a partially ordered set of locations, and
// (b) a constraint-merging method `merge`.
//
// Requirement needs a rewrite (and, in fact, that has already happened in
// un-landed code in bug 1758274) for the following reasons:
//
// * it's potentially buggy (bug 1761654), although that doesn't currently
//   affect us, for reasons which are unclear.
//
// * the partially ordered set has a top point, meaning "no constraint", but it
//   doesn't have a corresponding bottom point, meaning "impossible
//   constraints".  (So it's a partially ordered set, but not a lattice).  This
//   leads to awkward coding in some places, which would be simplified if there
//   were an explicit way to indicate "impossible constraint".
//
//
// Some ASCII art
// ~~~~~~~~~~~~~~
//
// Here's some not-very-good ASCII art that tries to summarise the data
// structures that persist for the entire allocation of a function:
//
//     BacktrackingAllocator
//        |
//      (vregs)
//        |
//        v
//        |
//     VirtualRegister -->--(ins)--> LNode
//     |            |  `->--(def)--> LDefinition
//     v            ^
//     |            |
// (ranges vector)  |
//     |            |
//     |          (vreg)
//     `--v->--.    |
//              \   |
//               LiveRange -->--b-->-- LiveRange -->--b-->-- NULL
//              /    |
//     ,--b->--'    /
//     |         (bundle)
//     ^          /
//     |         v
//  (ranges)    /
//     |       /
//     LiveBundle --s-->- LiveBundle
//       |      \           /    |
//       |       \         /     |
//      (spill)   ^       ^     (spill)
//       |         \     /       |
//       v          \   /        ^
//       |          (list)       |
//       \            |          /
//        `--->---> SpillSet <--'
//
// --b-- LiveRange::next_: links in the list of LiveRanges that belong to
//       a LiveBundle
//
// --v-- VirtualRegister::ranges_: vector of LiveRanges that belong to a
//       VirtualRegister
//
// --s-- LiveBundle::spillParent: a possible link to my "spill parent bundle"
//
//
// * LiveRange is in the center.  Each LiveRange is a member of a linked list,
//   the --b-- list, and is also stored in VirtualRegister's `ranges` vector.
//
// * VirtualRegister has a `ranges` vector that contains its LiveRanges.
//
// * LiveBundle has a pointer `ranges` that points to the start of its --b--
//   list of LiveRanges.
//
// * LiveRange points back at both its owning VirtualRegister (`vreg`) and its
//   owning LiveBundle (`bundle`).
//
// * LiveBundle has a pointer --s-- `spillParent`, which may be null, to its
//   conceptual "spill parent bundle", as discussed in detail above.
//
// * LiveBundle has a pointer `spill` to its SpillSet.
//
// * SpillSet has a vector `list` of pointers back to the LiveBundles that
//   point at it.
//
// * VirtualRegister has pointers `ins` to the LNode that defines the value and
//   `def` to the LDefinition within that LNode.
//
// * BacktrackingAllocator has a vector `vregs` of pointers to all the
//   VirtualRegisters for the function.  There is no equivalent top-level table
//   of all the LiveBundles for the function.
//
// Note that none of these pointers are "owning" in the C++-storage-management
// sense.  Rather, everything is stored in single arena which is freed when
// compilation of the function is complete.  For this reason,
// BacktrackingAllocator.{h,cpp} is almost completely free of the usual C++
// storage-management artefacts one would normally expect to see.
//
//
// ========================================================================
// ====                                                                ====
// ==== Section 2: The core allocation loop, and bundle splitting      ====
// ====                                                                ====
// ========================================================================
//
// Phase 1 of the allocator (described at the start of this SMDOC) computes
// live ranges, merges them into bundles, and places the bundles in a priority
// queue ::allocationQueue, ordered by what ::computePriority computes.
//
//
// The allocation loops
// ~~~~~~~~~~~~~~~~~~~~
// The core of the allocation machinery consists of two loops followed by a
// call to ::pickStackSlots.  The latter is uninteresting.  The two loops live
// in ::go and are documented in detail there.
//
//
// Bundle splitting
// ~~~~~~~~~~~~~~~~
// If the first of the two abovementioned loops cannot find a register for a
// bundle, either directly or as a result of evicting conflicting bundles, then
// it will have to either split or spill the bundle.  The entry point to the
// split/spill subsystem is ::chooseBundleSplit.  See comments there.

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// End of documentation                                                      //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "jit/BacktrackingAllocator.h"

#include "mozilla/BinarySearch.h"
#include "mozilla/Maybe.h"

#include <algorithm>

#include "jit/BitSet.h"
#include "jit/CompileInfo.h"
#include "js/Printf.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;
using mozilla::Maybe;

// This is a big, complex file.  Code is grouped into various sections, each
// preceded by a box comment.  Sections not marked as "Misc helpers" are
// pretty much the top level flow, and are presented roughly in the same order
// in which the allocation pipeline operates.  BacktrackingAllocator::go,
// right at the end of the file, is a good starting point.

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Misc helpers: linked-list management                                      //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

static inline bool SortBefore(UsePosition* a, UsePosition* b) {
  return a->pos <= b->pos;
}

static inline bool SortBefore(LiveRange* a, LiveRange* b) {
  MOZ_ASSERT(!a->intersects(b));
  return a->from() < b->from();
}

template <typename T>
static void InsertSortedList(InlineForwardList<T>& list, T* value,
                             T* startAt = nullptr) {
  if (list.empty()) {
    MOZ_ASSERT(!startAt);
    list.pushFront(value);
    return;
  }

#ifdef DEBUG
  if (startAt) {
    // `startAt` must be an element in `list` that sorts before `value`.
    MOZ_ASSERT(SortBefore(startAt, value));
    MOZ_ASSERT_IF(*list.begin() == list.back(), list.back() == startAt);
    MOZ_ASSERT_IF(startAt != *list.begin(), SortBefore(*list.begin(), startAt));
    MOZ_ASSERT_IF(startAt != list.back(), SortBefore(startAt, list.back()));
  }
#endif

  if (SortBefore(list.back(), value)) {
    list.pushBack(value);
    return;
  }

  // If `startAt` is non-nullptr, we can start iterating there.
  T* prev = nullptr;
  InlineForwardListIterator<T> iter =
      startAt ? list.begin(startAt) : list.begin();
  if (startAt) {
    // `value` must sort after `startAt` so skip to the next element.
    MOZ_ASSERT(!SortBefore(value, *iter));
    ++iter;
    prev = startAt;
  }
  for (; iter; iter++) {
    if (SortBefore(value, *iter)) {
      break;
    }
    prev = *iter;
  }

  if (prev) {
    list.insertAfter(prev, value);
  } else {
    list.pushFront(value);
  }
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Misc helpers: methods for class SpillSet                                  //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

void SpillSet::setAllocation(LAllocation alloc) {
  for (size_t i = 0; i < numSpilledBundles(); i++) {
    spilledBundle(i)->setAllocation(alloc);
  }
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Misc helpers: methods for class LiveRange                                 //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

static size_t SpillWeightFromUsePolicy(LUse::Policy policy) {
  switch (policy) {
    case LUse::ANY:
      return 1000;

    case LUse::REGISTER:
    case LUse::FIXED:
      return 2000;

    default:
      return 0;
  }
}

inline void LiveRange::noteAddedUse(UsePosition* use) {
  LUse::Policy policy = use->usePolicy();
  usesSpillWeight_ += SpillWeightFromUsePolicy(policy);
  if (policy == LUse::FIXED) {
    ++numFixedUses_;
  }
}

inline void LiveRange::noteRemovedUse(UsePosition* use) {
  LUse::Policy policy = use->usePolicy();
  usesSpillWeight_ -= SpillWeightFromUsePolicy(policy);
  if (policy == LUse::FIXED) {
    --numFixedUses_;
  }
  MOZ_ASSERT_IF(!hasUses(), !usesSpillWeight_ && !numFixedUses_);
}

void LiveRange::addUse(UsePosition* use) {
  MOZ_ASSERT(covers(use->pos));
  InsertSortedList(uses_, use);
  noteAddedUse(use);
}

UsePosition* LiveRange::popUse() {
  UsePosition* ret = uses_.popFront();
  noteRemovedUse(ret);
  return ret;
}

void LiveRange::tryToMoveDefAndUsesInto(LiveRange* other) {
  MOZ_ASSERT(&other->vreg() == &vreg());
  MOZ_ASSERT(this != other);

  // This method shouldn't be called for two non-intersecting live ranges
  // because it's a no-op in that case.
  MOZ_ASSERT(intersects(other));

  CodePosition otherFrom = other->from();
  CodePosition otherTo = other->to();

  // The uses are sorted by position, so first skip all uses before |other|
  // starts.
  UsePositionIterator iter = usesBegin();
  while (iter && iter->pos < otherFrom) {
    iter++;
  }

  // Move over all uses which fit in |other|'s boundaries.
  while (iter && iter->pos < otherTo) {
    UsePosition* use = *iter;
    MOZ_ASSERT(other->covers(use->pos));
    uses_.removeAndIncrement(iter);
    noteRemovedUse(use);
    other->addUse(use);
  }

  MOZ_ASSERT_IF(iter, !other->covers(iter->pos));

  // Distribute the definition to |other| as well, if possible.
  if (hasDefinition() && from() == other->from()) {
    other->setHasDefinition();
  }
}

void LiveRange::moveAllUsesToTheEndOf(LiveRange* other) {
  MOZ_ASSERT(&other->vreg() == &vreg());
  MOZ_ASSERT(this != other);
  MOZ_ASSERT(other->contains(this));

  if (uses_.empty()) {
    return;
  }

  // Assert |other->uses_| remains sorted after adding our uses at the end.
  MOZ_ASSERT_IF(!other->uses_.empty(),
                SortBefore(other->uses_.back(), *uses_.begin()));

  other->uses_.extendBack(std::move(uses_));
  MOZ_ASSERT(!hasUses());

  other->usesSpillWeight_ += usesSpillWeight_;
  other->numFixedUses_ += numFixedUses_;
  usesSpillWeight_ = 0;
  numFixedUses_ = 0;
}

bool LiveRange::contains(LiveRange* other) const {
  return from() <= other->from() && to() >= other->to();
}

void LiveRange::intersect(LiveRange* other, Range* pre, Range* inside,
                          Range* post) const {
  MOZ_ASSERT(pre->empty() && inside->empty() && post->empty());

  CodePosition innerFrom = from();
  if (from() < other->from()) {
    if (to() < other->from()) {
      *pre = range_;
      return;
    }
    *pre = Range(from(), other->from());
    innerFrom = other->from();
  }

  CodePosition innerTo = to();
  if (to() > other->to()) {
    if (from() >= other->to()) {
      *post = range_;
      return;
    }
    *post = Range(other->to(), to());
    innerTo = other->to();
  }

  if (innerFrom != innerTo) {
    *inside = Range(innerFrom, innerTo);
  }
}

bool LiveRange::intersects(LiveRange* other) const {
  Range pre, inside, post;
  intersect(other, &pre, &inside, &post);
  return !inside.empty();
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Misc helpers: methods for class LiveBundle                                //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#ifdef DEBUG
size_t LiveBundle::numRanges() const {
  size_t count = 0;
  for (RangeIterator iter = rangesBegin(); iter; iter++) {
    count++;
  }
  return count;
}
#endif

LiveRange* LiveBundle::rangeFor(CodePosition pos) const {
  for (RangeIterator iter = rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;
    if (range->covers(pos)) {
      return range;
    }
  }
  return nullptr;
}

void LiveBundle::addRange(LiveRange* range,
                          LiveRange* startAt /* = nullptr */) {
  MOZ_ASSERT(!range->bundle());
  MOZ_ASSERT(range->hasVreg());
  MOZ_ASSERT_IF(startAt, startAt->bundle() == this);
  range->setBundle(this);
  InsertSortedList(ranges_, range, startAt);
}

void LiveBundle::addRangeAtEnd(LiveRange* range) {
  // Note: this method is functionally equivalent to `addRange`, but can be used
  // when we know `range` must be added after all existing ranges in the bundle.
  MOZ_ASSERT(!range->bundle());
  MOZ_ASSERT(range->hasVreg());
  MOZ_ASSERT_IF(!ranges_.empty(), SortBefore(ranges_.back(), range));
  range->setBundle(this);
  ranges_.pushBack(range);
}

bool LiveBundle::addRangeAtEnd(TempAllocator& alloc, VirtualRegister* vreg,
                               CodePosition from, CodePosition to) {
  LiveRange* range = LiveRange::FallibleNew(alloc, vreg, from, to);
  if (!range) {
    return false;
  }
  addRangeAtEnd(range);
  return true;
}

bool LiveBundle::addRangeAndDistributeUses(TempAllocator& alloc,
                                           LiveRange* oldRange,
                                           CodePosition from, CodePosition to) {
  LiveRange* range = LiveRange::FallibleNew(alloc, &oldRange->vreg(), from, to);
  if (!range) {
    return false;
  }
  addRange(range);
  oldRange->tryToMoveDefAndUsesInto(range);
  return true;
}

LiveRange* LiveBundle::popFirstRange() {
  RangeIterator iter = rangesBegin();
  if (!iter) {
    return nullptr;
  }

  LiveRange* range = *iter;
  ranges_.removeAt(iter);

  range->setBundle(nullptr);
  return range;
}

void LiveBundle::removeRange(LiveRange* range) {
  for (RangeIterator iter = rangesBegin(); iter; iter++) {
    LiveRange* existing = *iter;
    if (existing == range) {
      ranges_.removeAt(iter);
      return;
    }
  }
  MOZ_CRASH();
}

void LiveBundle::removeAllRangesFromVirtualRegisters() {
  VirtualRegister* prevVreg = nullptr;
  for (RangeIterator iter = rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;
    MOZ_ASSERT(!range->hasUses());
    if (&range->vreg() != prevVreg) {
      // As an optimization, remove all ranges owned by this bundle from the
      // register's list, instead of a single range at a time.
      range->vreg().removeRangesForBundle(this);
      prevVreg = &range->vreg();
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Misc helpers: methods for class VirtualRegister                           //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

bool VirtualRegister::addInitialRange(TempAllocator& alloc, CodePosition from,
                                      CodePosition to) {
  MOZ_ASSERT(rangesSorted_, "ranges stay sorted during live range building");

  MOZ_ASSERT(from < to);
  MOZ_ASSERT_IF(hasRanges(), from < ranges_.back()->to());

  // Mark [from,to) as a live range for this register during the initial
  // liveness analysis, coalescing with any existing overlapping ranges.

  LiveRange* merged = nullptr;
  for (RangeIterator iter(*this); iter; iter++) {
    LiveRange* existing = *iter;

    MOZ_ASSERT(from < existing->to());

    if (to < existing->from()) {
      // All other ranges start after |to| so can't overlap.
      break;
    }

    if (!merged) {
      // This is the first old range we've found that overlaps the new
      // range. Extend this one to cover its union with the new range.
      merged = existing;

      if (from < existing->from()) {
        existing->setFrom(from);
      }
      if (to > existing->to()) {
        existing->setTo(to);
      }

      // Continue searching to see if any other old ranges can be
      // coalesced with the new merged range.
    } else {
      // Coalesce this range into the previous range we merged into.
      MOZ_ASSERT(existing->from() >= merged->from());
      if (existing->to() > merged->to()) {
        merged->setTo(existing->to());
      }

      MOZ_ASSERT(!existing->hasDefinition());
      existing->moveAllUsesToTheEndOf(merged);
      MOZ_ASSERT(!existing->hasUses());
    }

    removeFirstRange(iter);
  }

  if (merged) {
    // Re-append the merged range.
    if (!ranges_.append(merged)) {
      return false;
    }
  } else {
    // The new range does not overlap any existing range for the vreg.
    MOZ_ASSERT_IF(hasRanges(), to < ranges_.back()->from());

    LiveRange* range = LiveRange::FallibleNew(alloc, this, from, to);
    if (!range) {
      return false;
    }

    if (!ranges_.append(range)) {
      return false;
    }
  }

  MOZ_ASSERT(rangesSorted_, "ranges are still sorted");

#ifdef DEBUG
  // Check the last few ranges don't overlap and are in the correct order.
  size_t len = ranges_.length();
  static constexpr size_t MaxIterations = 4;
  size_t start = len > MaxIterations ? (len - MaxIterations) : 1;

  for (size_t i = start; i < len; i++) {
    LiveRange* range = ranges_[i];
    LiveRange* prev = ranges_[i - 1];
    MOZ_ASSERT(range->from() < range->to());
    MOZ_ASSERT(range->to() < prev->from());
  }
#endif

  return true;
}

void VirtualRegister::addInitialUse(UsePosition* use) {
  MOZ_ASSERT(rangesSorted_, "ranges stay sorted during live range building");
  ranges_.back()->addUse(use);
}

void VirtualRegister::setInitialDefinition(CodePosition from) {
  MOZ_ASSERT(rangesSorted_, "ranges stay sorted during live range building");
  LiveRange* first = ranges_.back();
  MOZ_ASSERT(from >= first->from());
  first->setFrom(from);
  first->setHasDefinition();
}

LiveRange* VirtualRegister::rangeFor(CodePosition pos,
                                     bool preferRegister /* = false */) const {
  assertRangesSorted();

  size_t len = ranges_.length();

  // Because the ranges are sorted in order of descending start position, we use
  // binary search to find the first range where |range->from <= pos|. All
  // ranges before that definitely don't cover |pos|.
  auto compare = [pos](LiveRange* other) {
    if (pos < other->from()) {
      return 1;
    }
    if (pos > other->from()) {
      return -1;
    }
    return 0;
  };
  size_t index;
  mozilla::BinarySearchIf(ranges_, 0, len, compare, &index);

  if (index == len) {
    // None of the ranges overlap.
    MOZ_ASSERT(ranges_.back()->from() > pos);
    return nullptr;
  }

  // There can be multiple ranges with |range->from == pos|. We want to start at
  // the first one.
  while (index > 0 && ranges_[index - 1]->from() == pos) {
    index--;
  }

  // Verify the above code is correct:
  // * The range at |index| starts before or at |pos| and needs to be searched.
  // * All ranges before |index| start after |pos| and can be ignored.
  MOZ_ASSERT(ranges_[index]->from() <= pos);
  MOZ_ASSERT_IF(index > 0, ranges_[index - 1]->from() > pos);

  LiveRange* found = nullptr;
  do {
    LiveRange* range = ranges_[index];
    if (range->covers(pos)) {
      if (!preferRegister || range->bundle()->allocation().isAnyRegister()) {
        return range;
      }
      if (!found) {
        found = range;
      }
    }
    index++;
  } while (index < len);

  return found;
}

void VirtualRegister::sortRanges() {
  if (rangesSorted_) {
    assertRangesSorted();
    return;
  }

  // Sort ranges by start position in descending order.
  //
  // It would be correct to only compare the start position, but std::sort is
  // not a stable sort and we don't want the order of ranges with the same start
  // position to depend on the sort implementation. Use the end position and the
  // bundle's id to ensure ranges are always sorted the same way.
  auto compareRanges = [](LiveRange* a, LiveRange* b) -> bool {
    if (a->from() != b->from()) {
      return a->from() > b->from();
    }
    if (a->to() != b->to()) {
      return a->to() > b->to();
    }
    // Overlapping live ranges must belong to different bundles.
    MOZ_ASSERT_IF(a != b, a->bundle()->id() != b->bundle()->id());
    return a->bundle()->id() > b->bundle()->id();
  };
  std::sort(ranges_.begin(), ranges_.end(), compareRanges);

  rangesSorted_ = true;
}

#ifdef DEBUG
void VirtualRegister::assertRangesSorted() const {
  MOZ_ASSERT(rangesSorted_);

  // Assert the last N ranges in the vector are sorted correctly. We don't check
  // the whole vector to not slow down debug builds too much.

  size_t len = ranges_.length();
  static constexpr size_t MaxIterations = 4;
  size_t start = len > MaxIterations ? (len - MaxIterations) : 1;

  for (size_t i = start; i < len; i++) {
    LiveRange* prev = ranges_[i - 1];
    LiveRange* range = ranges_[i];
    MOZ_ASSERT(range->from() <= prev->from());

    // If two ranges have the same |from| position, neither must be defining.
    // This ensures the defining range, if any, always comes first after
    // sorting.
    MOZ_ASSERT_IF(range->from() == prev->from(),
                  !range->hasDefinition() && !prev->hasDefinition());
  }
}
#endif

bool VirtualRegister::addRange(LiveRange* range) {
  bool sorted = ranges_.empty() ||
                (rangesSorted_ && ranges_.back()->from() >= range->from());
  if (!ranges_.append(range)) {
    return false;
  }
  rangesSorted_ = sorted;
  return true;
}

void VirtualRegister::removeFirstRange(RangeIterator& iter) {
  MOZ_ASSERT(iter.index() == ranges_.length() - 1);
  ranges_.popBack();
}

void VirtualRegister::removeRangesForBundle(LiveBundle* bundle) {
  auto bundleMatches = [bundle](LiveRange* range) {
    return range->bundle() == bundle;
  };
  ranges_.eraseIf(bundleMatches);
}

template <typename Pred>
void VirtualRegister::removeRangesIf(Pred&& pred) {
  assertRangesSorted();
  ranges_.eraseIf([&](LiveRange* range) { return pred(ranges_, range); });
}

// Helper for ::tryMergeReusedRegister.
bool VirtualRegister::replaceLastRangeLinear(LiveRange* old, LiveRange* newPre,
                                             LiveRange* newPost) {
  assertRangesSorted();

  // |old| is currently the first range in the Vector (the range with the
  // highest start position). This range is being replaced with two new ranges,
  // |newPre| and |newPost|. The relative order of these ranges by start
  // position is:
  //
  //   old <= newPre <= newPost
  //
  // To keep the vector sorted by descending start position, |newPost| must
  // become the new last range and |newPre| must be inserted after it.

  MOZ_ASSERT(ranges_[0] == old);
  MOZ_ASSERT(old->from() <= newPre->from());
  MOZ_ASSERT(newPre->from() <= newPost->from());

  ranges_[0] = newPost;

  if (!ranges_.insert(ranges_.begin() + 1, newPre)) {
    return false;
  }

  assertRangesSorted();
  return true;
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Misc helpers: queries about uses                                          //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

static inline LDefinition* FindReusingDefOrTemp(LNode* node,
                                                LAllocation* alloc) {
  if (node->isPhi()) {
    MOZ_ASSERT(node->toPhi()->numDefs() == 1);
    MOZ_ASSERT(node->toPhi()->getDef(0)->policy() !=
               LDefinition::MUST_REUSE_INPUT);
    return nullptr;
  }

  LInstruction* ins = node->toInstruction();

  for (size_t i = 0; i < ins->numDefs(); i++) {
    LDefinition* def = ins->getDef(i);
    if (def->policy() == LDefinition::MUST_REUSE_INPUT &&
        ins->getOperand(def->getReusedInput()) == alloc) {
      return def;
    }
  }
  for (size_t i = 0; i < ins->numTemps(); i++) {
    LDefinition* def = ins->getTemp(i);
    if (def->policy() == LDefinition::MUST_REUSE_INPUT &&
        ins->getOperand(def->getReusedInput()) == alloc) {
      return def;
    }
  }
  return nullptr;
}

bool BacktrackingAllocator::isReusedInput(LUse* use, LNode* ins,
                                          bool considerCopy) {
  if (LDefinition* def = FindReusingDefOrTemp(ins, use)) {
    return considerCopy || !vregs[def->virtualRegister()].mustCopyInput();
  }
  return false;
}

bool BacktrackingAllocator::isRegisterUse(UsePosition* use, LNode* ins,
                                          bool considerCopy) {
  switch (use->usePolicy()) {
    case LUse::ANY:
      return isReusedInput(use->use(), ins, considerCopy);

    case LUse::REGISTER:
    case LUse::FIXED:
      return true;

    default:
      return false;
  }
}

bool BacktrackingAllocator::isRegisterDefinition(LiveRange* range) {
  if (!range->hasDefinition()) {
    return false;
  }

  VirtualRegister& reg = range->vreg();
  if (reg.ins()->isPhi()) {
    return false;
  }

  if (reg.def()->policy() == LDefinition::FIXED &&
      !reg.def()->output()->isAnyRegister()) {
    return false;
  }

  return true;
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Misc helpers: atomic LIR groups                                           //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

// The following groupings contain implicit (invisible to ::buildLivenessInfo)
// value flows, and therefore no split points may be requested inside them.
// This is an otherwise unstated part of the contract between LIR generation
// and the allocator.
//
// (1) (any insn) ; OsiPoint
//
// [Further group definitions and supporting code to come, pending rework
//  of the wasm atomic-group situation.]

CodePosition BacktrackingAllocator::minimalDefEnd(LNode* ins) const {
  // Compute the shortest interval that captures vregs defined by ins.
  // Watch for instructions that are followed by an OSI point.
  // If moves are introduced between the instruction and the OSI point then
  // safepoint information for the instruction may be incorrect.
  while (true) {
    LNode* next = insData[ins->id() + 1];
    if (!next->isOsiPoint()) {
      break;
    }
    ins = next;
  }

  return outputOf(ins);
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Misc helpers: computation of bundle priorities and spill weights          //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

size_t BacktrackingAllocator::computePriority(LiveBundle* bundle) {
  // The priority of a bundle is its total length, so that longer lived
  // bundles will be processed before shorter ones (even if the longer ones
  // have a low spill weight). See processBundle().
  size_t lifetimeTotal = 0;

  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;
    lifetimeTotal += range->to() - range->from();
  }

  return lifetimeTotal;
}

bool BacktrackingAllocator::minimalDef(LiveRange* range, LNode* ins) {
  // Whether this is a minimal range capturing a definition at ins.
  return (range->to() <= minimalDefEnd(ins).next()) &&
         ((!ins->isPhi() && range->from() == inputOf(ins)) ||
          range->from() == outputOf(ins));
}

bool BacktrackingAllocator::minimalUse(LiveRange* range, UsePosition* use) {
  // Whether this is a minimal range capturing |use|.
  LNode* ins = insData[use->pos];
  return (range->from() == inputOf(ins)) &&
         (range->to() ==
          (use->use()->usedAtStart() ? outputOf(ins) : outputOf(ins).next()));
}

bool BacktrackingAllocator::minimalBundle(LiveBundle* bundle, bool* pfixed) {
  // Return true iff |bundle| is a minimal bundle. A minimal bundle is a bundle
  // that can't be split further. Minimal bundles have a single (very short)
  // range.
  //
  // If the bundle is minimal we also set the |*pfixed| outparam to indicate
  // whether the bundle must be allocated to a fixed register. The value of
  // |*pfixed| is undefined if this function returns false.

  LiveBundle::RangeIterator iter = bundle->rangesBegin();
  LiveRange* range = *iter;

  MOZ_ASSERT(range->hasVreg(), "Call ranges are not added to LiveBundles");

  // If a bundle contains multiple ranges, splitAtAllRegisterUses will split
  // each range into a separate bundle.
  if (++iter) {
    return false;
  }

  if (range->hasDefinition()) {
    VirtualRegister& reg = range->vreg();
    if (!minimalDef(range, reg.ins())) {
      return false;
    }
    if (pfixed) {
      *pfixed = reg.def()->policy() == LDefinition::FIXED &&
                reg.def()->output()->isAnyRegister();
    }
    return true;
  }

  // Performance optimization: |minimalUse| will only return true for length-1
  // or length-2 ranges so if this is a longer range it can't be minimal.
  if (range->to() - range->from() > 2) {
#ifdef DEBUG
    for (UsePositionIterator iter = range->usesBegin(); iter; iter++) {
      MOZ_ASSERT(!minimalUse(range, *iter));
    }
#endif
    return false;
  }

  bool fixed = false, minimal = false, multiple = false;

  for (UsePositionIterator iter = range->usesBegin(); iter; iter++) {
    if (iter != range->usesBegin()) {
      multiple = true;
    }

    switch (iter->usePolicy()) {
      case LUse::FIXED:
        if (fixed) {
          return false;
        }
        fixed = true;
        if (minimalUse(range, *iter)) {
          minimal = true;
        }
        break;

      case LUse::REGISTER:
        if (minimalUse(range, *iter)) {
          minimal = true;
        }
        break;

      default:
        break;
    }
  }

  // If a range contains a fixed use and at least one other use,
  // splitAtAllRegisterUses will split each use into a different bundle.
  if (multiple && fixed) {
    return false;
  }

  if (!minimal) {
    return false;
  }
  if (pfixed) {
    *pfixed = fixed;
  }
  return true;
}

size_t BacktrackingAllocator::computeSpillWeight(LiveBundle* bundle) {
  // Minimal bundles have an extremely high spill weight, to ensure they
  // can evict any other bundles and be allocated to a register.
  bool fixed;
  if (minimalBundle(bundle, &fixed)) {
    return fixed ? 2000000 : 1000000;
  }

  size_t usesTotal = 0;
  fixed = false;

  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;

    if (range->hasDefinition()) {
      VirtualRegister& reg = range->vreg();
      if (reg.def()->policy() == LDefinition::FIXED &&
          reg.def()->output()->isAnyRegister()) {
        usesTotal += 2000;
        fixed = true;
      } else if (!reg.ins()->isPhi()) {
        usesTotal += 2000;
      }
    }

    usesTotal += range->usesSpillWeight();
    if (range->numFixedUses() > 0) {
      fixed = true;
    }
  }

  // Compute spill weight as a use density, lowering the weight for long
  // lived bundles with relatively few uses.
  size_t lifetimeTotal = computePriority(bundle);
  return lifetimeTotal ? usesTotal / lifetimeTotal : 0;
}

size_t BacktrackingAllocator::maximumSpillWeight(
    const LiveBundleVector& bundles) {
  size_t maxWeight = 0;
  for (size_t i = 0; i < bundles.length(); i++) {
    maxWeight = std::max(maxWeight, computeSpillWeight(bundles[i]));
  }
  return maxWeight;
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Initialization of the allocator                                           //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

// This function pre-allocates and initializes as much global state as possible
// to avoid littering the algorithms with memory management cruft.
bool BacktrackingAllocator::init() {
  if (!insData.init(mir, graph.numInstructions())) {
    return false;
  }

  if (!entryPositions.reserve(graph.numBlocks()) ||
      !exitPositions.reserve(graph.numBlocks())) {
    return false;
  }

  for (size_t i = 0; i < graph.numBlocks(); i++) {
    LBlock* block = graph.getBlock(i);
    for (LInstructionIterator ins = block->begin(); ins != block->end();
         ins++) {
      insData[ins->id()] = *ins;
    }
    for (size_t j = 0; j < block->numPhis(); j++) {
      LPhi* phi = block->getPhi(j);
      insData[phi->id()] = phi;
    }

    CodePosition entry =
        block->numPhis() != 0
            ? CodePosition(block->getPhi(0)->id(), CodePosition::INPUT)
            : inputOf(block->firstInstructionWithId());
    CodePosition exit = outputOf(block->lastInstructionWithId());

    MOZ_ASSERT(block->mir()->id() == i);
    entryPositions.infallibleAppend(entry);
    exitPositions.infallibleAppend(exit);
  }

  uint32_t numBlocks = graph.numBlockIds();
  MOZ_ASSERT(liveIn.empty());
  if (!liveIn.growBy(numBlocks)) {
    return false;
  }

  size_t numVregs = graph.numVirtualRegisters();
  if (!vregs.initCapacity(numVregs)) {
    return false;
  }
  for (uint32_t i = 0; i < numVregs; i++) {
    vregs.infallibleEmplaceBack();
  }

  // Build virtual register objects.
  for (size_t i = 0; i < graph.numBlocks(); i++) {
    if (mir->shouldCancel("Create data structures (main loop)")) {
      return false;
    }

    LBlock* block = graph.getBlock(i);
    for (LInstructionIterator ins = block->begin(); ins != block->end();
         ins++) {
      if (mir->shouldCancel("Create data structures (inner loop 1)")) {
        return false;
      }

      for (LInstruction::OutputIter output(*ins); !output.done(); output++) {
        vreg(*output).init(*ins, *output, /* isTemp = */ false);
      }
      for (LInstruction::TempIter temp(*ins); !temp.done(); temp++) {
        vreg(*temp).init(*ins, *temp, /* isTemp = */ true);
      }
    }
    for (size_t j = 0; j < block->numPhis(); j++) {
      LPhi* phi = block->getPhi(j);
      LDefinition* def = phi->getDef(0);
      vreg(def).init(phi, def, /* isTemp = */ false);
    }
  }

  LiveRegisterSet remainingRegisters(allRegisters_.asLiveSet());
  while (!remainingRegisters.emptyGeneral()) {
    AnyRegister reg = AnyRegister(remainingRegisters.takeAnyGeneral());
    registers[reg.code()].allocatable = true;
  }
  while (!remainingRegisters.emptyFloat()) {
    AnyRegister reg =
        AnyRegister(remainingRegisters.takeAnyFloat<RegTypeName::Any>());
    registers[reg.code()].allocatable = true;
  }

  LifoAlloc* lifoAlloc = mir->alloc().lifoAlloc();
  for (size_t i = 0; i < AnyRegister::Total; i++) {
    registers[i].reg = AnyRegister::FromCode(i);
    registers[i].allocations.setAllocator(lifoAlloc);
  }

  hotcode.setAllocator(lifoAlloc);

  // Partition the graph into hot and cold sections, for helping to make
  // splitting decisions. Since we don't have any profiling data this is a
  // crapshoot, so just mark the bodies of inner loops as hot and everything
  // else as cold.

  LBlock* backedge = nullptr;
  for (size_t i = 0; i < graph.numBlocks(); i++) {
    LBlock* block = graph.getBlock(i);

    // If we see a loop header, mark the backedge so we know when we have
    // hit the end of the loop. Don't process the loop immediately, so that
    // if there is an inner loop we will ignore the outer backedge.
    if (block->mir()->isLoopHeader()) {
      backedge = block->mir()->backedge()->lir();
    }

    if (block == backedge) {
      LBlock* header = block->mir()->loopHeaderOfBackedge()->lir();
      LiveRange* range = LiveRange::FallibleNew(
          alloc(), nullptr, entryOf(header), exitOf(block).next());
      if (!range || !hotcode.insert(range)) {
        return false;
      }
    }
  }

  return true;
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Liveness analysis                                                         //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

// Helper for ::buildLivenessInfo
bool BacktrackingAllocator::addInitialFixedRange(AnyRegister reg,
                                                 CodePosition from,
                                                 CodePosition to) {
  LiveRange* range = LiveRange::FallibleNew(alloc(), nullptr, from, to);
  if (!range) {
    return false;
  }
  LiveRangePlus rangePlus(range);
  return registers[reg.code()].allocations.insert(rangePlus);
}

// Helper for ::buildLivenessInfo
#ifdef DEBUG
// Returns true iff ins has a def/temp reusing the input allocation.
static bool IsInputReused(LInstruction* ins, LUse* use) {
  for (size_t i = 0; i < ins->numDefs(); i++) {
    if (ins->getDef(i)->policy() == LDefinition::MUST_REUSE_INPUT &&
        ins->getOperand(ins->getDef(i)->getReusedInput())->toUse() == use) {
      return true;
    }
  }

  for (size_t i = 0; i < ins->numTemps(); i++) {
    if (ins->getTemp(i)->policy() == LDefinition::MUST_REUSE_INPUT &&
        ins->getOperand(ins->getTemp(i)->getReusedInput())->toUse() == use) {
      return true;
    }
  }

  return false;
}
#endif

/*
 * This function builds up liveness ranges for all virtual registers
 * defined in the function.
 *
 * The algorithm is based on the one published in:
 *
 * Wimmer, Christian, and Michael Franz. "Linear Scan Register Allocation on
 *     SSA Form." Proceedings of the International Symposium on Code Generation
 *     and Optimization. Toronto, Ontario, Canada, ACM. 2010. 170-79. PDF.
 *
 * The algorithm operates on blocks ordered such that dominators of a block
 * are before the block itself, and such that all blocks of a loop are
 * contiguous. It proceeds backwards over the instructions in this order,
 * marking registers live at their uses, ending their live ranges at
 * definitions, and recording which registers are live at the top of every
 * block. To deal with loop backedges, registers live at the beginning of
 * a loop gain a range covering the entire loop.
 */
bool BacktrackingAllocator::buildLivenessInfo() {
  JitSpew(JitSpew_RegAlloc, "Beginning liveness analysis");

  // The callPositions vector and the safepoint vectors are initialized from
  // index |length - 1| to 0, to ensure they are sorted.
  if (!callPositions.growByUninitialized(graph.numCallInstructions()) ||
      !safepoints_.growByUninitialized(graph.numSafepoints()) ||
      !nonCallSafepoints_.growByUninitialized(graph.numNonCallSafepoints())) {
    return false;
  }
  size_t prevCallPositionIndex = callPositions.length();
  size_t prevSafepointIndex = safepoints_.length();
  size_t prevNonCallSafepointIndex = nonCallSafepoints_.length();

  for (size_t i = graph.numBlocks(); i > 0; i--) {
    if (mir->shouldCancel("Build Liveness Info (main loop)")) {
      return false;
    }

    LBlock* block = graph.getBlock(i - 1);
    MBasicBlock* mblock = block->mir();

    VirtualRegBitSet& live = liveIn[mblock->id()];

    // Propagate liveIn from our successors to us.
    for (size_t i = 0; i < mblock->lastIns()->numSuccessors(); i++) {
      MBasicBlock* successor = mblock->lastIns()->getSuccessor(i);
      // Skip backedges, as we fix them up at the loop header.
      if (mblock->id() < successor->id()) {
        if (!live.insertAll(liveIn[successor->id()])) {
          return false;
        }
      }
    }

    // Add successor phis.
    if (mblock->successorWithPhis()) {
      LBlock* phiSuccessor = mblock->successorWithPhis()->lir();
      for (unsigned int j = 0; j < phiSuccessor->numPhis(); j++) {
        LPhi* phi = phiSuccessor->getPhi(j);
        LAllocation* use = phi->getOperand(mblock->positionInPhiSuccessor());
        uint32_t reg = use->toUse()->virtualRegister();
        if (!live.insert(reg)) {
          return false;
        }
        vreg(use).setUsedByPhi();
      }
    }

    // Registers are assumed alive for the entire block, a define shortens
    // the range to the point of definition.
    for (VirtualRegBitSet::Iterator liveRegId(live); liveRegId; ++liveRegId) {
      if (!vregs[*liveRegId].addInitialRange(alloc(), entryOf(block),
                                             exitOf(block).next())) {
        return false;
      }
    }

    // Shorten the front end of ranges for live variables to their point of
    // definition, if found.
    for (LInstructionReverseIterator ins = block->rbegin();
         ins != block->rend(); ins++) {
      // Calls may clobber registers, so force a spill and reload around the
      // callsite.
      if (ins->isCall()) {
        for (AnyRegisterIterator iter(allRegisters_.asLiveSet()); iter.more();
             ++iter) {
          bool found = false;
          for (size_t i = 0; i < ins->numDefs(); i++) {
            if (ins->getDef(i)->isFixed() &&
                ins->getDef(i)->output()->aliases(LAllocation(*iter))) {
              found = true;
              break;
            }
          }
          // If this register doesn't have an explicit def above, mark
          // it as clobbered by the call unless it is actually
          // call-preserved.
          if (!found && !ins->isCallPreserved(*iter)) {
            if (!addInitialFixedRange(*iter, outputOf(*ins),
                                      outputOf(*ins).next())) {
              return false;
            }
          }
        }

        // Add the call position before the previous one to ensure the vector is
        // sorted.
        MOZ_ASSERT(prevCallPositionIndex > 0);
        MOZ_ASSERT_IF(prevCallPositionIndex < callPositions.length(),
                      outputOf(*ins) < callPositions[prevCallPositionIndex]);
        prevCallPositionIndex--;
        callPositions[prevCallPositionIndex] = outputOf(*ins);
      }

      for (LInstruction::OutputIter output(*ins); !output.done(); output++) {
        LDefinition* def = *output;
        CodePosition from = outputOf(*ins);

        if (def->policy() == LDefinition::MUST_REUSE_INPUT) {
          ins->changePolicyOfReusedInputToAny(def);
        }

        if (!vreg(def).addInitialRange(alloc(), from, from.next())) {
          return false;
        }
        vreg(def).setInitialDefinition(from);
        live.remove(def->virtualRegister());
      }

      for (LInstruction::TempIter tempIter(*ins); !tempIter.done();
           tempIter++) {
        LDefinition* temp = *tempIter;

        // Normally temps are considered to cover both the input
        // and output of the associated instruction. In some cases
        // though we want to use a fixed register as both an input
        // and clobbered register in the instruction, so watch for
        // this and shorten the temp to cover only the output.
        CodePosition from = inputOf(*ins);
        if (temp->policy() == LDefinition::FIXED) {
          AnyRegister reg = temp->output()->toAnyRegister();
          for (LInstruction::NonSnapshotInputIter alloc(**ins); alloc.more();
               alloc.next()) {
            if (alloc->isUse()) {
              LUse* use = alloc->toUse();
              if (use->isFixedRegister()) {
                if (GetFixedRegister(vreg(use).def(), use) == reg) {
                  from = outputOf(*ins);
                }
              }
            }
          }
        }

        // * For non-call instructions, temps cover both the input and output,
        //   so temps never alias uses (even at-start uses) or defs.
        // * For call instructions, temps only cover the input (the output is
        //   used for the force-spill ranges added above). This means temps
        //   still don't alias uses but they can alias the (fixed) defs. For now
        //   we conservatively require temps to have a fixed register for call
        //   instructions to prevent a footgun.
        MOZ_ASSERT_IF(ins->isCall(), temp->policy() == LDefinition::FIXED);
        CodePosition to =
            ins->isCall() ? outputOf(*ins) : outputOf(*ins).next();

        if (!vreg(temp).addInitialRange(alloc(), from, to)) {
          return false;
        }
        vreg(temp).setInitialDefinition(from);
      }

      DebugOnly<bool> hasUseRegister = false;
      DebugOnly<bool> hasUseRegisterAtStart = false;

      for (LInstruction::InputIter inputAlloc(**ins); inputAlloc.more();
           inputAlloc.next()) {
        if (inputAlloc->isUse()) {
          LUse* use = inputAlloc->toUse();

          // Call uses should always be at-start, since calls use all
          // registers.
          MOZ_ASSERT_IF(ins->isCall() && !inputAlloc.isSnapshotInput(),
                        use->usedAtStart());

#ifdef DEBUG
          // If there are both useRegisterAtStart(x) and useRegister(y)
          // uses, we may assign the same register to both operands
          // (bug 772830). Don't allow this for now.
          if (use->policy() == LUse::REGISTER) {
            if (use->usedAtStart()) {
              if (!IsInputReused(*ins, use)) {
                hasUseRegisterAtStart = true;
              }
            } else {
              hasUseRegister = true;
            }
          }
          MOZ_ASSERT(!(hasUseRegister && hasUseRegisterAtStart));
#endif

          // Don't treat RECOVERED_INPUT uses as keeping the vreg alive.
          if (use->policy() == LUse::RECOVERED_INPUT) {
            continue;
          }

          CodePosition to = use->usedAtStart() ? inputOf(*ins) : outputOf(*ins);
          if (use->isFixedRegister()) {
            LAllocation reg(AnyRegister::FromCode(use->registerCode()));
            for (size_t i = 0; i < ins->numDefs(); i++) {
              LDefinition* def = ins->getDef(i);
              if (def->policy() == LDefinition::FIXED &&
                  *def->output() == reg) {
                to = inputOf(*ins);
              }
            }
          }

          if (!vreg(use).addInitialRange(alloc(), entryOf(block), to.next())) {
            return false;
          }
          UsePosition* usePosition =
              new (alloc().fallible()) UsePosition(use, to);
          if (!usePosition) {
            return false;
          }
          vreg(use).addInitialUse(usePosition);
          if (!live.insert(use->virtualRegister())) {
            return false;
          }
        }
      }

      if (ins->safepoint()) {
        MOZ_ASSERT(prevSafepointIndex > 0);
        prevSafepointIndex--;
        safepoints_[prevSafepointIndex] = *ins;
        if (!ins->isCall()) {
          MOZ_ASSERT(prevNonCallSafepointIndex > 0);
          prevNonCallSafepointIndex--;
          nonCallSafepoints_[prevNonCallSafepointIndex] = *ins;
        }
      }
    }

    // Phis have simultaneous assignment semantics at block begin, so at
    // the beginning of the block we can be sure that liveIn does not
    // contain any phi outputs.
    for (unsigned int i = 0; i < block->numPhis(); i++) {
      LDefinition* def = block->getPhi(i)->getDef(0);
      if (live.contains(def->virtualRegister())) {
        live.remove(def->virtualRegister());
      } else {
        // This is a dead phi, so add a dummy range over all phis. This
        // can go away if we have an earlier dead code elimination pass.
        CodePosition entryPos = entryOf(block);
        if (!vreg(def).addInitialRange(alloc(), entryPos, entryPos.next())) {
          return false;
        }
      }
    }

    if (mblock->isLoopHeader()) {
      // MakeLoopsContiguous ensures blocks of a loop are contiguous, so add a
      // single live range spanning the loop. Because we require liveIn in a
      // later pass for resolution, we also have to fix that up for all loop
      // blocks.

      MBasicBlock* backedge = mblock->backedge();

      // Add a range for this entire loop
      CodePosition from = entryOf(mblock->lir());
      CodePosition to = exitOf(backedge->lir()).next();

      for (VirtualRegBitSet::Iterator liveRegId(live); liveRegId; ++liveRegId) {
        if (!vregs[*liveRegId].addInitialRange(alloc(), from, to)) {
          return false;
        }
      }

      if (mblock != backedge) {
        // Start at the block after |mblock|.
        MOZ_ASSERT(graph.getBlock(i - 1) == mblock->lir());
        size_t j = i;
        while (true) {
          MBasicBlock* loopBlock = graph.getBlock(j)->mir();

          // Fix up the liveIn set.
          if (!liveIn[loopBlock->id()].insertAll(live)) {
            return false;
          }

          if (loopBlock == backedge) {
            break;
          }
          j++;
        }
      }
    }

    MOZ_ASSERT_IF(!mblock->numPredecessors(), live.empty());
  }

  MOZ_RELEASE_ASSERT(prevCallPositionIndex == 0,
                     "Must have initialized all call positions");
  MOZ_RELEASE_ASSERT(prevSafepointIndex == 0,
                     "Must have initialized all safepoints");
  MOZ_RELEASE_ASSERT(prevNonCallSafepointIndex == 0,
                     "Must have initialized all safepoints");

  JitSpew(JitSpew_RegAlloc, "Completed liveness analysis");
  return true;
}

Maybe<size_t> BacktrackingAllocator::lookupFirstCallPositionInRange(
    CodePosition from, CodePosition to) {
  MOZ_ASSERT(from < to);

  // Use binary search to find the first call position in range [from, to).
  size_t len = callPositions.length();
  size_t index;
  mozilla::BinarySearch(callPositions, 0, len, from, &index);
  MOZ_ASSERT(index <= len);

  if (index == len) {
    // Last call position in the vector is before this range.
    MOZ_ASSERT_IF(len > 0, callPositions.back() < from);
    return {};
  }

  if (callPositions[index] >= to) {
    // The next call position comes after this range.
    return {};
  }

  MOZ_ASSERT(callPositions[index] >= from && callPositions[index] < to);
  return mozilla::Some(index);
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Merging and queueing of LiveRange groups                                  //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

// Helper for ::tryMergeBundles
static bool IsArgumentSlotDefinition(LDefinition* def) {
  return def->policy() == LDefinition::FIXED && def->output()->isArgument();
}

// Helper for ::tryMergeBundles
static bool IsThisSlotDefinition(LDefinition* def) {
  return IsArgumentSlotDefinition(def) &&
         def->output()->toArgument()->index() <
             THIS_FRAME_ARGSLOT + sizeof(Value);
}

// Helper for ::tryMergeBundles
static bool HasStackPolicy(LDefinition* def) {
  return def->policy() == LDefinition::STACK;
}

// Helper for ::tryMergeBundles
static bool CanMergeTypesInBundle(LDefinition::Type a, LDefinition::Type b) {
  // Fast path for the common case.
  if (a == b) {
    return true;
  }

  // Only merge if the sizes match, so that we don't get confused about the
  // width of spill slots.
  return LStackSlot::width(a) == LStackSlot::width(b);
}

// Helper for ::tryMergeReusedRegister
void BacktrackingAllocator::tryMergeBundles(LiveBundle* bundle0,
                                            LiveBundle* bundle1) {
  // See if bundle0 and bundle1 can be merged together.
  if (bundle0 == bundle1) {
    return;
  }

  // Get a representative virtual register from each bundle.
  VirtualRegister& reg0 = bundle0->firstRange()->vreg();
  VirtualRegister& reg1 = bundle1->firstRange()->vreg();

  MOZ_ASSERT(CanMergeTypesInBundle(reg0.type(), reg1.type()));
  MOZ_ASSERT(reg0.isCompatible(reg1));

  if (!compilingWasm()) {
    // Registers which might spill to the frame's |this| slot can only be
    // grouped with other such registers. The frame's |this| slot must always
    // hold the |this| value, as required by JitFrame tracing and by the Ion
    // constructor calling convention.
    if (IsThisSlotDefinition(reg0.def()) || IsThisSlotDefinition(reg1.def())) {
      if (*reg0.def()->output() != *reg1.def()->output()) {
        return;
      }
    }

    // Registers which might spill to the frame's argument slots can only be
    // grouped with other such registers in the following cases:
    //
    // * If the frame might access those arguments through a lazy arguments
    //   object or rest parameter.
    //
    // * On 32-bit platforms, to prevent creating garbage Values in the formal
    //   argument slots when spilling just the type or payload of a different
    //   Value. GC tracing of ArraySortData relies on the argument slots holding
    //   valid JS values. See bug 1911858.
    if (IsArgumentSlotDefinition(reg0.def()) ||
        IsArgumentSlotDefinition(reg1.def())) {
#ifdef JS_PUNBOX64
      MOZ_ASSERT(reg0.type() == LDefinition::Type::BOX);
      MOZ_ASSERT(reg1.type() == LDefinition::Type::BOX);
      bool canSpillToArgSlots =
          !graph.mir().entryBlock()->info().mayReadFrameArgsDirectly();
#else
      bool canSpillToArgSlots = false;
#endif
      if (!canSpillToArgSlots) {
        if (*reg0.def()->output() != *reg1.def()->output()) {
          return;
        }
      }
    }
  }

  // When we make a call to a WebAssembly function that returns multiple
  // results, some of those results can go on the stack.  The callee is passed a
  // pointer to this stack area, which is represented as having policy
  // LDefinition::STACK (with type LDefinition::STACKRESULTS).  Individual
  // results alias parts of the stack area with a value-appropriate type, but
  // policy LDefinition::STACK.  This aliasing between allocations makes it
  // unsound to merge anything with a LDefinition::STACK policy.
  if (HasStackPolicy(reg0.def()) || HasStackPolicy(reg1.def())) {
    return;
  }

  // Limit the number of times we compare ranges if there are many ranges in
  // one of the bundles, to avoid quadratic behavior.
  static const size_t MAX_RANGES = 200;

  // Make sure that ranges in the bundles do not overlap.
  LiveBundle::RangeIterator iter0 = bundle0->rangesBegin(),
                            iter1 = bundle1->rangesBegin();
  size_t count = 0;
  while (iter0 && iter1) {
    if (++count >= MAX_RANGES) {
      return;
    }

    LiveRange* range0 = *iter0;
    LiveRange* range1 = *iter1;

    if (range0->from() >= range1->to()) {
      iter1++;
    } else if (range1->from() >= range0->to()) {
      iter0++;
    } else {
      return;
    }
  }

  // Move all ranges from bundle1 into bundle0. Use a fast path for the case
  // where we can add all ranges from bundle1 at the end of bundle0.
  if (SortBefore(bundle0->lastRange(), bundle1->firstRange())) {
    while (LiveRange* range = bundle1->popFirstRange()) {
      bundle0->addRangeAtEnd(range);
    }
  } else {
    LiveRange* prevRange = nullptr;
    while (LiveRange* range = bundle1->popFirstRange()) {
      bundle0->addRange(range, prevRange);
      prevRange = range;
    }
  }
}

// Helper for ::mergeAndQueueRegisters
void BacktrackingAllocator::allocateStackDefinition(VirtualRegister& reg) {
  LInstruction* ins = reg.ins()->toInstruction();
  if (reg.def()->type() == LDefinition::STACKRESULTS) {
    LStackArea alloc(ins->toInstruction());
    stackSlotAllocator.allocateStackArea(&alloc);
    reg.def()->setOutput(alloc);
  } else {
    // Because the definitions are visited in order, the area has been allocated
    // before we reach this result, so we know the operand is an LStackArea.
    const LUse* use = ins->getOperand(0)->toUse();
    VirtualRegister& area = vregs[use->virtualRegister()];
    const LStackArea* areaAlloc = area.def()->output()->toStackArea();
    reg.def()->setOutput(areaAlloc->resultAlloc(ins, reg.def()));
  }
}

// Helper for ::mergeAndQueueRegisters
bool BacktrackingAllocator::tryMergeReusedRegister(VirtualRegister& def,
                                                   VirtualRegister& input) {
  // def is a vreg which reuses input for its output physical register. Try
  // to merge ranges for def with those of input if possible, as avoiding
  // copies before def's instruction is crucial for generated code quality
  // (MUST_REUSE_INPUT is used for all arithmetic on x86/x64).

  // Don't try to merge if the definition starts at the input point of the
  // instruction.
  if (def.firstRange()->from() == inputOf(def.ins())) {
    MOZ_ASSERT(def.isTemp());
    def.setMustCopyInput();
    return true;
  }
  MOZ_ASSERT(def.firstRange()->from() == outputOf(def.ins()));

  if (!CanMergeTypesInBundle(def.type(), input.type())) {
    def.setMustCopyInput();
    return true;
  }

  LiveRange* inputRange = input.rangeFor(outputOf(def.ins()));
  if (!inputRange) {
    // The input is not live after the instruction, either in a safepoint
    // for the instruction or in subsequent code. The input and output
    // can thus be in the same group.
    tryMergeBundles(def.firstBundle(), input.firstBundle());
    return true;
  }

  // Avoid merging in very large live ranges as merging has non-linear
  // complexity.  The cutoff value is hard to gauge.  1M was chosen because it
  // is "large" and yet usefully caps compile time on AutoCad-for-the-web to
  // something reasonable on a 2017-era desktop system.  See bug 1728781.
  //
  // Subsequently -- bug 1897427 comment 9 -- even this threshold seems too
  // high, resulting in delays of around 10 seconds.  Setting it to 500000
  // "fixes" that one, but it seems prudent to reduce it to 250000 so as to
  // avoid having to change it yet again at some future point.
  const uint32_t RANGE_SIZE_CUTOFF = 250000;
  if (inputRange->to() - inputRange->from() > RANGE_SIZE_CUTOFF) {
    def.setMustCopyInput();
    return true;
  }

  // The input is live afterwards, either in future instructions or in a
  // safepoint for the reusing instruction. This is impossible to satisfy
  // without copying the input.
  //
  // It may or may not be better to split the input into two bundles at the
  // point of the definition, which may permit merging. One case where it is
  // definitely better to split is if the input never has any register uses
  // after the instruction. Handle this splitting eagerly.

  LBlock* block = def.ins()->block();

  // The input's lifetime must end within the same block as the definition,
  // otherwise it could live on in phis elsewhere.
  if (inputRange != input.lastRange() || inputRange->to() > exitOf(block)) {
    def.setMustCopyInput();
    return true;
  }

  // If we already split the input for some other register, don't make a
  // third bundle.
  if (inputRange->bundle() != input.firstRange()->bundle()) {
    def.setMustCopyInput();
    return true;
  }

  // If the input will start out in memory then adding a separate bundle for
  // memory uses after the def won't help.
  if (input.def()->isFixed() && !input.def()->output()->isAnyRegister()) {
    def.setMustCopyInput();
    return true;
  }

  // The input cannot have register or reused uses after the definition.
  for (UsePositionIterator iter = inputRange->usesBegin(); iter; iter++) {
    if (iter->pos <= inputOf(def.ins())) {
      continue;
    }

    LUse* use = iter->use();
    if (FindReusingDefOrTemp(insData[iter->pos], use)) {
      def.setMustCopyInput();
      return true;
    }
    if (iter->usePolicy() != LUse::ANY &&
        iter->usePolicy() != LUse::KEEPALIVE) {
      def.setMustCopyInput();
      return true;
    }
  }

  LiveRange* preRange = LiveRange::FallibleNew(
      alloc(), &input, inputRange->from(), outputOf(def.ins()));
  if (!preRange) {
    return false;
  }

  // The new range starts at reg's input position, which means it overlaps
  // with the old range at one position. This is what we want, because we
  // need to copy the input before the instruction.
  LiveRange* postRange = LiveRange::FallibleNew(
      alloc(), &input, inputOf(def.ins()), inputRange->to());
  if (!postRange) {
    return false;
  }

  inputRange->tryToMoveDefAndUsesInto(preRange);
  inputRange->tryToMoveDefAndUsesInto(postRange);
  MOZ_ASSERT(!inputRange->hasUses());

  JitSpewIfEnabled(JitSpew_RegAlloc,
                   "  splitting reused input at %u to try to help grouping",
                   inputOf(def.ins()).bits());

  LiveBundle* firstBundle = inputRange->bundle();

  // Note: this inserts an item at the beginning of the ranges vector. That's
  // okay in this case because the checks above ensure this happens at most once
  // for the |input| virtual register.
  if (!input.replaceLastRangeLinear(inputRange, preRange, postRange)) {
    return false;
  }

  firstBundle->removeRange(inputRange);
  firstBundle->addRange(preRange);

  // The new range goes in a separate bundle, where it will be spilled during
  // allocation.
  LiveBundle* secondBundle =
      LiveBundle::FallibleNew(alloc(), nullptr, nullptr, getNextBundleId());
  if (!secondBundle) {
    return false;
  }
  secondBundle->addRangeAtEnd(postRange);

  tryMergeBundles(def.firstBundle(), input.firstBundle());
  return true;
}

bool BacktrackingAllocator::mergeAndQueueRegisters() {
  MOZ_ASSERT(!vregs[0u].hasRanges());

  // Create a bundle for each register containing all its ranges.
  for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
    VirtualRegister& reg = vregs[i];
    if (!reg.hasRanges()) {
      continue;
    }

    LiveBundle* bundle =
        LiveBundle::FallibleNew(alloc(), nullptr, nullptr, getNextBundleId());
    if (!bundle) {
      return false;
    }
    for (VirtualRegister::RangeIterator iter(reg); iter; iter++) {
      bundle->addRangeAtEnd(*iter);
    }
  }

  // If there is an OSR block, merge parameters in that block with the
  // corresponding parameters in the initial block.
  if (MBasicBlock* osr = graph.mir().osrBlock()) {
    size_t original = 1;
    for (LInstructionIterator iter = osr->lir()->begin();
         iter != osr->lir()->end(); iter++) {
      if (iter->isParameter()) {
        for (size_t i = 0; i < iter->numDefs(); i++) {
          DebugOnly<bool> found = false;
          VirtualRegister& paramVreg = vreg(iter->getDef(i));
          for (; original < paramVreg.vreg(); original++) {
            VirtualRegister& originalVreg = vregs[original];
            if (*originalVreg.def()->output() == *iter->getDef(i)->output()) {
              MOZ_ASSERT(originalVreg.ins()->isParameter());
              tryMergeBundles(originalVreg.firstBundle(),
                              paramVreg.firstBundle());
              found = true;
              break;
            }
          }
          MOZ_ASSERT(found);
        }
      }
    }
  }

  // Try to merge registers with their reused inputs.
  for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
    VirtualRegister& reg = vregs[i];
    if (!reg.hasRanges()) {
      continue;
    }

    if (reg.def()->policy() == LDefinition::MUST_REUSE_INPUT) {
      LUse* use = reg.ins()
                      ->toInstruction()
                      ->getOperand(reg.def()->getReusedInput())
                      ->toUse();
      if (!tryMergeReusedRegister(reg, vreg(use))) {
        return false;
      }
    }
  }

  // Try to merge phis with their inputs.
  for (size_t i = 0; i < graph.numBlocks(); i++) {
    LBlock* block = graph.getBlock(i);
    for (size_t j = 0; j < block->numPhis(); j++) {
      LPhi* phi = block->getPhi(j);
      VirtualRegister& outputVreg = vreg(phi->getDef(0));
      for (size_t k = 0, kend = phi->numOperands(); k < kend; k++) {
        VirtualRegister& inputVreg = vreg(phi->getOperand(k)->toUse());
        tryMergeBundles(inputVreg.firstBundle(), outputVreg.firstBundle());
      }
    }
  }

  // Add all bundles to the allocation queue, and create spill sets for them.
  for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
    VirtualRegister& reg = vregs[i];

    // Eagerly allocate stack result areas and their component stack results.
    if (reg.def() && reg.def()->policy() == LDefinition::STACK) {
      allocateStackDefinition(reg);
    }

    for (VirtualRegister::RangeIterator iter(reg); iter; iter++) {
      LiveRange* range = *iter;
      LiveBundle* bundle = range->bundle();
      if (range == bundle->firstRange()) {
        if (!alloc().ensureBallast()) {
          return false;
        }

        SpillSet* spill = SpillSet::New(alloc());
        if (!spill) {
          return false;
        }
        bundle->setSpillSet(spill);

        size_t priority = computePriority(bundle);
        if (!allocationQueue.insert(QueueItem(bundle, priority))) {
          return false;
        }
      }
    }
  }

  return true;
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Code for the splitting/spilling subsystem begins here.                    //
//                                                                           //
// The code that follows is structured in the following sequence:            //
//                                                                           //
// (1) Routines that are helpers for ::splitAt.                              //
// (2) ::splitAt itself, which implements splitting decisions.               //
// (3) heuristic routines (eg ::splitAcrossCalls), which decide where        //
//     splits should be made.  They then call ::splitAt to perform the       //
//     chosen split.                                                         //
// (4) The top level driver, ::chooseBundleSplit.                            //
//                                                                           //
// There are further comments on ::splitAt and ::chooseBundleSplit below.    //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Implementation of splitting decisions, but not the making of those        //
// decisions: various helper functions                                       //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

bool BacktrackingAllocator::updateVirtualRegisterListsThenRequeueBundles(
    LiveBundle* bundle, const LiveBundleVector& newBundles) {
#ifdef DEBUG
  if (newBundles.length() == 1) {
    LiveBundle* newBundle = newBundles[0];
    if (newBundle->numRanges() == bundle->numRanges() &&
        computePriority(newBundle) == computePriority(bundle)) {
      bool different = false;
      LiveBundle::RangeIterator oldRanges = bundle->rangesBegin();
      LiveBundle::RangeIterator newRanges = newBundle->rangesBegin();
      while (oldRanges) {
        LiveRange* oldRange = *oldRanges;
        LiveRange* newRange = *newRanges;
        if (oldRange->from() != newRange->from() ||
            oldRange->to() != newRange->to()) {
          different = true;
          break;
        }
        oldRanges++;
        newRanges++;
      }

      // This is likely to trigger an infinite loop in register allocation. This
      // can be the result of invalid register constraints, making regalloc
      // impossible; consider relaxing those.
      MOZ_ASSERT(different,
                 "Split results in the same bundle with the same priority");
    }
  }
#endif

  if (JitSpewEnabled(JitSpew_RegAlloc)) {
    JitSpew(JitSpew_RegAlloc, "  .. into:");
    for (size_t i = 0; i < newBundles.length(); i++) {
      JitSpew(JitSpew_RegAlloc, "    %s", newBundles[i]->toString().get());
    }
  }

  // Remove all ranges in the old bundle from their register's list.
  bundle->removeAllRangesFromVirtualRegisters();

  // Add all ranges in the new bundles to their register's list.
  for (size_t i = 0; i < newBundles.length(); i++) {
    LiveBundle* newBundle = newBundles[i];
    for (LiveBundle::RangeIterator iter = newBundle->rangesBegin(); iter;
         iter++) {
      LiveRange* range = *iter;
      if (!range->vreg().addRange(range)) {
        return false;
      }
    }
  }

  // Queue the new bundles for register assignment.
  for (size_t i = 0; i < newBundles.length(); i++) {
    LiveBundle* newBundle = newBundles[i];
    size_t priority = computePriority(newBundle);
    if (!allocationQueue.insert(QueueItem(newBundle, priority))) {
      return false;
    }
  }

  return true;
}

// Helper for ::splitAt
// When splitting a bundle according to a list of split positions, return
// whether a use or range at |pos| should use a different bundle than the last
// position this was called for.
static bool UseNewBundle(const SplitPositionVector& splitPositions,
                         CodePosition pos, size_t* activeSplitPosition) {
  if (splitPositions.empty()) {
    // When the split positions are empty we are splitting at all uses.
    return true;
  }

  if (*activeSplitPosition == splitPositions.length()) {
    // We've advanced past all split positions.
    return false;
  }

  if (splitPositions[*activeSplitPosition] > pos) {
    // We haven't gotten to the next split position yet.
    return false;
  }

  // We've advanced past the next split position, find the next one which we
  // should split at.
  while (*activeSplitPosition < splitPositions.length() &&
         splitPositions[*activeSplitPosition] <= pos) {
    (*activeSplitPosition)++;
  }
  return true;
}

// Helper for ::splitAt
static bool HasPrecedingRangeSharingVreg(LiveBundle* bundle, LiveRange* range) {
  MOZ_ASSERT(range->bundle() == bundle);

  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* prevRange = *iter;
    if (prevRange == range) {
      return false;
    }
    if (&prevRange->vreg() == &range->vreg()) {
      return true;
    }
  }

  MOZ_CRASH();
}

// Helper for ::splitAt
static bool HasFollowingRangeSharingVreg(LiveBundle* bundle, LiveRange* range) {
  MOZ_ASSERT(range->bundle() == bundle);

  LiveBundle::RangeIterator iter = bundle->rangesBegin(range);
  MOZ_ASSERT(*iter == range);
  iter++;

  for (; iter; iter++) {
    LiveRange* nextRange = *iter;
    if (&nextRange->vreg() == &range->vreg()) {
      return true;
    }
  }

  return false;
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Implementation of splitting decisions, but not the making of those        //
// decisions:                                                                //
//   ::splitAt                                                               //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

// ::splitAt
// ---------
// It would be nice to be able to interpret ::splitAt as simply performing
// whatever split the heuristic routines decide on.  Unfortunately it
// tries to "improve" on the initial locations, which as
// https://bugzilla.mozilla.org/show_bug.cgi?id=1758274#c17 shows, often
// leads to excessive spilling.  So there is no clean distinction between
// policy (where to split, computed by the heuristic routines) and
// implementation (done by ::splitAt).
//
// ::splitAt -- creation of spill parent bundles
// ---------------------------------------------
// To understand what ::splitAt does, we must refer back to Section 1's
// description of LiveBundle::spillParent_.
//
// Initially (as created by Phase 1), all bundles have `spillParent_` being
// NULL.  If ::splitAt is asked to split such a bundle, it will first create a
// "spill bundle" or "spill parent" bundle.  This is a copy of the original,
// with two changes:
//
// * all register uses have been removed, so that only stack-compatible uses
//   remain.
//
// * for all LiveRanges in the bundle that define a register, the start point
//   of the range is moved one CodePosition forwards, thusly:
//
//   from = minimalDefEnd(insData[from]).next();
//
// The reason for the latter relates to the idea described in Section 1, that
// all LiveRanges for any given VirtualRegister must form a tree rooted at the
// defining LiveRange.  If the spill-bundle definition range start points are
// the same as those in the original bundle, then we will end up with two roots
// for the tree, and it is then unclear which one should supply "the value".
//
// Putting the spill-bundle start point one CodePosition further along causes
// the range containing the register def (after splitting) to still be the
// defining point.  ::createMoveGroupsFromLiveRangeTransitions will observe the
// equivalent spill-bundle range starting one point later and add a MoveGroup
// to move the value into it.  Since the spill bundle is intended to be stack
// resident, the effect is to force creation of the MoveGroup that will
// actually spill this value onto the stack.
//
// If the bundle provided to ::splitAt already has a spill parent, then
// ::splitAt doesn't create a new spill parent.  This situation will happen
// when the bundle to be split was itself created by splitting.  The effect is
// that *all* bundles created from an "original bundle" share the same spill
// parent, and hence they will share the same spill slot, which guarantees that
// all the spilled fragments of a VirtualRegister share the same spill slot,
// which means we'll never have to move a VirtualRegister between different
// spill slots during its lifetime.
//
// ::splitAt -- creation of other bundles
// --------------------------------------
// With the spill parent bundle question out of the way, ::splitAt then goes on
// to create the remaining new bundles, using near-incomprehensible logic
// steered by `UseNewBundle`.
//
// This supposedly splits the bundle at the positions given by the
// `SplitPositionVector` parameter to ::splitAt, putting them in a temporary
// vector `newBundles`.  Whether it really splits at the requested positions or
// not is hard to say; more important is what happens next.
//
// ::splitAt -- "improvement" ("filtering") of the split bundles
// -------------------------------------------------------------
// ::splitAt now tries to reduce the length of the LiveRanges that make up the
// new bundles (not including the "spill parent").  I assume this is to remove
// sections of the bundles that carry no useful value (eg, extending after the
// last using a range), thereby removing the demand for registers in those
// parts.  This does however mean that ::splitAt is no longer really splitting
// where the heuristic routines wanted, and that can lead to a big increase in
// spilling in loops, as
// https://bugzilla.mozilla.org/show_bug.cgi?id=1758274#c17 describes.
//
// ::splitAt -- meaning of the incoming `SplitPositionVector`
// ----------------------------------------------------------
// ::splitAt has one last mystery which is important to document.  The split
// positions are specified as CodePositions, but this leads to ambiguity
// because, in a sequence of N (LIR) instructions, there are 2N valid
// CodePositions.  For example:
//
//    6-7 WasmLoadTls [def v2<o>] [use v1:R]
//    8-9 WasmNullConstant [def v3<o>]
//
// Consider splitting the range for `v2`, which starts at CodePosition 7.
// What's the difference between saying "split it at 7" and "split it at 8" ?
// Not much really, since in both cases what we intend is for the range to be
// split in between the two instructions.
//
// Hence I believe the semantics is:
//
// * splitting at an even numbered CodePosition (eg, 8), which is an input-side
//   position, means "split before the instruction containing this position".
//
// * splitting at an odd numbered CodePositin (eg, 7), which is an output-side
//   position, means "split after the instruction containing this position".
//
// Hence in the example, we could specify either 7 or 8 to mean the same
// placement of the split.  Well, almost true, but actually:
//
// (SPEC) specifying 8 means
//
//   "split between these two insns, and any resulting MoveGroup goes in the
//    list to be emitted before the start of the second insn"
//
// (SPEC) specifying 7 means
//
//   "split between these two insns, and any resulting MoveGroup goes in the
//    list to be emitted after the end of the first insn"
//
// In most cases we don't care on which "side of the valley" the MoveGroup ends
// up, in which case we can use either convention.
//
// (SPEC) I believe these semantics are implied by the logic in
// ::createMoveGroupsFromLiveRangeTransitions.  They are certainly not
// documented anywhere in the code.

bool BacktrackingAllocator::splitAt(LiveBundle* bundle,
                                    const SplitPositionVector& splitPositions) {
  // Split the bundle at the given split points. Register uses which have no
  // intervening split points are consolidated into the same bundle. If the
  // list of split points is empty, then all register uses are placed in
  // minimal bundles.

  // splitPositions should be sorted.
  for (size_t i = 1; i < splitPositions.length(); ++i) {
    MOZ_ASSERT(splitPositions[i - 1] < splitPositions[i]);
  }

  // We don't need to create a new spill bundle if there already is one.
  bool spillBundleIsNew = false;
  LiveBundle* spillBundle = bundle->spillParent();
  if (!spillBundle) {
    spillBundle = LiveBundle::FallibleNew(alloc(), bundle->spillSet(), nullptr,
                                          getNextBundleId());
    if (!spillBundle) {
      return false;
    }
    spillBundleIsNew = true;

    for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
      LiveRange* range = *iter;

      CodePosition from = range->from();
      if (isRegisterDefinition(range)) {
        from = minimalDefEnd(insData[from]).next();
      }

      if (from < range->to()) {
        if (!spillBundle->addRangeAtEnd(alloc(), &range->vreg(), from,
                                        range->to())) {
          return false;
        }

        if (range->hasDefinition() && !isRegisterDefinition(range)) {
          spillBundle->lastRange()->setHasDefinition();
        }
      }
    }
  }

  LiveBundleVector newBundles;

  // The bundle which ranges are currently being added to.
  LiveBundle* activeBundle = LiveBundle::FallibleNew(
      alloc(), bundle->spillSet(), spillBundle, getNextBundleId());
  if (!activeBundle || !newBundles.append(activeBundle)) {
    return false;
  }

  // State for use by UseNewBundle.
  size_t activeSplitPosition = 0;

  // Make new bundles according to the split positions, and distribute ranges
  // and uses to them.
  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;

    if (UseNewBundle(splitPositions, range->from(), &activeSplitPosition)) {
      activeBundle = LiveBundle::FallibleNew(alloc(), bundle->spillSet(),
                                             spillBundle, getNextBundleId());
      if (!activeBundle || !newBundles.append(activeBundle)) {
        return false;
      }
    }

    LiveRange* activeRange = LiveRange::FallibleNew(alloc(), &range->vreg(),
                                                    range->from(), range->to());
    if (!activeRange) {
      return false;
    }
    activeBundle->addRangeAtEnd(activeRange);

    if (isRegisterDefinition(range)) {
      activeRange->setHasDefinition();
    }

    while (range->hasUses()) {
      UsePosition* use = range->popUse();
      LNode* ins = insData[use->pos];

      // Any uses of a register that appear before its definition has
      // finished must be associated with the range for that definition.
      if (isRegisterDefinition(range) &&
          use->pos <= minimalDefEnd(insData[range->from()])) {
        activeRange->addUse(use);
      } else if (isRegisterUse(use, ins)) {
        // Place this register use into a different bundle from the
        // last one if there are any split points between the two uses.
        // UseNewBundle always returns true if we are splitting at all
        // register uses, but we can still reuse the last range and
        // bundle if they have uses at the same position, except when
        // either use is fixed (the two uses might require incompatible
        // registers.)
        if (UseNewBundle(splitPositions, use->pos, &activeSplitPosition) &&
            (!activeRange->hasUses() ||
             activeRange->usesBegin()->pos != use->pos ||
             activeRange->usesBegin()->usePolicy() == LUse::FIXED ||
             use->usePolicy() == LUse::FIXED)) {
          activeBundle = LiveBundle::FallibleNew(
              alloc(), bundle->spillSet(), spillBundle, getNextBundleId());
          if (!activeBundle || !newBundles.append(activeBundle)) {
            return false;
          }
          activeRange = LiveRange::FallibleNew(alloc(), &range->vreg(),
                                               range->from(), range->to());
          if (!activeRange) {
            return false;
          }
          activeBundle->addRangeAtEnd(activeRange);
        }

        activeRange->addUse(use);
      } else {
        MOZ_ASSERT(spillBundleIsNew);
        spillBundle->rangeFor(use->pos)->addUse(use);
      }
    }
  }

  LiveBundleVector filteredBundles;

  // Trim the ends of ranges in each new bundle when there are no other
  // earlier or later ranges in the same bundle with the same vreg.
  for (size_t i = 0; i < newBundles.length(); i++) {
    LiveBundle* bundle = newBundles[i];

    for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter;) {
      LiveRange* range = *iter;

      if (!range->hasDefinition()) {
        if (!HasPrecedingRangeSharingVreg(bundle, range)) {
          if (range->hasUses()) {
            UsePosition* use = *range->usesBegin();
            range->setFrom(inputOf(insData[use->pos]));
          } else {
            bundle->removeRangeAndIncrementIterator(iter);
            continue;
          }
        }
      }

      if (!HasFollowingRangeSharingVreg(bundle, range)) {
        if (range->hasUses()) {
          UsePosition* use = range->lastUse();
          range->setTo(use->pos.next());
        } else if (range->hasDefinition()) {
          range->setTo(minimalDefEnd(insData[range->from()]).next());
        } else {
          bundle->removeRangeAndIncrementIterator(iter);
          continue;
        }
      }

      iter++;
    }

    if (bundle->hasRanges() && !filteredBundles.append(bundle)) {
      return false;
    }
  }

  if (spillBundleIsNew && !filteredBundles.append(spillBundle)) {
    return false;
  }

  return updateVirtualRegisterListsThenRequeueBundles(bundle, filteredBundles);
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Creation of splitting decisions, but not their implementation:            //
//   ::splitAcrossCalls                                                      //
//   ::trySplitAcrossHotcode                                                 //
//   ::trySplitAfterLastRegisterUse                                          //
//   ::trySplitBeforeFirstRegisterUse                                        //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

bool BacktrackingAllocator::splitAcrossCalls(LiveBundle* bundle) {
  // Split the bundle to separate register uses and non-register uses and
  // allow the vreg to be spilled across its range.

  // Locations of all calls in the bundle's range. This vector is sorted in
  // ascending order because ranges in the bundle are sorted by start position
  // and don't overlap
  SplitPositionVector bundleCallPositions;

  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;

    // Append call positions in range [from+1, to) to bundleCallPositions.
    // Calls at the beginning of the range are ignored because there is no
    // splitting to do in this case.

    CodePosition from = range->from().next();
    if (from == range->to()) {
      // Empty range can't contain any calls.
      continue;
    }

    // Use binary search to find the first call position.
    Maybe<size_t> index = lookupFirstCallPositionInRange(from, range->to());
    if (!index.isSome()) {
      // There are no calls inside this range.
      continue;
    }

    // Find the index of the last call within this range.
    size_t startIndex = *index;
    size_t endIndex = startIndex;
    while (endIndex < callPositions.length() - 1) {
      if (callPositions[endIndex + 1] >= range->to()) {
        break;
      }
      endIndex++;
    }

    MOZ_ASSERT(startIndex <= endIndex);

#ifdef DEBUG
    auto inRange = [range](CodePosition pos) {
      return range->covers(pos) && pos != range->from();
    };

    // Assert startIndex is the first call position in this range.
    MOZ_ASSERT(inRange(callPositions[startIndex]));
    MOZ_ASSERT_IF(startIndex > 0, !inRange(callPositions[startIndex - 1]));

    // Assert endIndex is the last call position in this range.
    MOZ_ASSERT(inRange(callPositions[endIndex]));
    MOZ_ASSERT_IF(endIndex + 1 < callPositions.length(),
                  !inRange(callPositions[endIndex + 1]));

    // Assert bundleCallPositions stays sorted.
    MOZ_ASSERT_IF(!bundleCallPositions.empty(),
                  bundleCallPositions.back() < callPositions[startIndex]);
#endif

    const CodePosition* start = &callPositions[startIndex];
    size_t count = endIndex - startIndex + 1;
    if (!bundleCallPositions.append(start, start + count)) {
      return false;
    }
  }

  MOZ_ASSERT(!bundleCallPositions.empty());

#ifdef JS_JITSPEW
  JitSpewStart(JitSpew_RegAlloc, "  .. split across calls at ");
  for (size_t i = 0; i < bundleCallPositions.length(); ++i) {
    JitSpewCont(JitSpew_RegAlloc, "%s%u", i != 0 ? ", " : "",
                bundleCallPositions[i].bits());
  }
  JitSpewFin(JitSpew_RegAlloc);
#endif

  return splitAt(bundle, bundleCallPositions);
}

bool BacktrackingAllocator::trySplitAcrossHotcode(LiveBundle* bundle,
                                                  bool* success) {
  // If this bundle has portions that are hot and portions that are cold,
  // split it at the boundaries between hot and cold code.

  LiveRange* hotRange = nullptr;

  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;
    if (hotcode.contains(range, &hotRange)) {
      break;
    }
  }

  // Don't split if there is no hot code in the bundle.
  if (!hotRange) {
    JitSpew(JitSpew_RegAlloc, "  .. bundle does not contain hot code");
    return true;
  }

  // Don't split if there is no cold code in the bundle.
  bool coldCode = false;
  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;
    if (!hotRange->contains(range)) {
      coldCode = true;
      break;
    }
  }
  if (!coldCode) {
    JitSpew(JitSpew_RegAlloc, "  .. bundle does not contain cold code");
    return true;
  }

  JitSpewIfEnabled(JitSpew_RegAlloc, "  .. split across hot range %s",
                   hotRange->toString().get());

  // Tweak the splitting method when compiling wasm code to look at actual
  // uses within the hot/cold code. This heuristic is in place as the below
  // mechanism regresses several asm.js tests. Hopefully this will be fixed
  // soon and this special case removed. See bug 948838.
  if (compilingWasm()) {
    SplitPositionVector splitPositions;
    if (!splitPositions.append(hotRange->from()) ||
        !splitPositions.append(hotRange->to())) {
      return false;
    }
    *success = true;
    return splitAt(bundle, splitPositions);
  }

  LiveBundle* hotBundle = LiveBundle::FallibleNew(
      alloc(), bundle->spillSet(), bundle->spillParent(), getNextBundleId());
  if (!hotBundle) {
    return false;
  }
  LiveBundle* preBundle = nullptr;
  LiveBundle* postBundle = nullptr;

  // Accumulate the ranges of hot and cold code in the bundle. Note that
  // we are only comparing with the single hot range found, so the cold code
  // may contain separate hot ranges.
  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;
    LiveRange::Range hot, coldPre, coldPost;
    range->intersect(hotRange, &coldPre, &hot, &coldPost);

    if (!hot.empty()) {
      if (!hotBundle->addRangeAndDistributeUses(alloc(), range, hot.from,
                                                hot.to)) {
        return false;
      }
    }

    if (!coldPre.empty()) {
      if (!preBundle) {
        preBundle =
            LiveBundle::FallibleNew(alloc(), bundle->spillSet(),
                                    bundle->spillParent(), getNextBundleId());
        if (!preBundle) {
          return false;
        }
      }
      if (!preBundle->addRangeAndDistributeUses(alloc(), range, coldPre.from,
                                                coldPre.to)) {
        return false;
      }
    }

    if (!coldPost.empty()) {
      if (!postBundle) {
        postBundle =
            LiveBundle::FallibleNew(alloc(), bundle->spillSet(),
                                    bundle->spillParent(), getNextBundleId());
        if (!postBundle) {
          return false;
        }
      }
      if (!postBundle->addRangeAndDistributeUses(
              alloc(), range, coldPost.from, coldPost.to)) {
        return false;
      }
    }
  }

  MOZ_ASSERT(hotBundle->numRanges() != 0);

  LiveBundleVector newBundles;
  if (!newBundles.append(hotBundle)) {
    return false;
  }

  MOZ_ASSERT(preBundle || postBundle);
  if (preBundle && !newBundles.append(preBundle)) {
    return false;
  }
  if (postBundle && !newBundles.append(postBundle)) {
    return false;
  }

  *success = true;
  return updateVirtualRegisterListsThenRequeueBundles(bundle, newBundles);
}

bool BacktrackingAllocator::trySplitAfterLastRegisterUse(LiveBundle* bundle,
                                                         LiveBundle* conflict,
                                                         bool* success) {
  // If this bundle's later uses do not require it to be in a register,
  // split it after the last use which does require a register. If conflict
  // is specified, only consider register uses before the conflict starts.

  CodePosition lastRegisterFrom, lastRegisterTo, lastUse;

  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;

    // If the range defines a register, consider that a register use for
    // our purposes here.
    if (isRegisterDefinition(range)) {
      CodePosition spillStart = minimalDefEnd(insData[range->from()]).next();
      if (!conflict || spillStart < conflict->firstRange()->from()) {
        lastUse = lastRegisterFrom = range->from();
        lastRegisterTo = spillStart;
      }
    }

    for (UsePositionIterator iter(range->usesBegin()); iter; iter++) {
      LNode* ins = insData[iter->pos];

      // Uses in the bundle should be sorted.
      MOZ_ASSERT(iter->pos >= lastUse);
      lastUse = inputOf(ins);

      if (!conflict || outputOf(ins) < conflict->firstRange()->from()) {
        if (isRegisterUse(*iter, ins, /* considerCopy = */ true)) {
          lastRegisterFrom = inputOf(ins);
          lastRegisterTo = iter->pos.next();
        }
      }
    }
  }

  // Can't trim non-register uses off the end by splitting.
  if (!lastRegisterFrom.bits()) {
    JitSpew(JitSpew_RegAlloc, "  .. bundle has no register uses");
    return true;
  }
  if (lastUse < lastRegisterTo) {
    JitSpew(JitSpew_RegAlloc, "  .. bundle's last use is a register use");
    return true;
  }

  JitSpewIfEnabled(JitSpew_RegAlloc, "  .. split after last register use at %u",
                   lastRegisterTo.bits());

  SplitPositionVector splitPositions;
  if (!splitPositions.append(lastRegisterTo)) {
    return false;
  }
  *success = true;
  return splitAt(bundle, splitPositions);
}

bool BacktrackingAllocator::trySplitBeforeFirstRegisterUse(LiveBundle* bundle,
                                                           LiveBundle* conflict,
                                                           bool* success) {
  // If this bundle's earlier uses do not require it to be in a register,
  // split it before the first use which does require a register. If conflict
  // is specified, only consider register uses after the conflict ends.

  if (isRegisterDefinition(bundle->firstRange())) {
    JitSpew(JitSpew_RegAlloc, "  .. bundle is defined by a register");
    return true;
  }
  if (!bundle->firstRange()->hasDefinition()) {
    JitSpew(JitSpew_RegAlloc, "  .. bundle does not have definition");
    return true;
  }

  CodePosition firstRegisterFrom;

  CodePosition conflictEnd;
  if (conflict) {
    for (LiveBundle::RangeIterator iter = conflict->rangesBegin(); iter;
         iter++) {
      LiveRange* range = *iter;
      if (range->to() > conflictEnd) {
        conflictEnd = range->to();
      }
    }
  }

  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;

    if (!conflict || range->from() > conflictEnd) {
      if (range->hasDefinition() && isRegisterDefinition(range)) {
        firstRegisterFrom = range->from();
        break;
      }
    }

    for (UsePositionIterator iter(range->usesBegin()); iter; iter++) {
      LNode* ins = insData[iter->pos];

      if (!conflict || outputOf(ins) >= conflictEnd) {
        if (isRegisterUse(*iter, ins, /* considerCopy = */ true)) {
          firstRegisterFrom = inputOf(ins);
          break;
        }
      }
    }
    if (firstRegisterFrom.bits()) {
      break;
    }
  }

  if (!firstRegisterFrom.bits()) {
    // Can't trim non-register uses off the beginning by splitting.
    JitSpew(JitSpew_RegAlloc, "  bundle has no register uses");
    return true;
  }

  JitSpewIfEnabled(JitSpew_RegAlloc,
                   "  .. split before first register use at %u",
                   firstRegisterFrom.bits());

  SplitPositionVector splitPositions;
  if (!splitPositions.append(firstRegisterFrom)) {
    return false;
  }
  *success = true;
  return splitAt(bundle, splitPositions);
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// The top level driver for the splitting machinery                          //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

// ::chooseBundleSplit
// -------------------
// If the first allocation loop (in ::go) can't allocate a bundle, it hands it
// off to ::chooseBundleSplit, which is the "entry point" of the bundle-split
// machinery.  This tries four heuristics in turn, to see if any can split the
// bundle:
//
// * ::trySplitAcrossHotcode
// * ::splitAcrossCalls (in some cases)
// * ::trySplitBeforeFirstRegisterUse
// * ::trySplitAfterLastRegisterUse
//
// These routines have similar structure: they try to decide on one or more
// CodePositions at which to split the bundle, using whatever heuristics they
// have to hand.  If suitable CodePosition(s) are found, they are put into a
// `SplitPositionVector`, and the bundle and the vector are handed off to
// ::splitAt, which performs the split (at least in theory) at the chosen
// positions.  It also arranges for the new bundles to be added to
// ::allocationQueue.
//
// ::trySplitAcrossHotcode has a special case for JS -- it modifies the
// bundle(s) itself, rather than using ::splitAt.
//
// If none of the heuristic routines apply, then ::splitAt is called with an
// empty vector of split points.  This is interpreted to mean "split at all
// register uses".  When combined with how ::splitAt works, the effect is to
// spill the bundle.

bool BacktrackingAllocator::chooseBundleSplit(LiveBundle* bundle, bool hasCall,
                                              LiveBundle* conflict) {
  bool success = false;

  JitSpewIfEnabled(JitSpew_RegAlloc, "  Splitting %s ..",
                   bundle->toString().get());

  if (!trySplitAcrossHotcode(bundle, &success)) {
    return false;
  }
  if (success) {
    return true;
  }

  if (hasCall) {
    return splitAcrossCalls(bundle);
  }

  if (!trySplitBeforeFirstRegisterUse(bundle, conflict, &success)) {
    return false;
  }
  if (success) {
    return true;
  }

  if (!trySplitAfterLastRegisterUse(bundle, conflict, &success)) {
    return false;
  }
  if (success) {
    return true;
  }

  // Split at all register uses.
  SplitPositionVector emptyPositions;
  return splitAt(bundle, emptyPositions);
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Bundle allocation                                                         //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

static const size_t MAX_ATTEMPTS = 2;

bool BacktrackingAllocator::computeRequirement(LiveBundle* bundle,
                                               Requirement* requirement,
                                               Requirement* hint) {
  // Set any requirement or hint on bundle according to its definition and
  // uses. Return false if there are conflicting requirements which will
  // require the bundle to be split.

  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;
    VirtualRegister& reg = range->vreg();

    if (range->hasDefinition()) {
      // Deal with any definition constraints/hints.
      LDefinition::Policy policy = reg.def()->policy();
      if (policy == LDefinition::FIXED || policy == LDefinition::STACK) {
        // Fixed and stack policies get a FIXED requirement.  (In the stack
        // case, the allocation should have been performed already by
        // mergeAndQueueRegisters.)
        JitSpewIfEnabled(JitSpew_RegAlloc,
                         "  Requirement %s, fixed by definition",
                         reg.def()->output()->toString().get());
        if (!requirement->merge(Requirement(*reg.def()->output()))) {
          return false;
        }
      } else if (reg.ins()->isPhi()) {
        // Phis don't have any requirements, but they should prefer their
        // input allocations. This is captured by the group hints above.
      } else {
        // Non-phis get a REGISTER requirement.
        if (!requirement->merge(Requirement(Requirement::REGISTER))) {
          return false;
        }
      }
    }

    // Search uses for requirements.
    for (UsePositionIterator iter = range->usesBegin(); iter; iter++) {
      LUse::Policy policy = iter->usePolicy();
      if (policy == LUse::FIXED) {
        AnyRegister required = GetFixedRegister(reg.def(), iter->use());

        JitSpewIfEnabled(JitSpew_RegAlloc, "  Requirement %s, due to use at %u",
                         required.name(), iter->pos.bits());

        // If there are multiple fixed registers which the bundle is
        // required to use, fail. The bundle will need to be split before
        // it can be allocated.
        if (!requirement->merge(Requirement(LAllocation(required)))) {
          return false;
        }
      } else if (policy == LUse::REGISTER) {
        if (!requirement->merge(Requirement(Requirement::REGISTER))) {
          return false;
        }
      } else if (policy == LUse::ANY) {
        // ANY differs from KEEPALIVE by actively preferring a register.
        if (!hint->merge(Requirement(Requirement::REGISTER))) {
          return false;
        }
      }

      // The only case of STACK use policies is individual stack results using
      // their containing stack result area, which is given a fixed allocation
      // above.
      MOZ_ASSERT_IF(policy == LUse::STACK,
                    requirement->kind() == Requirement::FIXED);
      MOZ_ASSERT_IF(policy == LUse::STACK,
                    requirement->allocation().isStackArea());
    }
  }

  return true;
}

bool BacktrackingAllocator::tryAllocateRegister(PhysicalRegister& r,
                                                LiveBundle* bundle,
                                                bool* success, bool* hasCall,
                                                LiveBundleVector& conflicting) {
  *success = false;

  // If we know this bundle contains a call (where all registers are spilled) we
  // shouldn't try again to allocate a register.
  MOZ_ASSERT(!*hasCall);

  if (!r.allocatable) {
    return true;
  }

  LiveBundleVector aliasedConflicting;

  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;
    LiveRangePlus rangePlus(range);

    // All ranges in the bundle must be compatible with the physical register.
    MOZ_ASSERT(range->vreg().isCompatible(r.reg));

    const size_t numAliased = r.reg.numAliased();
    for (size_t a = 0; a < numAliased; a++) {
      PhysicalRegister& rAlias = registers[r.reg.aliased(a).code()];
      LiveRangePlus existingPlus;
      if (!rAlias.allocations.contains(rangePlus, &existingPlus)) {
        continue;
      }
      const LiveRange* existing = existingPlus.liveRange();
      if (existing->hasVreg()) {
        MOZ_ASSERT(existing->bundle()->allocation().toAnyRegister() ==
                   rAlias.reg);
        bool duplicate = false;
        for (size_t i = 0; i < aliasedConflicting.length(); i++) {
          if (aliasedConflicting[i] == existing->bundle()) {
            duplicate = true;
            break;
          }
        }
        if (!duplicate && !aliasedConflicting.append(existing->bundle())) {
          return false;
        }
      } else {
        // This bundle contains a call instruction.
        MOZ_ASSERT(lookupFirstCallPositionInRange(range->from(), range->to()));
        JitSpewIfEnabled(JitSpew_RegAlloc, "  %s collides with fixed use %s",
                         rAlias.reg.name(), existing->toString().get());
        *hasCall = true;
        return true;
      }
      MOZ_ASSERT(r.reg.numAliased() == numAliased);
    }
  }

  if (!aliasedConflicting.empty()) {
    // One or more aliased registers is allocated to another bundle
    // overlapping this one. Keep track of the conflicting set, and in the
    // case of multiple conflicting sets keep track of the set with the
    // lowest maximum spill weight.

    // The #ifdef guards against "unused variable 'existing'" bustage.
#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_RegAlloc)) {
      if (aliasedConflicting.length() == 1) {
        LiveBundle* existing = aliasedConflicting[0];
        JitSpew(JitSpew_RegAlloc, "  %s collides with %s [weight %zu]",
                r.reg.name(), existing->toString().get(),
                computeSpillWeight(existing));
      } else {
        JitSpew(JitSpew_RegAlloc, "  %s collides with the following",
                r.reg.name());
        for (size_t i = 0; i < aliasedConflicting.length(); i++) {
          LiveBundle* existing = aliasedConflicting[i];
          JitSpew(JitSpew_RegAlloc, "    %s [weight %zu]",
                  existing->toString().get(), computeSpillWeight(existing));
        }
      }
    }
#endif

    if (conflicting.empty()) {
      conflicting = std::move(aliasedConflicting);
    } else {
      if (maximumSpillWeight(aliasedConflicting) <
          maximumSpillWeight(conflicting)) {
        conflicting = std::move(aliasedConflicting);
      }
    }
    return true;
  }

  JitSpewIfEnabled(JitSpew_RegAlloc, "  allocated to %s", r.reg.name());

  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;
    if (!alloc().ensureBallast()) {
      return false;
    }
    LiveRangePlus rangePlus(range);
    if (!r.allocations.insert(rangePlus)) {
      return false;
    }
  }

  bundle->setAllocation(LAllocation(r.reg));
  *success = true;
  return true;
}

bool BacktrackingAllocator::tryAllocateAnyRegister(
    LiveBundle* bundle, bool* success, bool* hasCall,
    LiveBundleVector& conflicting) {
  // Search for any available register which the bundle can be allocated to.

  LDefinition::Type type = bundle->firstRange()->vreg().type();

  if (LDefinition::isFloatReg(type)) {
    for (size_t i = AnyRegister::FirstFloatReg; i < AnyRegister::Total; i++) {
      if (!LDefinition::isFloatRegCompatible(type, registers[i].reg.fpu())) {
        continue;
      }
      if (!tryAllocateRegister(registers[i], bundle, success, hasCall,
                               conflicting)) {
        return false;
      }
      if (*success) {
        break;
      }
      if (*hasCall) {
        // This bundle contains a call instruction. Calls require spilling all
        // registers, so we have to split or spill this bundle.
        break;
      }
    }
    return true;
  }

  for (size_t i = 0; i < AnyRegister::FirstFloatReg; i++) {
    if (!tryAllocateRegister(registers[i], bundle, success, hasCall,
                             conflicting)) {
      return false;
    }
    if (*success) {
      break;
    }
    if (*hasCall) {
      // This bundle contains a call instruction. Calls require spilling all
      // registers, so we have to split or spill this bundle.
      break;
    }
  }
  return true;
}

bool BacktrackingAllocator::evictBundle(LiveBundle* bundle) {
  JitSpewIfEnabled(JitSpew_RegAlloc,
                   "  Evicting %s [priority %zu] [weight %zu]",
                   bundle->toString().get(), computePriority(bundle),
                   computeSpillWeight(bundle));

  AnyRegister reg(bundle->allocation().toAnyRegister());
  PhysicalRegister& physical = registers[reg.code()];
  MOZ_ASSERT(physical.reg == reg && physical.allocatable);

  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;
    LiveRangePlus rangePlus(range);
    physical.allocations.remove(rangePlus);
  }

  bundle->setAllocation(LAllocation());

  size_t priority = computePriority(bundle);
  return allocationQueue.insert(QueueItem(bundle, priority));
}

bool BacktrackingAllocator::tryAllocateFixed(LiveBundle* bundle,
                                             Requirement requirement,
                                             bool* success, bool* hasCall,
                                             LiveBundleVector& conflicting) {
  // Spill bundles which are required to be in a certain stack slot.
  if (!requirement.allocation().isAnyRegister()) {
    JitSpew(JitSpew_RegAlloc, "  stack allocation requirement");
    bundle->setAllocation(requirement.allocation());
    *success = true;
    return true;
  }

  AnyRegister reg = requirement.allocation().toAnyRegister();
  return tryAllocateRegister(registers[reg.code()], bundle, success, hasCall,
                             conflicting);
}

bool BacktrackingAllocator::tryAllocateNonFixed(LiveBundle* bundle,
                                                Requirement requirement,
                                                Requirement hint, bool* success,
                                                bool* hasCall,
                                                LiveBundleVector& conflicting) {
  MOZ_ASSERT(hint.kind() != Requirement::FIXED);
  MOZ_ASSERT(conflicting.empty());

  // Spill bundles which have no hint or register requirement.
  if (requirement.kind() == Requirement::NONE &&
      hint.kind() == Requirement::NONE) {
    JitSpew(JitSpew_RegAlloc,
            "  postponed spill (no hint or register requirement)");
    if (!spilledBundles.append(bundle)) {
      return false;
    }
    *success = true;
    return true;
  }

  if (!tryAllocateAnyRegister(bundle, success, hasCall, conflicting)) {
    return false;
  }
  if (*success) {
    return true;
  }

  // Spill bundles which have no register requirement if they didn't get
  // allocated.
  if (requirement.kind() == Requirement::NONE) {
    JitSpew(JitSpew_RegAlloc, "  postponed spill (no register requirement)");
    if (!spilledBundles.append(bundle)) {
      return false;
    }
    *success = true;
    return true;
  }

  // We failed to allocate this bundle.
  MOZ_ASSERT(!*success);
  return true;
}

bool BacktrackingAllocator::processBundle(const MIRGenerator* mir,
                                          LiveBundle* bundle) {
  JitSpewIfEnabled(JitSpew_RegAlloc,
                   "Allocating %s [priority %zu] [weight %zu]",
                   bundle->toString().get(), computePriority(bundle),
                   computeSpillWeight(bundle));

  // A bundle can be processed by doing any of the following:
  //
  // - Assigning the bundle a register. The bundle cannot overlap any other
  //   bundle allocated for that physical register.
  //
  // - Spilling the bundle, provided it has no register uses.
  //
  // - Splitting the bundle into two or more bundles which cover the original
  //   one. The new bundles are placed back onto the priority queue for later
  //   processing.
  //
  // - Evicting one or more existing allocated bundles, and then doing one
  //   of the above operations. Evicted bundles are placed back on the
  //   priority queue. Any evicted bundles must have a lower spill weight
  //   than the bundle being processed.
  //
  // As long as this structure is followed, termination is guaranteed.
  // In general, we want to minimize the amount of bundle splitting (which
  // generally necessitates spills), so allocate longer lived, lower weight
  // bundles first and evict and split them later if they prevent allocation
  // for higher weight bundles.

  Requirement requirement, hint;
  bool doesNotHaveFixedConflict =
      computeRequirement(bundle, &requirement, &hint);

  bool hasCall = false;
  LiveBundleVector conflicting;

  if (doesNotHaveFixedConflict) {
    for (size_t attempt = 0;; attempt++) {
      if (mir->shouldCancel("Backtracking Allocation (processBundle loop)")) {
        return false;
      }

      bool success = false;
      hasCall = false;
      conflicting.clear();

      // Ok, let's try allocating for this bundle.
      if (requirement.kind() == Requirement::FIXED) {
        if (!tryAllocateFixed(bundle, requirement, &success, &hasCall,
                              conflicting)) {
          return false;
        }
      } else {
        if (!tryAllocateNonFixed(bundle, requirement, hint, &success, &hasCall,
                                 conflicting)) {
          return false;
        }
      }

      // If that worked, we're done!
      if (success) {
        return true;
      }

      // If that didn't work, but we have one or more non-call bundles known to
      // be conflicting, maybe we can evict them and try again.
      if ((attempt < MAX_ATTEMPTS || minimalBundle(bundle)) && !hasCall &&
          !conflicting.empty() &&
          maximumSpillWeight(conflicting) < computeSpillWeight(bundle)) {
        for (size_t i = 0; i < conflicting.length(); i++) {
          if (!evictBundle(conflicting[i])) {
            return false;
          }
        }
        continue;
      }

      // We have to split this bundle.
      break;
    }
  }

  // A minimal bundle cannot be split any further. If we try to split it
  // it at this point we will just end up with the same bundle and will
  // enter an infinite loop. Weights and the initial live ranges must
  // be constructed so that any minimal bundle is allocatable.
  MOZ_ASSERT(!minimalBundle(bundle));

  LiveBundle* conflict = conflicting.empty() ? nullptr : conflicting[0];
  return chooseBundleSplit(bundle, hasCall, conflict);
}

// Helper for ::tryAllocatingRegistersForSpillBundles
bool BacktrackingAllocator::spill(LiveBundle* bundle) {
  JitSpew(JitSpew_RegAlloc, "  Spilling bundle");
  MOZ_ASSERT(bundle->allocation().isBogus());

  if (LiveBundle* spillParent = bundle->spillParent()) {
    JitSpew(JitSpew_RegAlloc, "    Using existing spill bundle");
    for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
      LiveRange* range = *iter;
      LiveRange* parentRange = spillParent->rangeFor(range->from());
      MOZ_ASSERT(parentRange->contains(range));
      MOZ_ASSERT(&range->vreg() == &parentRange->vreg());
      range->tryToMoveDefAndUsesInto(parentRange);
    }
    bundle->removeAllRangesFromVirtualRegisters();
    return true;
  }

  return bundle->spillSet()->addSpilledBundle(bundle);
}

bool BacktrackingAllocator::tryAllocatingRegistersForSpillBundles() {
  for (auto it = spilledBundles.begin(); it != spilledBundles.end(); it++) {
    LiveBundle* bundle = *it;
    LiveBundleVector conflicting;
    bool hasCall = false;
    bool success = false;

    if (mir->shouldCancel("Backtracking Try Allocating Spilled Bundles")) {
      return false;
    }

    JitSpewIfEnabled(JitSpew_RegAlloc, "Spill or allocate %s",
                     bundle->toString().get());

    if (!tryAllocateAnyRegister(bundle, &success, &hasCall, conflicting)) {
      return false;
    }

    // If the bundle still has no register, spill the bundle.
    if (!success && !spill(bundle)) {
      return false;
    }
  }

  return true;
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Rewriting of the LIR after bundle processing is done:                     //
//   ::pickStackSlots                                                        //
//   ::createMoveGroupsFromLiveRangeTransitions                              //
//   ::installAllocationsInLIR                                               //
//   ::populateSafepoints                                                    //
//   ::annotateMoveGroups                                                    //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

// Helper for ::pickStackSlot
bool BacktrackingAllocator::insertAllRanges(LiveRangePlusSet& set,
                                            LiveBundle* bundle) {
  for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
    LiveRange* range = *iter;
    if (!alloc().ensureBallast()) {
      return false;
    }
    LiveRangePlus rangePlus(range);
    if (!set.insert(rangePlus)) {
      return false;
    }
  }
  return true;
}

void BacktrackingAllocator::sortVirtualRegisterRanges() {
  for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
    VirtualRegister& reg = vregs[i];
    reg.sortRanges();
  }
}

// Helper for ::pickStackSlots
bool BacktrackingAllocator::pickStackSlot(SpillSet* spillSet) {
  // Look through all ranges that have been spilled in this set for a
  // register definition which is fixed to a stack or argument slot. If we
  // find one, use it for all bundles that have been spilled. tryMergeBundles
  // makes sure this reuse is possible when an initial bundle contains ranges
  // from multiple virtual registers.
  for (size_t i = 0; i < spillSet->numSpilledBundles(); i++) {
    LiveBundle* bundle = spillSet->spilledBundle(i);
    for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter; iter++) {
      LiveRange* range = *iter;
      if (range->hasDefinition()) {
        LDefinition* def = range->vreg().def();
        if (def->policy() == LDefinition::FIXED) {
          MOZ_ASSERT(!def->output()->isAnyRegister());
          MOZ_ASSERT(!def->output()->isStackSlot());
          spillSet->setAllocation(*def->output());
          return true;
        }
      }
    }
  }

  LDefinition::Type type =
      spillSet->spilledBundle(0)->firstRange()->vreg().type();

  SpillSlotList* slotList;
  switch (LStackSlot::width(type)) {
    case LStackSlot::Word:
      slotList = &normalSlots;
      break;
    case LStackSlot::DoubleWord:
      slotList = &doubleSlots;
      break;
    case LStackSlot::QuadWord:
      slotList = &quadSlots;
      break;
    default:
      MOZ_CRASH("Bad width");
  }

  // Maximum number of existing spill slots we will look at before giving up
  // and allocating a new slot.
  static const size_t MAX_SEARCH_COUNT = 10;

  size_t searches = 0;
  SpillSlot* stop = nullptr;
  while (!slotList->empty()) {
    SpillSlot* spillSlot = *slotList->begin();
    if (!stop) {
      stop = spillSlot;
    } else if (stop == spillSlot) {
      // We looked through every slot in the list.
      break;
    }

    bool success = true;
    for (size_t i = 0; i < spillSet->numSpilledBundles(); i++) {
      LiveBundle* bundle = spillSet->spilledBundle(i);
      for (LiveBundle::RangeIterator iter = bundle->rangesBegin(); iter;
           iter++) {
        LiveRange* range = *iter;
        LiveRangePlus rangePlus(range);
        LiveRangePlus existingPlus;
        if (spillSlot->allocated.contains(rangePlus, &existingPlus)) {
          success = false;
          break;
        }
      }
      if (!success) {
        break;
      }
    }
    if (success) {
      // We can reuse this physical stack slot for the new bundles.
      // Update the allocated ranges for the slot.
      for (size_t i = 0; i < spillSet->numSpilledBundles(); i++) {
        LiveBundle* bundle = spillSet->spilledBundle(i);
        if (!insertAllRanges(spillSlot->allocated, bundle)) {
          return false;
        }
      }
      spillSet->setAllocation(spillSlot->alloc);
      return true;
    }

    // On a miss, move the spill to the end of the list. This will cause us
    // to make fewer attempts to allocate from slots with a large and
    // highly contended range.
    slotList->popFront();
    slotList->pushBack(spillSlot);

    if (++searches == MAX_SEARCH_COUNT) {
      break;
    }
  }

  // We need a new physical stack slot.
  LStackSlot::Width width = LStackSlot::width(type);
  uint32_t stackSlot = stackSlotAllocator.allocateSlot(width);

  SpillSlot* spillSlot =
      new (alloc().fallible()) SpillSlot(stackSlot, width, alloc().lifoAlloc());
  if (!spillSlot) {
    return false;
  }

  for (size_t i = 0; i < spillSet->numSpilledBundles(); i++) {
    LiveBundle* bundle = spillSet->spilledBundle(i);
    if (!insertAllRanges(spillSlot->allocated, bundle)) {
      return false;
    }
  }

  spillSet->setAllocation(spillSlot->alloc);

  slotList->pushFront(spillSlot);
  return true;
}

bool BacktrackingAllocator::pickStackSlots() {
  for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
    VirtualRegister& reg = vregs[i];

    if (mir->shouldCancel("Backtracking Pick Stack Slots")) {
      return false;
    }

    for (VirtualRegister::RangeIterator iter(reg); iter; iter++) {
      LiveBundle* bundle = iter->bundle();

      if (bundle->allocation().isBogus()) {
        if (!pickStackSlot(bundle->spillSet())) {
          return false;
        }
        MOZ_ASSERT(!bundle->allocation().isBogus());
      }
    }
  }

  return true;
}

// Helper for ::createMoveGroupsFromLiveRangeTransitions
bool BacktrackingAllocator::moveAtEdge(LBlock* predecessor, LBlock* successor,
                                       LiveRange* from, LiveRange* to,
                                       LDefinition::Type type) {
  if (successor->mir()->numPredecessors() > 1) {
    MOZ_ASSERT(predecessor->mir()->numSuccessors() == 1);
    return moveAtExit(predecessor, from, to, type);
  }

  return moveAtEntry(successor, from, to, type);
}

// Helper for ::createMoveGroupsFromLiveRangeTransitions
void BacktrackingAllocator::removeDeadRanges(VirtualRegister& reg) {
  auto isDeadRange = [&](VirtualRegister::RangeVector& ranges,
                         LiveRange* range) {
    // Check for direct uses of this range.
    if (range->hasUses() || range->hasDefinition()) {
      return false;
    }

    // Check if there are later ranges for this vreg. The first item in the list
    // must have the highest start position so we compare against that one.
    CodePosition start = range->from();
    CodePosition maxFrom = ranges[0]->from();
    if (maxFrom > start) {
      return false;
    }

    LNode* ins = insData[start];
    if (start == entryOf(ins->block())) {
      return false;
    }

    // Check if this range ends at a loop backedge.
    LNode* last = insData[range->to().previous()];
    if (last->isGoto() &&
        last->toGoto()->target()->id() < last->block()->mir()->id()) {
      return false;
    }

    // Check if there are phis which this vreg flows to.
    if (reg.usedByPhi()) {
      return false;
    }

    return true;
  };

  reg.removeRangesIf(isDeadRange);
}

static void AssertCorrectRangeForPosition(const VirtualRegister& reg,
                                          CodePosition pos,
                                          const LiveRange* range) {
  MOZ_ASSERT(range->covers(pos));
#ifdef DEBUG
  // Assert the result is consistent with rangeFor. The ranges can be different
  // but must be equivalent (both register or both non-register ranges).
  LiveRange* expected = reg.rangeFor(pos, /* preferRegister = */ true);
  MOZ_ASSERT(range->bundle()->allocation().isAnyRegister() ==
             expected->bundle()->allocation().isAnyRegister());
#endif
}

// Helper for ::createMoveGroupsFromLiveRangeTransitions
bool BacktrackingAllocator::createMoveGroupsForControlFlowEdges(
    const VirtualRegister& reg, const ControlFlowEdgeVector& edges) {
  // Iterate over both the virtual register ranges (sorted by start position)
  // and the control flow edges (sorted by predecessorExit). When we find the
  // predecessor range for the next edge, add a move from predecessor range to
  // successor range.

  VirtualRegister::RangeIterator iter(reg);
  LiveRange* nonRegisterRange = nullptr;

  for (const ControlFlowEdge& edge : edges) {
    CodePosition pos = edge.predecessorExit;
    LAllocation successorAllocation =
        edge.successorRange->bundle()->allocation();

    // We don't need to insert a move if nonRegisterRange covers the predecessor
    // block exit and has the same allocation as the successor block. This check
    // is not required for correctness but it reduces the number of generated
    // moves.
    if (nonRegisterRange && pos < nonRegisterRange->to() &&
        nonRegisterRange->bundle()->allocation() == successorAllocation) {
      MOZ_ASSERT(nonRegisterRange->covers(pos));
      continue;
    }

    // Search for a matching range. Prefer a register range.
    LiveRange* predecessorRange = nullptr;
    bool foundSameAllocation = false;
    while (true) {
      if (iter.done() || iter->from() > pos) {
        // No register range covers this edge.
        predecessorRange = nonRegisterRange;
        break;
      }
      if (iter->to() <= pos) {
        // Skip ranges that end before this edge (and later edges).
        iter++;
        continue;
      }
      MOZ_ASSERT(iter->covers(pos));
      if (iter->bundle()->allocation() == successorAllocation) {
        // There's a range covering the predecessor block exit that has the same
        // allocation, so we don't need to insert a move. This check is not
        // required for correctness but it reduces the number of generated
        // moves.
        foundSameAllocation = true;
        break;
      }
      if (iter->bundle()->allocation().isAnyRegister()) {
        predecessorRange = *iter;
        break;
      }
      if (!nonRegisterRange || iter->to() > nonRegisterRange->to()) {
        nonRegisterRange = *iter;
      }
      iter++;
    }

    if (foundSameAllocation) {
      continue;
    }

    MOZ_ASSERT(predecessorRange);
    AssertCorrectRangeForPosition(reg, pos, predecessorRange);

    if (!alloc().ensureBallast()) {
      return false;
    }
    JitSpew(JitSpew_RegAlloc, "    (moveAtEdge#2)");
    if (!moveAtEdge(edge.predecessor, edge.successor, predecessorRange,
                    edge.successorRange, reg.type())) {
      return false;
    }
  }

  return true;
}

bool BacktrackingAllocator::createMoveGroupsFromLiveRangeTransitions() {
  // Add moves to handle changing assignments for vregs over their lifetime.
  JitSpew(JitSpew_RegAlloc, "ResolveControlFlow: begin");

  JitSpew(JitSpew_RegAlloc,
          "  ResolveControlFlow: adding MoveGroups within blocks");

  // Look for places where a register's assignment changes in the middle of a
  // basic block.
  MOZ_ASSERT(!vregs[0u].hasRanges());
  for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
    VirtualRegister& reg = vregs[i];

    if (mir->shouldCancel(
            "Backtracking Resolve Control Flow (vreg outer loop)")) {
      return false;
    }

    // Remove ranges which will never be used.
    removeDeadRanges(reg);

    LiveRange* registerRange = nullptr;
    LiveRange* nonRegisterRange = nullptr;
    VirtualRegister::RangeIterator iter(reg);

    // Keep track of the register and non-register ranges with the highest end
    // position before advancing the iterator. These are predecessor ranges for
    // later ranges.
    auto moveToNextRange = [&](LiveRange* range) {
      MOZ_ASSERT(*iter == range);
      if (range->bundle()->allocation().isAnyRegister()) {
        if (!registerRange || range->to() > registerRange->to()) {
          registerRange = range;
        }
      } else {
        if (!nonRegisterRange || range->to() > nonRegisterRange->to()) {
          nonRegisterRange = range;
        }
      }
      iter++;
    };

    // Iterate over all ranges.
    while (!iter.done()) {
      LiveRange* range = *iter;

      if (mir->shouldCancel(
              "Backtracking Resolve Control Flow (vreg inner loop)")) {
        return false;
      }

      // The range which defines the register does not have a predecessor
      // to add moves from.
      if (range->hasDefinition()) {
        moveToNextRange(range);
        continue;
      }

      // Ignore ranges that start at block boundaries. We will handle
      // these in the next phase.
      CodePosition start = range->from();
      LNode* ins = insData[start];
      if (start == entryOf(ins->block())) {
        moveToNextRange(range);
        continue;
      }

      // Determine the predecessor range to use for this range and other ranges
      // starting at the same position. Prefer a register range.
      LiveRange* predecessorRange = nullptr;
      if (registerRange && start.previous() < registerRange->to()) {
        predecessorRange = registerRange;
      } else {
        MOZ_ASSERT(nonRegisterRange);
        MOZ_ASSERT(start.previous() < nonRegisterRange->to());
        predecessorRange = nonRegisterRange;
      }
      AssertCorrectRangeForPosition(reg, start.previous(), predecessorRange);

      // Add moves from predecessorRange to all ranges that start here.
      do {
        range = *iter;
        MOZ_ASSERT(!range->hasDefinition());

        if (!alloc().ensureBallast()) {
          return false;
        }

#ifdef DEBUG
        // If we already saw a range which covers the start of this range, it
        // must have a different allocation.
        for (VirtualRegister::RangeIterator prevIter(reg); *prevIter != range;
             prevIter++) {
          MOZ_ASSERT_IF(prevIter->covers(start),
                        prevIter->bundle()->allocation() !=
                            range->bundle()->allocation());
        }
#endif

        if (start.subpos() == CodePosition::INPUT) {
          JitSpewIfEnabled(JitSpew_RegAlloc, "    moveInput (%s) <- (%s)",
                           range->toString().get(),
                           predecessorRange->toString().get());
          if (!moveInput(ins->toInstruction(), predecessorRange, range,
                         reg.type())) {
            return false;
          }
        } else {
          JitSpew(JitSpew_RegAlloc, "    (moveAfter)");
          if (!moveAfter(ins->toInstruction(), predecessorRange, range,
                         reg.type())) {
            return false;
          }
        }

        moveToNextRange(range);
      } while (!iter.done() && iter->from() == start);
    }
  }

  JitSpew(JitSpew_RegAlloc,
          "  ResolveControlFlow: adding MoveGroups for phi nodes");

  for (size_t i = 0; i < graph.numBlocks(); i++) {
    if (mir->shouldCancel("Backtracking Resolve Control Flow (block loop)")) {
      return false;
    }

    LBlock* successor = graph.getBlock(i);
    MBasicBlock* mSuccessor = successor->mir();
    if (mSuccessor->numPredecessors() < 1) {
      continue;
    }

    // Resolve phis to moves.
    for (size_t j = 0; j < successor->numPhis(); j++) {
      LPhi* phi = successor->getPhi(j);
      MOZ_ASSERT(phi->numDefs() == 1);
      LDefinition* def = phi->getDef(0);
      VirtualRegister& reg = vreg(def);
      LiveRange* to = reg.firstRange();
      MOZ_ASSERT(to->from() == entryOf(successor));

      for (size_t k = 0; k < mSuccessor->numPredecessors(); k++) {
        LBlock* predecessor = mSuccessor->getPredecessor(k)->lir();
        MOZ_ASSERT(predecessor->mir()->numSuccessors() == 1);

        LAllocation* input = phi->getOperand(k);
        LiveRange* from = vreg(input).rangeFor(exitOf(predecessor),
                                               /* preferRegister = */ true);
        MOZ_ASSERT(from);

        if (!alloc().ensureBallast()) {
          return false;
        }

        // Note: we have to use moveAtEdge both here and below (for edge
        // resolution) to avoid conflicting moves. See bug 1493900.
        JitSpew(JitSpew_RegAlloc, "    (moveAtEdge#1)");
        if (!moveAtEdge(predecessor, successor, from, to, def->type())) {
          return false;
        }
      }
    }
  }

  JitSpew(JitSpew_RegAlloc,
          "  ResolveControlFlow: adding MoveGroups to fix conflicted edges");

  // Add moves to resolve graph edges with different allocations at their
  // source and target.
  ControlFlowEdgeVector edges;
  for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
    VirtualRegister& reg = vregs[i];

    // First collect all control flow edges we need to resolve. This loop knows
    // the range on the successor side, but looking up the corresponding
    // predecessor range with rangeFor is quadratic so we handle that
    // differently.
    edges.clear();
    for (VirtualRegister::RangeIterator iter(reg); iter; iter++) {
      LiveRange* targetRange = *iter;

      size_t firstBlockId = insData[targetRange->from()]->block()->mir()->id();
      if (!targetRange->covers(entryOf(graph.getBlock(firstBlockId)))) {
        firstBlockId++;
      }
      for (size_t id = firstBlockId; id < graph.numBlocks(); id++) {
        LBlock* successor = graph.getBlock(id);
        if (!targetRange->covers(entryOf(successor))) {
          break;
        }

        VirtualRegBitSet& live = liveIn[id];
        if (!live.contains(i)) {
          continue;
        }

        for (size_t j = 0; j < successor->mir()->numPredecessors(); j++) {
          LBlock* predecessor = successor->mir()->getPredecessor(j)->lir();
          CodePosition predecessorExit = exitOf(predecessor);
          if (targetRange->covers(predecessorExit)) {
            continue;
          }
          if (!edges.emplaceBack(predecessor, successor, targetRange,
                                 predecessorExit)) {
            return false;
          }
        }
      }
    }

    if (edges.empty()) {
      continue;
    }

    // Sort edges by predecessor position. This doesn't need to be a stable sort
    // because createMoveGroupsForControlFlowEdges will use the same predecessor
    // range if there are multiple edges with the same predecessor position.
    auto compareEdges = [](const ControlFlowEdge& a, const ControlFlowEdge& b) {
      return a.predecessorExit < b.predecessorExit;
    };
    std::sort(edges.begin(), edges.end(), compareEdges);

    // Resolve edges and add move groups.
    if (!createMoveGroupsForControlFlowEdges(reg, edges)) {
      return false;
    }
  }

  JitSpew(JitSpew_RegAlloc, "ResolveControlFlow: end");
  return true;
}

// Helper for ::addLiveRegistersForRange
size_t BacktrackingAllocator::findFirstNonCallSafepoint(CodePosition pos,
                                                        size_t startFrom) {
  // Assert startFrom is valid.
  MOZ_ASSERT_IF(startFrom > 0,
                inputOf(nonCallSafepoints_[startFrom - 1]) < pos);

  size_t i = startFrom;
  for (; i < nonCallSafepoints_.length(); i++) {
    const LInstruction* ins = nonCallSafepoints_[i];
    if (pos <= inputOf(ins)) {
      break;
    }
  }
  return i;
}

// Helper for ::installAllocationsInLIR
void BacktrackingAllocator::addLiveRegistersForRange(
    VirtualRegister& reg, LiveRange* range, size_t* firstNonCallSafepoint) {
  // Fill in the live register sets for all non-call safepoints.
  LAllocation a = range->bundle()->allocation();
  if (!a.isAnyRegister()) {
    return;
  }

  // Don't add output registers to the safepoint.
  CodePosition start = range->from();
  if (range->hasDefinition()) {
#ifdef CHECK_OSIPOINT_REGISTERS
    // Add output and temp registers to the safepoint's clobberedRegs.
    // Note: the outputs aren't added to the safepoint's liveRegs here, but the
    // same register might still be added to liveRegs for one of the inputs, so
    // we have to add outputs to clobberedRegs here.
    if (reg.ins()->isInstruction() && !reg.ins()->isCall()) {
      if (LSafepoint* safepoint = reg.ins()->toInstruction()->safepoint()) {
        safepoint->addClobberedRegister(a.toAnyRegister());
      }
    }
#endif
    if (!reg.isTemp()) {
      start = start.next();
    }
  }

  *firstNonCallSafepoint =
      findFirstNonCallSafepoint(start, *firstNonCallSafepoint);

  for (size_t i = *firstNonCallSafepoint; i < nonCallSafepoints_.length();
       i++) {
    LInstruction* ins = nonCallSafepoints_[i];
    CodePosition pos = inputOf(ins);

    // Safepoints are sorted, so we can shortcut out of this loop
    // if we go out of range.
    if (range->to() <= pos) {
      break;
    }

    MOZ_ASSERT(range->covers(pos));

    LSafepoint* safepoint = ins->safepoint();
    safepoint->addLiveRegister(a.toAnyRegister());
  }
}

// Helper for ::installAllocationsInLIR
static inline size_t NumReusingDefs(LInstruction* ins) {
  size_t num = 0;
  for (size_t i = 0; i < ins->numDefs(); i++) {
    LDefinition* def = ins->getDef(i);
    if (def->policy() == LDefinition::MUST_REUSE_INPUT) {
      num++;
    }
  }
  return num;
}

bool BacktrackingAllocator::installAllocationsInLIR() {
  JitSpew(JitSpew_RegAlloc, "Installing Allocations");

  // The virtual registers and safepoints are both ordered by position. To avoid
  // quadratic behavior in findFirstNonCallSafepoint, we use
  // firstNonCallSafepoint as cursor to start the search at the safepoint
  // returned by the previous call.
  size_t firstNonCallSafepoint = 0;

  MOZ_ASSERT(!vregs[0u].hasRanges());
  for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
    VirtualRegister& reg = vregs[i];

    if (mir->shouldCancel("Backtracking Install Allocations (main loop)")) {
      return false;
    }

    firstNonCallSafepoint =
        findFirstNonCallSafepoint(inputOf(reg.ins()), firstNonCallSafepoint);

    // The ranges are sorted by start position, so we can use the same
    // findFirstNonCallSafepoint optimization here.
    size_t firstNonCallSafepointForRange = firstNonCallSafepoint;

    for (VirtualRegister::RangeIterator iter(reg); iter; iter++) {
      LiveRange* range = *iter;

      if (range->hasDefinition()) {
        reg.def()->setOutput(range->bundle()->allocation());
        if (reg.ins()->recoversInput()) {
          LSnapshot* snapshot = reg.ins()->toInstruction()->snapshot();
          for (size_t i = 0; i < snapshot->numEntries(); i++) {
            LAllocation* entry = snapshot->getEntry(i);
            if (entry->isUse() &&
                entry->toUse()->policy() == LUse::RECOVERED_INPUT) {
              *entry = *reg.def()->output();
            }
          }
        }
      }

      for (UsePositionIterator iter(range->usesBegin()); iter; iter++) {
        LAllocation* alloc = iter->use();
        *alloc = range->bundle()->allocation();

        // For any uses which feed into MUST_REUSE_INPUT definitions,
        // add copies if the use and def have different allocations.
        LNode* ins = insData[iter->pos];
        if (LDefinition* def = FindReusingDefOrTemp(ins, alloc)) {
          LiveRange* outputRange = vreg(def).firstRange();
          MOZ_ASSERT(outputRange->covers(outputOf(ins)));
          LAllocation res = outputRange->bundle()->allocation();
          LAllocation sourceAlloc = range->bundle()->allocation();

          if (res != *alloc) {
            if (!this->alloc().ensureBallast()) {
              return false;
            }
            if (NumReusingDefs(ins->toInstruction()) <= 1) {
              LMoveGroup* group = getInputMoveGroup(ins->toInstruction());
              if (!group->addAfter(sourceAlloc, res, reg.type())) {
                return false;
              }
            } else {
              LMoveGroup* group = getFixReuseMoveGroup(ins->toInstruction());
              if (!group->add(sourceAlloc, res, reg.type())) {
                return false;
              }
            }
            *alloc = res;
          }
        }
      }

      addLiveRegistersForRange(reg, range, &firstNonCallSafepointForRange);
    }
  }

  graph.setLocalSlotsSize(stackSlotAllocator.stackHeight());
  return true;
}

// Helper for ::populateSafepoints
size_t BacktrackingAllocator::findFirstSafepoint(CodePosition pos,
                                                 size_t startFrom) {
  // Assert startFrom is valid.
  MOZ_ASSERT_IF(startFrom > 0, inputOf(safepoints_[startFrom - 1]) < pos);

  size_t i = startFrom;
  for (; i < safepoints_.length(); i++) {
    LInstruction* ins = safepoints_[i];
    if (pos <= inputOf(ins)) {
      break;
    }
  }
  return i;
}

bool BacktrackingAllocator::populateSafepoints() {
  JitSpew(JitSpew_RegAlloc, "Populating Safepoints");

  // The virtual registers and safepoints are both ordered by position. To avoid
  // quadratic behavior in findFirstSafepoint, we use firstSafepoint as cursor
  // to start the search at the safepoint returned by the previous call.
  size_t firstSafepoint = 0;

  MOZ_ASSERT(!vregs[0u].def());
  for (uint32_t i = 1; i < graph.numVirtualRegisters(); i++) {
    VirtualRegister& reg = vregs[i];

    if (!reg.def() || !reg.def()->isSafepointGCType(reg.ins())) {
      continue;
    }

    firstSafepoint = findFirstSafepoint(inputOf(reg.ins()), firstSafepoint);
    if (firstSafepoint >= graph.numSafepoints()) {
      break;
    }

    // The ranges are sorted by start position, so we can use the same
    // findFirstSafepoint optimization here.
    size_t firstSafepointForRange = firstSafepoint;

    for (VirtualRegister::RangeIterator iter(reg); iter; iter++) {
      LiveRange* range = *iter;

      firstSafepointForRange =
          findFirstSafepoint(range->from(), firstSafepointForRange);

      for (size_t j = firstSafepointForRange; j < graph.numSafepoints(); j++) {
        LInstruction* ins = safepoints_[j];

        if (inputOf(ins) >= range->to()) {
          break;
        }

        MOZ_ASSERT(range->covers(inputOf(ins)));

        // Include temps but not instruction outputs. Also make sure
        // MUST_REUSE_INPUT is not used with gcthings or nunboxes, or
        // we would have to add the input reg to this safepoint.
        if (ins == reg.ins() && !reg.isTemp()) {
          DebugOnly<LDefinition*> def = reg.def();
          MOZ_ASSERT_IF(def->policy() == LDefinition::MUST_REUSE_INPUT,
                        def->type() == LDefinition::GENERAL ||
                            def->type() == LDefinition::INT32 ||
                            def->type() == LDefinition::FLOAT32 ||
                            def->type() == LDefinition::DOUBLE ||
                            def->type() == LDefinition::SIMD128);
          continue;
        }

        LSafepoint* safepoint = ins->safepoint();

        LAllocation a = range->bundle()->allocation();
        if (a.isGeneralReg() && ins->isCall()) {
          continue;
        }

        if (!safepoint->addGCAllocation(i, reg.def(), a)) {
          return false;
        }
      }
    }
  }

  return true;
}

bool BacktrackingAllocator::annotateMoveGroups() {
  // Annotate move groups in the LIR graph with any register that is not
  // allocated at that point and can be used as a scratch register. This is
  // only required for x86, as other platforms always have scratch registers
  // available for use.
#ifdef JS_CODEGEN_X86
  LiveRange* range = LiveRange::FallibleNew(alloc(), nullptr, CodePosition(),
                                            CodePosition().next());
  if (!range) {
    return false;
  }

  for (size_t i = 0; i < graph.numBlocks(); i++) {
    if (mir->shouldCancel("Backtracking Annotate Move Groups")) {
      return false;
    }

    LBlock* block = graph.getBlock(i);
    LInstruction* last = nullptr;
    for (LInstructionIterator iter = block->begin(); iter != block->end();
         ++iter) {
      if (iter->isMoveGroup()) {
        CodePosition from = last ? outputOf(last) : entryOf(block);
        range->setTo(from.next());
        range->setFrom(from);

        for (size_t i = 0; i < AnyRegister::Total; i++) {
          PhysicalRegister& reg = registers[i];
          if (reg.reg.isFloat() || !reg.allocatable) {
            continue;
          }

          // This register is unavailable for use if (a) it is in use
          // by some live range immediately before the move group,
          // or (b) it is an operand in one of the group's moves. The
          // latter case handles live ranges which end immediately
          // before the move group or start immediately after.
          // For (b) we need to consider move groups immediately
          // preceding or following this one.

          if (iter->toMoveGroup()->uses(reg.reg.gpr())) {
            continue;
          }
          bool found = false;
          LInstructionIterator niter(iter);
          for (niter++; niter != block->end(); niter++) {
            if (niter->isMoveGroup()) {
              if (niter->toMoveGroup()->uses(reg.reg.gpr())) {
                found = true;
                break;
              }
            } else {
              break;
            }
          }
          if (iter != block->begin()) {
            LInstructionIterator riter(iter);
            do {
              riter--;
              if (riter->isMoveGroup()) {
                if (riter->toMoveGroup()->uses(reg.reg.gpr())) {
                  found = true;
                  break;
                }
              } else {
                break;
              }
            } while (riter != block->begin());
          }

          if (found) {
            continue;
          }
          LiveRangePlus existingPlus;
          LiveRangePlus rangePlus(range);
          if (reg.allocations.contains(rangePlus, &existingPlus)) {
            continue;
          }

          iter->toMoveGroup()->setScratchRegister(reg.reg.gpr());
          break;
        }
      } else {
        last = *iter;
      }
    }
  }
#endif

  return true;
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Debug-printing support                                                    //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#ifdef JS_JITSPEW

UniqueChars LiveRange::toString() const {
  AutoEnterOOMUnsafeRegion oomUnsafe;

  UniqueChars buf = JS_smprintf("v%u %u-%u", hasVreg() ? vreg().vreg() : 0,
                                from().bits(), to().bits() - 1);

  if (buf && bundle() && !bundle()->allocation().isBogus()) {
    buf = JS_sprintf_append(std::move(buf), " %s",
                            bundle()->allocation().toString().get());
  }

  buf = JS_sprintf_append(std::move(buf), " {");

  if (buf && hasDefinition()) {
    buf = JS_sprintf_append(std::move(buf), " %u_def", from().bits());
    if (hasVreg()) {
      // If the definition has a fixed requirement, print it too.
      const LDefinition* def = vreg().def();
      LDefinition::Policy policy = def->policy();
      if (policy == LDefinition::FIXED || policy == LDefinition::STACK) {
        if (buf) {
          buf = JS_sprintf_append(std::move(buf), ":F:%s",
                                  def->output()->toString().get());
        }
      }
    }
  }

  for (UsePositionIterator iter = usesBegin(); buf && iter; iter++) {
    buf = JS_sprintf_append(std::move(buf), " %u_%s", iter->pos.bits(),
                            iter->use()->toString().get());
  }

  buf = JS_sprintf_append(std::move(buf), " }");

  if (!buf) {
    oomUnsafe.crash("LiveRange::toString()");
  }

  return buf;
}

UniqueChars LiveBundle::toString() const {
  AutoEnterOOMUnsafeRegion oomUnsafe;

  UniqueChars buf = JS_smprintf("LB%u(", id());

  if (buf) {
    if (spillParent()) {
      buf =
          JS_sprintf_append(std::move(buf), "parent=LB%u", spillParent()->id());
    } else {
      buf = JS_sprintf_append(std::move(buf), "parent=none");
    }
  }

  for (LiveBundle::RangeIterator iter = rangesBegin(); buf && iter; iter++) {
    if (buf) {
      buf = JS_sprintf_append(std::move(buf), "%s %s",
                              (iter == rangesBegin()) ? "" : " ##",
                              iter->toString().get());
    }
  }

  if (buf) {
    buf = JS_sprintf_append(std::move(buf), ")");
  }

  if (!buf) {
    oomUnsafe.crash("LiveBundle::toString()");
  }

  return buf;
}

void BacktrackingAllocator::dumpLiveRangesByVReg(const char* who) {
  MOZ_ASSERT(!vregs[0u].hasRanges());

  JitSpewCont(JitSpew_RegAlloc, "\n");
  JitSpew(JitSpew_RegAlloc, "Live ranges by virtual register (%s):", who);

  for (uint32_t i = 1; i < graph.numVirtualRegisters(); i++) {
    JitSpewHeader(JitSpew_RegAlloc);
    JitSpewCont(JitSpew_RegAlloc, "  ");
    VirtualRegister& reg = vregs[i];
    for (VirtualRegister::RangeIterator iter(reg); iter; iter++) {
      if (*iter != reg.firstRange()) {
        JitSpewCont(JitSpew_RegAlloc, " ## ");
      }
      JitSpewCont(JitSpew_RegAlloc, "%s", iter->toString().get());
    }
    JitSpewCont(JitSpew_RegAlloc, "\n");
  }
}

void BacktrackingAllocator::dumpLiveRangesByBundle(const char* who) {
  MOZ_ASSERT(!vregs[0u].hasRanges());

  JitSpewCont(JitSpew_RegAlloc, "\n");
  JitSpew(JitSpew_RegAlloc, "Live ranges by bundle (%s):", who);

  for (uint32_t i = 1; i < graph.numVirtualRegisters(); i++) {
    VirtualRegister& reg = vregs[i];
    for (VirtualRegister::RangeIterator baseIter(reg); baseIter; baseIter++) {
      LiveRange* range = *baseIter;
      LiveBundle* bundle = range->bundle();
      if (range == bundle->firstRange()) {
        JitSpew(JitSpew_RegAlloc, "  %s", bundle->toString().get());
      }
    }
  }
}

void BacktrackingAllocator::dumpAllocations() {
  JitSpew(JitSpew_RegAlloc, "Allocations:");

  dumpLiveRangesByBundle("in dumpAllocations()");

  JitSpewCont(JitSpew_RegAlloc, "\n");
  JitSpew(JitSpew_RegAlloc, "Allocations by physical register:");

  for (size_t i = 0; i < AnyRegister::Total; i++) {
    if (registers[i].allocatable && !registers[i].allocations.empty()) {
      JitSpewHeader(JitSpew_RegAlloc);
      JitSpewCont(JitSpew_RegAlloc, "  %s:", AnyRegister::FromCode(i).name());
      bool first = true;
      LiveRangePlusSet::Iter lrpIter(&registers[i].allocations);
      while (lrpIter.hasMore()) {
        LiveRange* range = lrpIter.next().liveRange();
        if (first) {
          first = false;
        } else {
          fprintf(stderr, " /");
        }
        fprintf(stderr, " %s", range->toString().get());
      }
      JitSpewCont(JitSpew_RegAlloc, "\n");
    }
  }

  JitSpewCont(JitSpew_RegAlloc, "\n");
}

#endif  // JS_JITSPEW

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Top level of the register allocation machinery                            //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

bool BacktrackingAllocator::go() {
  JitSpewCont(JitSpew_RegAlloc, "\n");
  JitSpew(JitSpew_RegAlloc, "Beginning register allocation");

  JitSpewCont(JitSpew_RegAlloc, "\n");
  if (JitSpewEnabled(JitSpew_RegAlloc)) {
    dumpInstructions("(Pre-allocation LIR)");
  }

  if (!init()) {
    return false;
  }

  if (!buildLivenessInfo()) {
    return false;
  }

#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_RegAlloc)) {
    dumpLiveRangesByVReg("after liveness analysis");
  }
#endif

  if (!allocationQueue.reserve(graph.numVirtualRegisters() * 3 / 2)) {
    return false;
  }

  JitSpewCont(JitSpew_RegAlloc, "\n");
  JitSpew(JitSpew_RegAlloc, "Beginning grouping and queueing registers");
  if (!mergeAndQueueRegisters()) {
    return false;
  }
  JitSpew(JitSpew_RegAlloc, "Completed grouping and queueing registers");

#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_RegAlloc)) {
    dumpLiveRangesByBundle("after grouping/queueing regs");
  }
#endif

  // There now follow two allocation loops, which are really the heart of the
  // allocator.  First, the "main" allocation loop.  This does almost all of
  // the allocation work, by repeatedly pulling bundles out of
  // ::allocationQueue and calling ::processBundle on it, until there are no
  // bundles left in the queue.  Note that ::processBundle can add new smaller
  // bundles to the queue if it needs to split or spill a bundle.
  //
  // For each bundle in turn pulled out of ::allocationQueue, ::processBundle:
  //
  // * calls ::computeRequirement to discover the overall constraint for the
  //   bundle.
  //
  // * tries to find a register for it, by calling either ::tryAllocateFixed or
  //   ::tryAllocateNonFixed.
  //
  // * if that fails, but ::tryAllocateFixed / ::tryAllocateNonFixed indicate
  //   that there is some other bundle with lower spill weight that can be
  //   evicted, then that bundle is evicted (hence, put back into
  //   ::allocationQueue), and we try again.
  //
  // * at most MAX_ATTEMPTS may be made.
  //
  // * If that still fails to find a register, then the bundle is handed off to
  //   ::chooseBundleSplit.  That will choose to either split the bundle,
  //   yielding multiple pieces which are put back into ::allocationQueue, or
  //   it will spill the bundle.  Note that the same mechanism applies to both;
  //   there's no clear boundary between splitting and spilling, because
  //   spilling can be interpreted as an extreme form of splitting.
  //
  // ::processBundle and its callees contains much gnarly and logic which isn't
  // easy to understand, particularly in the area of how eviction candidates
  // are chosen.  But it works well enough, and tinkering doesn't seem to
  // improve the resulting allocations.  More important is the splitting logic,
  // because that controls where spill/reload instructions are placed.
  //
  // Eventually ::allocationQueue becomes empty, and each LiveBundle has either
  // been allocated a register or is marked for spilling.  In the latter case
  // it will have been added to ::spilledBundles.

  JitSpewCont(JitSpew_RegAlloc, "\n");
  JitSpew(JitSpew_RegAlloc, "Beginning main allocation loop");
  JitSpewCont(JitSpew_RegAlloc, "\n");

  // Allocate, spill and split bundles until finished.
  while (!allocationQueue.empty()) {
    if (mir->shouldCancel("Backtracking Allocation")) {
      return false;
    }

    QueueItem item = allocationQueue.removeHighest();
    if (!processBundle(mir, item.bundle)) {
      return false;
    }
  }

  // And here's the second allocation loop (hidden inside
  // ::tryAllocatingRegistersForSpillBundles).  It makes one last attempt to
  // find a register for each spill bundle.  There's no attempt to free up
  // registers by eviction.  In at least 99% of cases this attempt fails, in
  // which case the bundle is handed off to ::spill.  The lucky remaining 1%
  // get a register.  Unfortunately this scheme interacts badly with the
  // splitting strategy, leading to excessive register-to-register copying in
  // some very simple cases.  See bug 1752520.
  //
  // A modest but probably worthwhile amount of allocation time can be saved by
  // making ::tryAllocatingRegistersForSpillBundles use specialised versions of
  // ::tryAllocateAnyRegister and its callees, that don't bother to create sets
  // of conflicting bundles.  Creating those sets is expensive and, here,
  // pointless, since we're not going to do any eviction based on them.  This
  // refinement is implemented in the un-landed patch at bug 1758274 comment
  // 15.

  JitSpewCont(JitSpew_RegAlloc, "\n");
  JitSpew(JitSpew_RegAlloc,
          "Main allocation loop complete; "
          "beginning spill-bundle allocation loop");
  JitSpewCont(JitSpew_RegAlloc, "\n");

  if (!tryAllocatingRegistersForSpillBundles()) {
    return false;
  }

  JitSpewCont(JitSpew_RegAlloc, "\n");
  JitSpew(JitSpew_RegAlloc, "Spill-bundle allocation loop complete");
  JitSpewCont(JitSpew_RegAlloc, "\n");

  // After this point, the VirtualRegister ranges are sorted and must stay
  // sorted.
  sortVirtualRegisterRanges();

  if (!pickStackSlots()) {
    return false;
  }

#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_RegAlloc)) {
    dumpAllocations();
  }
#endif

  if (!createMoveGroupsFromLiveRangeTransitions()) {
    return false;
  }

  if (!installAllocationsInLIR()) {
    return false;
  }

  if (!populateSafepoints()) {
    return false;
  }

  if (!annotateMoveGroups()) {
    return false;
  }

  JitSpewCont(JitSpew_RegAlloc, "\n");
  if (JitSpewEnabled(JitSpew_RegAlloc)) {
    dumpInstructions("(Post-allocation LIR)");
  }

  JitSpew(JitSpew_RegAlloc, "Finished register allocation");

  return true;
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
///////////////////////////////////////////////////////////////////////////////
