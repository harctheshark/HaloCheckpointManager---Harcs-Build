#pragma once
#include "pch.h"
#include "IAnchorPoint.h"

// How an on-screen message is coloured (user request 2026-11-27: a feature that applied / turned on correctly in green,
// anything that failed in red). Auto = decided from the wording (MessagesGUI.cpp classifyMessage); Neutral = the
// user's Messages font colour. Pass Success / Failure explicitly where the wording would mislead the classifier.
enum class MessageKind { Auto, Neutral, Success, Failure };

class IMessagesGUI
{
public:
	virtual void addMessage(std::string message, MessageKind kind = MessageKind::Auto) = 0;
	virtual void setAnchorPoint(std::weak_ptr<IAnchorPoint> anchorPoint) = 0;
	virtual ~IMessagesGUI() = default;
};
