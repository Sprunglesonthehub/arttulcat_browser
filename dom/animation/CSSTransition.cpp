/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CSSTransition.h"

#include "mozilla/AnimationEventDispatcher.h"
#include "mozilla/dom/CSSTransitionBinding.h"
#include "mozilla/dom/KeyframeEffectBinding.h"
#include "mozilla/dom/KeyframeEffect.h"
#include "mozilla/TimeStamp.h"
#include "nsPresContext.h"

namespace mozilla::dom {

JSObject* CSSTransition::WrapObject(JSContext* aCx,
                                    JS::Handle<JSObject*> aGivenProto) {
  return dom::CSSTransition_Binding::Wrap(aCx, this, aGivenProto);
}

void CSSTransition::GetTransitionProperty(nsString& aRetVal) const {
  MOZ_ASSERT(mTransitionProperty.IsValid(),
             "Transition Property should be initialized");
  mTransitionProperty.ToString(aRetVal);
}

AnimationPlayState CSSTransition::PlayStateFromJS() const {
  FlushUnanimatedStyle();
  return Animation::PlayStateFromJS();
}

bool CSSTransition::PendingFromJS() const {
  // Transitions don't become pending again after they start running but, if
  // while the transition is still pending, style is updated in such a way
  // that the transition will be canceled, we need to report false here.
  // Hence we need to flush, but only when we're pending.
  if (Pending()) {
    FlushUnanimatedStyle();
  }
  return Animation::PendingFromJS();
}

void CSSTransition::PlayFromJS(ErrorResult& aRv) {
  FlushUnanimatedStyle();
  Animation::PlayFromJS(aRv);
}

void CSSTransition::UpdateTiming(SeekFlag aSeekFlag,
                                 SyncNotifyFlag aSyncNotifyFlag) {
  if (mNeedsNewAnimationIndexWhenRun &&
      PlayState() != AnimationPlayState::Idle) {
    mAnimationIndex = sNextAnimationIndex++;
    mNeedsNewAnimationIndexWhenRun = false;
  }

  Animation::UpdateTiming(aSeekFlag, aSyncNotifyFlag);
}

void CSSTransition::QueueEvents(const StickyTimeDuration& aActiveTime) {
  if (!mOwningElement.ShouldFireEvents()) {
    return;
  }

  nsPresContext* presContext = mOwningElement.GetPresContext();
  if (!presContext) {
    return;
  }

  static constexpr StickyTimeDuration zeroDuration = StickyTimeDuration();

  TransitionPhase currentPhase;
  StickyTimeDuration intervalStartTime;
  StickyTimeDuration intervalEndTime;

  if (!mEffect) {
    currentPhase = GetAnimationPhaseWithoutEffect<TransitionPhase>(*this);
  } else {
    ComputedTiming computedTiming = mEffect->GetComputedTiming();

    currentPhase = static_cast<TransitionPhase>(computedTiming.mPhase);
    intervalStartTime = IntervalStartTime(computedTiming.mActiveDuration);
    intervalEndTime = IntervalEndTime(computedTiming.mActiveDuration);
  }

  if (mPendingState != PendingState::NotPending &&
      (mPreviousTransitionPhase == TransitionPhase::Idle ||
       mPreviousTransitionPhase == TransitionPhase::Pending)) {
    currentPhase = TransitionPhase::Pending;
  }

  if (currentPhase == mPreviousTransitionPhase) {
    return;
  }

  // TimeStamps to use for ordering the events when they are dispatched. We
  // use a TimeStamp so we can compare events produced by different elements,
  // perhaps even with different timelines.
  // The zero timestamp is for transitionrun events where we ignore the delay
  // for the purpose of ordering events.
  TimeStamp zeroTimeStamp = AnimationTimeToTimeStamp(zeroDuration);
  TimeStamp startTimeStamp = ElapsedTimeToTimeStamp(intervalStartTime);
  TimeStamp endTimeStamp = ElapsedTimeToTimeStamp(intervalEndTime);

  AutoTArray<AnimationEventInfo, 3> events;

  auto appendTransitionEvent = [&](EventMessage aMessage,
                                   const StickyTimeDuration& aElapsedTime,
                                   const TimeStamp& aScheduledEventTimeStamp) {
    double elapsedTime = aElapsedTime.ToSeconds();
    if (aMessage == eTransitionCancel) {
      // 0 is an inappropriate value for this callsite. What we need to do is
      // use a single random value for all increasing times reportable.
      // That is to say, whenever elapsedTime goes negative (because an
      // animation restarts, something rewinds the animation, or otherwise)
      // a new random value for the mix-in must be generated.
      elapsedTime = nsRFPService::ReduceTimePrecisionAsSecsRFPOnly(
          elapsedTime, 0, mRTPCallerType);
    }
    events.AppendElement(AnimationEventInfo(
        TransitionProperty(), mOwningElement.Target(), aMessage, elapsedTime,
        mAnimationIndex, aScheduledEventTimeStamp, this));
  };

  // Handle cancel events first
  if ((mPreviousTransitionPhase != TransitionPhase::Idle &&
       mPreviousTransitionPhase != TransitionPhase::After) &&
      currentPhase == TransitionPhase::Idle) {
    appendTransitionEvent(eTransitionCancel, aActiveTime,
                          GetTimelineCurrentTimeAsTimeStamp());
  }

  // All other events
  switch (mPreviousTransitionPhase) {
    case TransitionPhase::Idle:
      if (currentPhase == TransitionPhase::Pending ||
          currentPhase == TransitionPhase::Before) {
        // When we are replacing a transition and flushing the style in the
        // meantime, after a timeout, we may tick this transition without a
        // proper |mPendingReadyTime| because the refresh driver is not in
        // refresh, i.e. mInRefresh is false. So in the current tick we queue
        // this event but the transition would be triggered in the next tick.
        //
        // In general, we use Animation::EnsurePaintIsScheduled() to assign a
        // valid time to |mPendingReadyTime| of this transition, and then we
        // could trigger this transition if this value is set. When triggering,
        // we set a proper |mStartTime|, which could be used to calculate the
        // animation time, i.e. |zeroTimeStamp|.
        //
        // However, due to this race condition (i.e. the transition hasn't been
        // triggered yet but we are enqueuing this event), it's posssible to
        // have a null |zeroTimeStamp|, which breaks the sorting of transition
        // events. So we use the current time as a fallback way to make sure we
        // have a reasonable schedule time for sorting.
        appendTransitionEvent(eTransitionRun, intervalStartTime,
                              zeroTimeStamp.IsNull()
                                  ? GetTimelineCurrentTimeAsTimeStamp()
                                  : zeroTimeStamp);
      } else if (currentPhase == TransitionPhase::Active) {
        appendTransitionEvent(eTransitionRun, intervalStartTime, zeroTimeStamp);
        appendTransitionEvent(eTransitionStart, intervalStartTime,
                              startTimeStamp);
      } else if (currentPhase == TransitionPhase::After) {
        appendTransitionEvent(eTransitionRun, intervalStartTime, zeroTimeStamp);
        appendTransitionEvent(eTransitionStart, intervalStartTime,
                              startTimeStamp);
        appendTransitionEvent(eTransitionEnd, intervalEndTime, endTimeStamp);
      }
      break;

    case TransitionPhase::Pending:
    case TransitionPhase::Before:
      if (currentPhase == TransitionPhase::Active) {
        appendTransitionEvent(eTransitionStart, intervalStartTime,
                              startTimeStamp);
      } else if (currentPhase == TransitionPhase::After) {
        appendTransitionEvent(eTransitionStart, intervalStartTime,
                              startTimeStamp);
        appendTransitionEvent(eTransitionEnd, intervalEndTime, endTimeStamp);
      }
      break;

    case TransitionPhase::Active:
      if (currentPhase == TransitionPhase::After) {
        appendTransitionEvent(eTransitionEnd, intervalEndTime, endTimeStamp);
      } else if (currentPhase == TransitionPhase::Before) {
        appendTransitionEvent(eTransitionEnd, intervalStartTime,
                              startTimeStamp);
      }
      break;

    case TransitionPhase::After:
      if (currentPhase == TransitionPhase::Active) {
        appendTransitionEvent(eTransitionStart, intervalEndTime,
                              startTimeStamp);
      } else if (currentPhase == TransitionPhase::Before) {
        appendTransitionEvent(eTransitionStart, intervalEndTime,
                              startTimeStamp);
        appendTransitionEvent(eTransitionEnd, intervalStartTime, endTimeStamp);
      }
      break;
  }
  mPreviousTransitionPhase = currentPhase;

  if (!events.IsEmpty()) {
    presContext->AnimationEventDispatcher()->QueueEvents(std::move(events));
  }
}

void CSSTransition::Tick(TickState& aState) {
  Animation::Tick(aState);
  QueueEvents();
}

const AnimatedPropertyID& CSSTransition::TransitionProperty() const {
  MOZ_ASSERT(mTransitionProperty.IsValid(),
             "Transition property should be initialized");
  return mTransitionProperty;
}

AnimationValue CSSTransition::ToValue() const {
  MOZ_ASSERT(!mTransitionToValue.IsNull(),
             "Transition ToValue should be initialized");
  return mTransitionToValue;
}

int32_t CSSTransition::CompareCompositeOrder(
    const Maybe<EventContext>& aContext, const CSSTransition& aOther,
    const Maybe<EventContext>& aOtherContext,
    nsContentUtils::NodeIndexCache& aCache) const {
  MOZ_ASSERT((IsTiedToMarkup() || aContext) &&
                 (aOther.IsTiedToMarkup() || aOtherContext),
             "Should only be called for CSS transitions that are sorted "
             "as CSS transitions (i.e. tied to CSS markup) or with overridden "
             "target and animation index");

  // 0. Object-equality case
  if (&aOther == this) {
    return 0;
  }

  // 1. Sort by document order
  const OwningElementRef& owningElement1 =
      IsTiedToMarkup() ? mOwningElement : OwningElementRef(aContext->mTarget);
  const OwningElementRef& owningElement2 =
      aOther.IsTiedToMarkup() ? aOther.mOwningElement
                              : OwningElementRef(aOtherContext->mTarget);
  if (!owningElement1.Equals(owningElement2)) {
    return owningElement1.Compare(owningElement2, aCache);
  }

  // 2. (Same element and pseudo): Sort by transition generation
  const uint64_t index1 = IsTiedToMarkup() ? mAnimationIndex : aContext->mIndex;
  const uint64_t index2 =
      aOther.IsTiedToMarkup() ? aOther.mAnimationIndex : aOtherContext->mIndex;
  if (index1 != index2) {
    return index1 < index2 ? -1 : 1;
  }

  // 3. (Same transition generation): Sort by transition property
  if (mTransitionProperty == aOther.mTransitionProperty) {
    return 0;
  }
  nsAutoString name, otherName;
  GetTransitionProperty(name);
  aOther.GetTransitionProperty(otherName);
  return name < otherName ? -1 : 1;
}

/* static */
Nullable<TimeDuration> CSSTransition::GetCurrentTimeAt(
    const AnimationTimeline& aTimeline, const TimeStamp& aBaseTime,
    const TimeDuration& aStartTime, double aPlaybackRate) {
  Nullable<TimeDuration> result;

  Nullable<TimeDuration> timelineTime = aTimeline.ToTimelineTime(aBaseTime);
  if (!timelineTime.IsNull()) {
    result.SetValue(
        (timelineTime.Value() - aStartTime).MultDouble(aPlaybackRate));
  }

  return result;
}

double CSSTransition::CurrentValuePortion() const {
  if (!GetEffect()) {
    return 0.0;
  }

  // Transitions use a fill mode of 'backwards' so GetComputedTiming will
  // never return a null time progress due to being *before* the animation
  // interval. However, it might be possible that we're behind on flushing
  // causing us to get called *after* the animation interval. So, just in
  // case, we override the fill mode to 'both' to ensure the progress
  // is never null.
  TimingParams timingToUse = GetEffect()->SpecifiedTiming();
  timingToUse.SetFill(dom::FillMode::Both);
  ComputedTiming computedTiming = GetEffect()->GetComputedTiming(&timingToUse);

  if (computedTiming.mProgress.IsNull()) {
    return 0.0;
  }

  // 'transition-timing-function' corresponds to the effect timing while
  // the transition keyframes have a linear timing function so we can ignore
  // them for the purposes of calculating the value portion.
  return computedTiming.mProgress.Value();
}

bool CSSTransition::UpdateStartValueFromReplacedTransition() {
  MOZ_ASSERT(mEffect && mEffect->AsKeyframeEffect() &&
                 mEffect->AsKeyframeEffect()->HasAnimationOfPropertySet(
                     nsCSSPropertyIDSet::CompositorAnimatables()),
             "Should be called for compositor-runnable transitions");

  if (!mReplacedTransition) {
    return false;
  }

  // We don't set |mReplacedTransition| if the timeline of this transition is
  // different from the document timeline. The timeline of Animation may be
  // null via script, so if it's null, it must be different from the document
  // timeline (because document timeline is readonly so we cannot change it by
  // script). Therefore, we check this assertion if mReplacedTransition is
  // valid.
  MOZ_ASSERT(mTimeline,
             "Should have a timeline if we are replacing transition start "
             "values");

  if (Maybe<double> valuePosition =
          ComputeTransformedProgress(*mTimeline, *mReplacedTransition)) {
    // FIXME: Bug 1634945. We may have to use the last value on the compositor
    // to replace the start value.
    const AnimationValue& replacedFrom = mReplacedTransition->mFromValue;
    const AnimationValue& replacedTo = mReplacedTransition->mToValue;
    AnimationValue startValue;
    startValue.mServo =
        Servo_AnimationValues_Interpolate(replacedFrom.mServo,
                                          replacedTo.mServo, *valuePosition)
            .Consume();

    mEffect->AsKeyframeEffect()->ReplaceTransitionStartValue(
        std::move(startValue));
  }

  mReplacedTransition.reset();

  return true;
}

/* static*/
Maybe<double> CSSTransition::ComputeTransformedProgress(
    const AnimationTimeline& aTimeline,
    const ReplacedTransitionProperties& aProperties) {
  ComputedTiming computedTiming = AnimationEffect::GetComputedTimingAt(
      CSSTransition::GetCurrentTimeAt(aTimeline, TimeStamp::Now(),
                                      aProperties.mStartTime,
                                      aProperties.mPlaybackRate),
      aProperties.mTiming, aProperties.mPlaybackRate,
      Animation::ProgressTimelinePosition::NotBoundary);
  if (computedTiming.mProgress.IsNull()) {
    return Nothing();
  }

  return Some(StyleComputedTimingFunction::GetPortion(
      aProperties.mTimingFunction, computedTiming.mProgress.Value(),
      computedTiming.mBeforeFlag));
}

void CSSTransition::SetEffectFromStyle(KeyframeEffect* aEffect) {
  MOZ_ASSERT(aEffect->IsValidTransition());

  Animation::SetEffectNoUpdate(aEffect);
  mTransitionProperty = aEffect->Properties()[0].mProperty;
  mTransitionToValue = aEffect->Properties()[0].mSegments[0].mToValue;
}

}  // namespace mozilla::dom
