#pragma once
#include "pch.h"
#include "SharedRequestToken.h"
#include <atomic>

// uses shared_ptr custom deleter to fire updateService when the shared_ptr to the token expires. and ofc fires updateService when a new token is requested.
template<class SharedRequestDerivedType> requires std::derived_from<SharedRequestDerivedType, SharedRequestToken>
class SharedRequestProvider
{
private:
	std::weak_ptr< SharedRequestDerivedType> mRequest;
	std::function<SharedRequestDerivedType* ()> mRequestFactory;
	// Shared with each token's deleter. Set to false in our destructor so a token whose deleter
	// fires AFTER we're gone can detect that and skip updateService() instead of calling it on
	// freed memory. This replaces an unbounded spin-wait in the destructor that deadlocked shutdown
	// whenever a subscriber (token holder) outlived its event in the teardown order.
	std::shared_ptr<std::atomic_bool> mAlive = std::make_shared<std::atomic_bool>(true);

public:

	std::shared_ptr<SharedRequestDerivedType> makeScopedRequest()
	{

		if (auto lock = mRequest.lock())
		{
			return lock;
		}
		else
		{
			// create new request, fire update event.
			// The deleter captures the shared "alive" flag so that once the last token reference
			// expires it only calls updateService() if this provider still exists (see ~).
			auto alive = mAlive;
			auto shared = std::shared_ptr<SharedRequestDerivedType>(mRequestFactory(),
				[this, alive](SharedRequestDerivedType* ptr) { delete ptr; if (alive->load()) this->updateService(); }); // once last reference expires, updateService is called (unless we've been destroyed)
			mRequest = shared;
			this->updateService();
			return shared;
		}
	}

	bool serviceIsRequested() const
	{
		return !mRequest.expired();
	}

	virtual void updateService() {} // function fired when new SharedRequestToken created or last one expires. override this to act on the new serviceIsRequested().


	virtual ~SharedRequestProvider()
	{
		// Tell any outstanding token deleters that we're gone, so they skip updateService() instead
		// of calling it on freed memory. Previously this spin-waited for every token to expire first,
		// but during shutdown a subscriber (token holder) can be destroyed AFTER its event in the
		// teardown order - so the wait never ended, deadlocking teardown and stopping the DLL from
		// unloading (which is what prevented HCM from re-attaching after being closed and reopened).
		mAlive->store(false);
	}

	SharedRequestProvider(std::function<SharedRequestDerivedType* ()> requestFactory) : mRequestFactory(std::move(requestFactory)) {}

};

// no extra functionality, just returns a base SharedRequestToken.
class TokenSharedRequestProvider : public SharedRequestProvider<SharedRequestToken>
{
public:
	TokenSharedRequestProvider() : SharedRequestProvider<SharedRequestToken>([]() { return new SharedRequestToken(); }) {}
};