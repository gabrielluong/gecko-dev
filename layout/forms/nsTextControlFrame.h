/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsTextControlFrame_h___
#define nsTextControlFrame_h___

#include "mozilla/Attributes.h"
#include "mozilla/TextControlElement.h"
#include "nsContainerFrame.h"
#include "nsIAnonymousContentCreator.h"
#include "nsIContent.h"
#include "nsIStatefulFrame.h"

class nsISelectionController;
class EditorInitializerEntryTracker;
namespace mozilla {
class AutoTextControlHandlingState;
class ScrollContainerFrame;
class TextEditor;
class TextControlState;
enum class PseudoStyleType : uint8_t;
enum class SelectionDirection : uint8_t;
namespace dom {
class Element;
}  // namespace dom
}  // namespace mozilla

class nsTextControlFrame : public nsContainerFrame,
                           public nsIAnonymousContentCreator,
                           public nsIStatefulFrame {
  using Element = mozilla::dom::Element;

 public:
  NS_DECL_FRAMEARENA_HELPERS(nsTextControlFrame)

  NS_DECLARE_FRAME_PROPERTY_SMALL_VALUE(ContentScrollPos, nsPoint)

 protected:
  nsTextControlFrame(ComputedStyle*, nsPresContext*, nsIFrame::ClassID);

 public:
  explicit nsTextControlFrame(ComputedStyle* aStyle,
                              nsPresContext* aPresContext)
      : nsTextControlFrame(aStyle, aPresContext, kClassID) {}

  virtual ~nsTextControlFrame();

  /**
   * Destroy() causes preparing to destroy editor and that may cause running
   * selection listeners of spellchecker selection and document state listeners.
   * Not sure whether the former does something or not, but nobody should run
   * content script.  The latter is currently only FinderHighlighter to clean up
   * its fields at destruction.  Thus, the latter won't run content script too.
   * Therefore, this won't run unsafe script.
   */
  MOZ_CAN_RUN_SCRIPT_BOUNDARY void Destroy(DestroyContext&) override;

  mozilla::ScrollContainerFrame* GetScrollTargetFrame() const override;

  nscoord IntrinsicISize(const mozilla::IntrinsicSizeInput& aInput,
                         mozilla::IntrinsicISizeType aType) override;

  void Reflow(nsPresContext* aPresContext, ReflowOutput& aDesiredSize,
              const ReflowInput& aReflowInput,
              nsReflowStatus& aStatus) override;

  Maybe<nscoord> GetNaturalBaselineBOffset(
      mozilla::WritingMode aWM, BaselineSharingGroup aBaselineGroup,
      BaselineExportContext aExportContext) const override;

  BaselineSharingGroup GetDefaultBaselineSharingGroup() const override {
    return BaselineSharingGroup::Last;
  }

  static Maybe<nscoord> GetSingleLineTextControlBaseline(
      const nsIFrame* aFrame, nscoord aFirstBaseline, mozilla::WritingMode aWM,
      BaselineSharingGroup aBaselineGroup) {
    if (aFrame->StyleDisplay()->IsContainLayout()) {
      return Nothing{};
    }
    NS_ASSERTION(aFirstBaseline != NS_INTRINSIC_ISIZE_UNKNOWN,
                 "please call Reflow before asking for the baseline");
    return mozilla::Some(aBaselineGroup == BaselineSharingGroup::First
                             ? aFirstBaseline
                             : aFrame->BSize(aWM) - aFirstBaseline);
  }

#ifdef ACCESSIBILITY
  mozilla::a11y::AccType AccessibleType() override;
#endif

#ifdef DEBUG_FRAME_DUMP
  nsresult GetFrameName(nsAString& aResult) const override {
    aResult.AssignLiteral("nsTextControlFrame");
    return NS_OK;
  }
#endif

  // nsIAnonymousContentCreator
  nsresult CreateAnonymousContent(nsTArray<ContentInfo>& aElements) override;
  void AppendAnonymousContentTo(nsTArray<nsIContent*>& aElements,
                                uint32_t aFilter) override;

  void SetInitialChildList(ChildListID, nsFrameList&&) override;

  void BuildDisplayList(nsDisplayListBuilder* aBuilder,
                        const nsDisplayListSet& aLists) override;

  MOZ_CAN_RUN_SCRIPT_BOUNDARY already_AddRefed<mozilla::TextEditor>
  GetTextEditor();

  MOZ_CAN_RUN_SCRIPT NS_IMETHOD SetSelectionRange(uint32_t aSelectionStart,
                                                  uint32_t aSelectionEnd,
                                                  mozilla::SelectionDirection);
  NS_IMETHOD GetOwnedSelectionController(nsISelectionController** aSelCon);
  nsFrameSelection* GetOwnedFrameSelection() {
    return ControlElement()->GetIndependentFrameSelection();
  }
  nsISelectionController* GetSelectionController() {
    return ControlElement()->GetSelectionController();
  }

  void PlaceholderChanged(const nsAttrValue* aOld, const nsAttrValue* aNew);

  /**
   * Ensure mEditor is initialized with the proper flags and the default value.
   * @throws NS_ERROR_NOT_INITIALIZED if mEditor has not been created
   * @throws various and sundry other things
   */
  MOZ_CAN_RUN_SCRIPT_BOUNDARY nsresult EnsureEditorInitialized();

  //==== END NSITEXTCONTROLFRAME

  //==== NSISTATEFULFRAME

  mozilla::UniquePtr<mozilla::PresState> SaveState() override;
  NS_IMETHOD RestoreState(mozilla::PresState* aState) override;

  //=== END NSISTATEFULFRAME

  //==== OVERLOAD of nsIFrame

  /** handler for attribute changes to mContent */
  MOZ_CAN_RUN_SCRIPT_BOUNDARY nsresult AttributeChanged(
      int32_t aNameSpaceID, nsAtom* aAttribute, int32_t aModType) override;
  void ElementStateChanged(mozilla::dom::ElementState aStates) override;

  nsresult PeekOffset(mozilla::PeekOffsetStruct* aPos) override;

  NS_DECL_QUERYFRAME

  // Whether we should scroll only the current selection into view in the inner
  // scroller, or also ancestors as needed.
  enum class ScrollAncestors { No, Yes };
  void ScrollSelectionIntoViewAsync(ScrollAncestors = ScrollAncestors::No);

 protected:
  MOZ_CAN_RUN_SCRIPT_BOUNDARY void OnFocus();
  MOZ_CAN_RUN_SCRIPT_BOUNDARY void HandleReadonlyOrDisabledChange();

  /**
   * Launch the reflow on the child frames - see nsTextControlFrame::Reflow()
   */
  void ReflowTextControlChild(nsIFrame* aKid, nsPresContext* aPresContext,
                              const ReflowInput& aReflowInput,
                              nsReflowStatus& aStatus,
                              ReflowOutput& aParentDesiredSize,
                              const mozilla::LogicalSize& aParentContentBoxSize,
                              nscoord& aButtonBoxISize);

 public:
  static Maybe<nscoord> ComputeBaseline(const nsIFrame*, const ReflowInput&,
                                        bool aForSingleLineControl);

  Element* GetRootNode() const { return mRootNode; }

  Element* GetPreviewNode() const { return mPreviewDiv; }

  Element* GetPlaceholderNode() const { return mPlaceholderDiv; }

  Element* GetButton() const { return mButton; }

  bool IsButtonBox(const nsIFrame* aFrame) const {
    return aFrame->GetContent() == GetButton();
  }

  // called by the focus listener
  nsresult MaybeBeginSecureKeyboardInput();
  void MaybeEndSecureKeyboardInput();

  mozilla::TextControlElement* ControlElement() const {
    MOZ_ASSERT(mozilla::TextControlElement::FromNode(GetContent()));
    return static_cast<mozilla::TextControlElement*>(GetContent());
  }

#define DEFINE_TEXTCTRL_CONST_FORWARDER(type, name) \
  type name() const { return ControlElement()->name(); }

  DEFINE_TEXTCTRL_CONST_FORWARDER(bool, IsSingleLineTextControl)
  DEFINE_TEXTCTRL_CONST_FORWARDER(bool, IsTextArea)
  DEFINE_TEXTCTRL_CONST_FORWARDER(bool, IsPasswordTextControl)
  DEFINE_TEXTCTRL_CONST_FORWARDER(Maybe<int32_t>, GetCols)
  DEFINE_TEXTCTRL_CONST_FORWARDER(int32_t, GetColsOrDefault)
  DEFINE_TEXTCTRL_CONST_FORWARDER(int32_t, GetRows)

#undef DEFINE_TEXTCTRL_CONST_FORWARDER

  MOZ_CAN_RUN_SCRIPT nsresult SelectAll();

 protected:
  class EditorInitializer;
  friend class EditorInitializer;
  friend class mozilla::AutoTextControlHandlingState;  // needs access to
                                                       // CacheValue
  friend class mozilla::TextControlState;  // needs access to UpdateValueDisplay

  // Temp reference to scriptrunner
  NS_DECLARE_FRAME_PROPERTY_WITH_DTOR(TextControlInitializer, EditorInitializer,
                                      nsTextControlFrame::RevokeInitializer)

  static void RevokeInitializer(EditorInitializer* aInitializer) {
    aInitializer->Revoke();
  };

  class EditorInitializer : public mozilla::Runnable {
   public:
    explicit EditorInitializer(nsTextControlFrame* aFrame)
        : mozilla::Runnable("nsTextControlFrame::EditorInitializer"),
          mFrame(aFrame) {}

    NS_IMETHOD Run() override;

    // avoids use of AutoWeakFrame
    void Revoke() { mFrame = nullptr; }

   private:
    nsTextControlFrame* mFrame;
  };

  nsresult OffsetToDOMPoint(uint32_t aOffset, nsINode** aResult,
                            uint32_t* aPosition);

  /**
   * Update the textnode under our anonymous div to show the new
   * value. This should only be called when we have no editor yet.
   * @throws NS_ERROR_UNEXPECTED if the div has no text content
   */
  nsresult UpdateValueDisplay(bool aNotify, bool aBeforeEditorInit = false,
                              const nsAString* aValue = nullptr);

  /**
   * Find out whether an attribute exists on the content or not.
   * @param aAtt the attribute to determine the existence of
   * @returns false if it does not exist
   */
  bool AttributeExists(nsAtom* aAtt) const {
    return mContent && mContent->AsElement()->HasAttr(aAtt);
  }

  /**
   * We call this when we are being destroyed or removed from the PFM.
   * @param aPresContext the current pres context
   */
  void PreDestroy();

  // Compute our intrinsic size.  This does not include any borders, paddings,
  // etc.  Just the size of our actual area for the text (and the scrollbars,
  // for <textarea>).
  mozilla::LogicalSize CalcIntrinsicSize(gfxContext* aRenderingContext,
                                         mozilla::WritingMode aWM) const;

 private:
  // helper methods
  MOZ_CAN_RUN_SCRIPT nsresult SetSelectionInternal(nsINode* aStartNode,
                                                   uint32_t aStartOffset,
                                                   nsINode* aEndNode,
                                                   uint32_t aEndOffset,
                                                   mozilla::SelectionDirection);
  MOZ_CAN_RUN_SCRIPT nsresult SetSelectionEndPoints(
      uint32_t aSelStart, uint32_t aSelEnd, mozilla::SelectionDirection);

  void FinishedInitializer() { RemoveProperty(TextControlInitializer()); }

  const nsAString& CachedValue() const { return mCachedValue; }

  void ClearCachedValue() { mCachedValue.SetIsVoid(true); }

  void CacheValue(const nsAString& aValue) { mCachedValue.Assign(aValue); }

  [[nodiscard]] bool CacheValue(const nsAString& aValue,
                                const mozilla::fallible_t& aFallible) {
    if (!mCachedValue.Assign(aValue, aFallible)) {
      ClearCachedValue();
      return false;
    }
    return true;
  }

 protected:
  class nsAnonDivObserver;

  nsresult CreateRootNode();
  void CreatePlaceholderIfNeeded();
  void UpdatePlaceholderText(nsString&, bool aNotify);
  void CreatePreviewIfNeeded();
  already_AddRefed<Element> MakeAnonElement(
      mozilla::PseudoStyleType, Element* aParent = nullptr,
      nsAtom* aTag = nsGkAtoms::div) const;
  already_AddRefed<Element> MakeAnonDivWithTextNode(
      mozilla::PseudoStyleType) const;

  bool ShouldInitializeEagerly() const;
  void InitializeEagerlyIfNeeded();

  RefPtr<Element> mRootNode;
  RefPtr<Element> mPlaceholderDiv;
  RefPtr<Element> mPreviewDiv;
  // If we have type=password, number, or search, then mButton is our
  // reveal-password, spinner, or search button box. Otherwise, it's nullptr.
  RefPtr<Element> mButton;
  RefPtr<nsAnonDivObserver> mMutationObserver;
  // Cache of the |.value| of <input> or <textarea> element without hard-wrap.
  // If its IsVoid() returns true, it doesn't cache |.value|.
  // Otherwise, it's cached when setting specific value or getting value from
  // TextEditor.  Additionally, when contents in the anonymous <div> element
  // is modified, this is cleared.
  //
  // FIXME(bug 1402545): Consider using an nsAutoString here.
  nsString mCachedValue{VoidString()};

  // Our first baseline, or NS_INTRINSIC_ISIZE_UNKNOWN if we have a pending
  // Reflow (or if we're contain:layout, which means we have no baseline).
  nscoord mFirstBaseline = NS_INTRINSIC_ISIZE_UNKNOWN;

  // these packed bools could instead use the high order bits on mState, saving
  // 4 bytes
  bool mEditorHasBeenInitialized = false;
  bool mIsProcessing = false;

#ifdef DEBUG
  bool mInEditorInitialization = false;
  friend class EditorInitializerEntryTracker;
#endif
};

#endif
