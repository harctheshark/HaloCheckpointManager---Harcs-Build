#pragma once

template <typename valueType>
class ISmoother
{
public:
	virtual ~ISmoother() = default;
	virtual void smooth(valueType& currentValue, valueType desiredValue) = 0;

	// Discard any state carried between frames. A no-op for the stateless smoothers (Null, Linear); overridden
	// by MomentumSmoother, which carries a velocity that must not leak across a mode change or a camera
	// reposition. Defaulted rather than pure so the existing smoothers are untouched.
	virtual void reset() {}
};
