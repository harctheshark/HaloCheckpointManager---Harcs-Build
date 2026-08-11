#pragma once
template <typename ret, typename... args>
class ScopedCallback;

template <typename ret, typename... args>
class ScopedCallback<eventpp::CallbackList<ret(args...)>>
{
private:
	std::weak_ptr <eventpp::CallbackList<ret(args...)>> m_pEventWeak;
	eventpp::CallbackList<ret(args...)>::Handle mHandle;
public:

	ScopedCallback() = delete;

	// ⚠ THE NULL CHECK IS NOT DEFENSIVE PROGRAMMING, IT IS A CRASH FIX.
	//
	// This used to be `mHandle(pEvent->append(functor))` with nothing in front of it. Almost every caller writes
	// `dicon.Resolve<Something>().lock()` inline in a member initialiser list, and .lock() yields NULL whenever
	// that service is not registered for the current game or is not alive yet - so the argument is null in a
	// case that is entirely reachable, and append() then dereferences it. Measured: an access violation WRITING
	// to 0x10000006F inside CallbackListBase::getNextCounter, taking the whole game down during cheat
	// construction with nothing in the log to say which cheat did it, because the throw site is a template
	// shared by every event in HCM.
	//
	// Throwing HCMInitException instead is what makes that diagnosable AND survivable: it is the one exception
	// OptionalCheatConstructor::createCheats catches, so the offending cheat fails to construct, says so by
	// name in the log, and everything else still loads.
	explicit ScopedCallback(std::shared_ptr <eventpp::CallbackList<ret(args...)>> pEvent, std::function<ret(args...)> functor)
		: m_pEventWeak(pEvent), mHandle(pEvent ? pEvent->append(functor) : throw HCMInitException("Tried to subscribe to an event that does not exist (the service providing it was not available for this game)")) {}

	virtual ~ScopedCallback()
	{
		auto m_pEvent = m_pEventWeak.lock();
		if (m_pEvent && mHandle)
			m_pEvent->remove(mHandle);
	}

	void removeCallback() // used in class destructors with destruction guards to prevent any new calls of functor
	{
		auto m_pEvent = m_pEventWeak.lock();
		if (m_pEvent && mHandle)
			m_pEvent->remove(mHandle);

		// to prevent double-removal
		m_pEventWeak.reset();
	}

	// copy is banned
	ScopedCallback(const ScopedCallback& that) = delete;
	ScopedCallback& operator=(const ScopedCallback& that) = delete;

	// move is banned
	ScopedCallback(ScopedCallback&& that) = delete;
	ScopedCallback& operator=(ScopedCallback&& that) = delete;


};
