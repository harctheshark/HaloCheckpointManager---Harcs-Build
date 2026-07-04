#pragma once
#include "pch.h"
#include "ObservedScopedCallback.h"
#include "SharedRequestProvider.h"
#include "GlobalKill.h"
// A wrapper for eventpp events. Whenever a client makes or destroys a callback to an event, the wrapper fires an onCallbackListChanged event.



template <typename ret, typename... args>
class ObservedEvent;

template <typename ret, typename... args>
class ObservedEvent< eventpp::CallbackList<ret(args...)>> : TokenSharedRequestProvider
{
private:
	std::unique_ptr<ScopedCallback<ActionEvent>> initOnCallbackListChanged;

	std::shared_ptr<eventpp::CallbackList<ret(args...)>> mEvent = std::make_shared< eventpp::CallbackList<ret(args...)>>();
	std::shared_ptr<ActionEvent> callbackListChangedEvent = std::make_shared<ActionEvent>();


	virtual void updateService() override
	{
		// During shutdown, do NOT fire the onCallbackListChanged notification. It is invoked when a
		// subscriber is added/removed, which happens all through teardown as services are destroyed.
		// The handler on the other end is typically a `[this]` lambda owned by a hook object that may
		// already have been destroyed (this ObservedEvent can outlive it - subscribers hold it), so
		// firing it would run against a dangling `this` and crash. GlobalKill is a static flag, so this
		// check is safe regardless of any object lifetime.
		if (GlobalKill::isKillSet()) return;
		callbackListChangedEvent->operator()();
	}

public:
	explicit ObservedEvent() {}

	explicit ObservedEvent(std::function<void()> onCallbackListChanged)
	{
		initOnCallbackListChanged = std::make_unique<ScopedCallback<ActionEvent>>(callbackListChangedEvent, onCallbackListChanged);
	}

	std::unique_ptr<ScopedCallback<eventpp::CallbackList<ret(args...)>>> subscribe(std::function<ret(args...)> functor)
	{
		// create the callback before firing so observer can get accurate count of how many subscribers there are
		std::unique_ptr<ScopedCallback<eventpp::CallbackList<ret(args...)>>> out(
			new ObservedScopedCallback<eventpp::CallbackList<ret(args...)>>(mEvent, functor, this->makeScopedRequest()) // have to create object in place because std::make_unique doesn't have access to the private constructor
		);
		return std::move(out);
	}

	bool isEventSubscribed() const {
		return this->serviceIsRequested(); }

	std::shared_ptr<ActionEvent> getCallbackListChangedEvent() {
		return callbackListChangedEvent;
	}

	ret fireEvent(args... a) { return mEvent->operator()(a...); }

	// copy is banned
	ObservedEvent(const ObservedEvent& that) = delete;
	ObservedEvent& operator=(const ObservedEvent& that) = delete;

	// move is banned
	ObservedEvent(ObservedEvent&& that) = delete;
	ObservedEvent& operator=(ObservedEvent&& that) = delete;
};
