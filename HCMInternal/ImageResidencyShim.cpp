#include "pch.h"
#include "ImageResidencyGuard.h"
#include "ImageResidencyShim.h"

// See ImageResidencyShim.h. These mirror ScopedImageResidency's constructor and destructor exactly - same
// thread_local depth, same shared lock, same re-entrancy rule - so a caller that cannot include the real header
// still participates in ImageResidency::drain().

namespace ImageResidency
{
	void enterFromForeignTU() noexcept
	{
		// Re-entrancy: only the outermost entry on this thread takes the lock. A nested entry must NOT queue
		// behind a pending exclusive waiter, or a thread that already holds the shared lock deadlocks itself
		// against a drain that started in between. Same reasoning as ScopedImageResidency.
		const bool outermost = (tlsDepth == 0);
		++tlsDepth;
		if (outermost) guard().lock_shared();
	}

	void leaveFromForeignTU() noexcept
	{
		--tlsDepth;
		if (tlsDepth == 0) guard().unlock_shared();
	}
}
