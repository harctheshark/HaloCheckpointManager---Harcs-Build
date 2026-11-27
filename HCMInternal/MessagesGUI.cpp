#include "pch.h"
#include "MessagesGUI.h"
#include "imgui.h"
#include "Lapua.h"
#include "RenderTextHelper.h"

void MessagesGUI::onImGuiRenderEvent(SimpleMath::Vector2 screenSize)
{
	std::unique_lock<std::mutex> lock(mDestructionGuard);

	bool hideMessages = false;
	if (mSettingsWeak.has_value())
	{
		auto mSettings = mSettingsWeak->lock();
		if (mSettings)
		{
			hideMessages = Lapua::lapuaGood && Lapua::lapuaGood2 && mSettings->hideWatermark->GetValue() && mSettings->hideWatermarkHideMessages->GetValue();
		}
	}


	if (hideMessages == false)
	{
		iterateMessages();
	}


}



// in milliseconds
constexpr float messageExpiryTimeout = 12000.f;
constexpr float messageFullOpacityTime = 8000.f;
constexpr float messageFadeTime = messageExpiryTimeout - messageFullOpacityTime;

void MessagesGUI::iterateMessages()
{
	std::unique_lock<std::mutex> lock(addMessageMutex);
	if (mMessages.empty()) return;

	// Calculate age of each message
	auto currentTime = std::chrono::high_resolution_clock::now();
	for (auto& message : mMessages)
	{
		message.messageAge = std::chrono::duration<float, std::milli>(currentTime - message.timeStamp).count();
	}

	// remove old messages from the list

	std::erase_if(mMessages, [&currentTime](const auto& message)
		{
			bool erase = message.messageAge > messageExpiryTimeout;
			//if (erase) PLOG_VERBOSE << "erasing expired message";
			return erase;
		});

	// draw remaining messages
	LOG_ONCE_THIS(PLOG_DEBUG << "mAnchorPoint exists? " << mAnchorPoint.has_value());
	SimpleMath::Vector2 messagePosition;
	if (mAnchorPoint.has_value())
	{
		auto anc = mAnchorPoint.value().lock();
		if (!anc)
		{
#if HCM_DEBUG
			PLOG_ERROR << "mAnchorPoint bad weak ptr";
#else
			LOG_ONCE(PLOG_ERROR << "mAnchorPoint bad weak ptr (at least once)");
#endif

			messagePosition = mAnchorOffset;
		}
		else
		{
			messagePosition = anc->getAnchorPoint() + mAnchorOffset;
		}

	}
	else
	{
		messagePosition = mAnchorOffset;
	}


	for (auto& message : mMessages)
	{
		drawMessage(message, messagePosition);
		// update messagePosition based on previous messages linecount
		messagePosition = SimpleMath::Vector2(messagePosition.x, messagePosition.y + (message.lineCount * (fontSize * 1.5f)) + 5.f);
	}
}

// returns vertical pixel height of message
void MessagesGUI::drawMessage(const temporaryMessage& message, const SimpleMath::Vector2& position)
{
	// Calculate opacity using message age
	float opacity = 1.f; 
	float delta = (message.messageAge - messageFullOpacityTime);
	if (delta > 0)
	{
		opacity = (messageFadeTime - delta) / messageFadeTime;
	}

	// green = a feature applied / turned on, red = something failed, otherwise the user's Messages colour
	ImColor textColor(fontColor.x, fontColor.y, fontColor.z, opacity);
	if (message.kind == MessageKind::Success) textColor = ImColor(0.30f, 1.00f, 0.38f, opacity);
	else if (message.kind == MessageKind::Failure) textColor = ImColor(1.00f, 0.24f, 0.24f, opacity);

#ifdef HCM_DEBUG // these checks are probably bad for performance
	if (position.x < 0) PLOG_FATAL << "position.x < 0: " << position.x;
	if (position.y < 0) PLOG_FATAL << "position.y < 0: " << position.y;
	if (message.message.size() == 0) PLOG_FATAL << "message size == 0";

#endif
	RenderTextHelper::drawOutlinedText(message.message, position, textColor, fontSize);

}

//https://stackoverflow.com/a/27757111
// Prefers inserting new lines at spaces instead of splitting words
std::string insertNewLines(const std::string& in, const size_t every_n, __int64& outLineCount)
{
	PLOG_VERBOSE << "insertNewLines";

	if (in.size() < every_n)
	{
		outLineCount = std::count(in.begin(), in.end(), '\n');
		outLineCount++;
		return in;
	}

	std::string temp;
	std::vector<std::string> lines;
	for (std::string::size_type i = 0; i < in.size(); i++)
	{
		//PLOG_VERBOSE << (int)in[i];
		if (in[i] == '\n')
		{
			//PLOG_DEBUG << "Found preexisting endline";
			lines.push_back(temp);
			temp.clear();
		}
		else
		{
			if (temp.size() == every_n)
			{
				auto lastSpace = temp.find_last_of(' ');
				if (lastSpace != std::string::npos && temp.length() - lastSpace < 20) // check the last space isn't too far back
				{
					lines.push_back(temp.substr(0, lastSpace));
					temp.erase(0, lastSpace);
				}
				else // space was too far back
				{
					lines.push_back(temp);
					temp.clear();
				}
			}
			temp.push_back(in[i]);
		}

	}

	if (temp.size() > 0) lines.push_back(temp); // put the last line in

	temp.clear();
	for (auto& line : lines)
	{
		temp.append(line);
		temp.push_back('\n');
	}

	// get rid of extra last newLineChar at the end
	temp.pop_back();
	outLineCount = lines.size();
	PLOG_VERBOSE << "lineCount: " << outLineCount;
	return temp;


}


namespace
{
	bool isWordChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

	// whole-word search in an already lower-cased string ("enabled" matches "re-enabled." but not "disabled")
	bool hasWord(const std::string& s, std::string_view w)
	{
		for (size_t p = s.find(w); p != std::string::npos; p = s.find(w, p + 1))
		{
			const bool startOk = p == 0 || !isWordChar(s[p - 1]);   // "re-enabled": the '-' is not a word char
			const bool endOk = p + w.size() >= s.size() || !isWordChar(s[p + w.size()]);
			if (startOk && endOk) return true;
		}
		return false;
	}

	// "X on", "X on.", "X on (experimental)", "X on: hold jump" - but not "on tick 5" (a preposition) and not an
	// instruction ("then toggle on", "turn it on")
	bool saysTurnedOn(const std::string& s)
	{
		for (size_t p = s.find("on"); p != std::string::npos; p = s.find("on", p + 1))
		{
			if (p > 0 && isWordChar(s[p - 1])) continue;
			size_t e = p + 2;
			if (e < s.size() && isWordChar(s[e])) continue;
			while (e < s.size() && s[e] == ' ') ++e;
			if (e < s.size() && std::string_view(".:!(-,;").find(s[e]) == std::string_view::npos) continue;
			const std::string before = s.substr(0, p);
			static constexpr std::string_view kImperative[] = { "toggle ", "turn ", "switch ", "turn it ", "it ", "them ", "back " };
			bool instruction = false;
			for (auto i : kImperative)
				if (before.size() >= i.size() && before.compare(before.size() - i.size(), i.size(), i) == 0) instruction = true;
			if (!instruction) return true;
		}
		return false;
	}

	MessageKind classifyMessage(const std::string& message)
	{
		std::string s(message);
		for (auto& c : s) c = (char)std::tolower((unsigned char)c);

		// failure first: "a graphics error turned it off" is red, not green
		static constexpr std::string_view kFailure[] = {
			"fail", "couldn't", "could not", "can't", "cannot", "unable to", "not found", "missing", "refus", "rejected",
			"invalid", "mismatch", "wrong build", "wrong file", "wrong version", "not supported", "unsupported", "exception",
			"crash", "timed out", "too large", "don't match", "doesn't match", "did not", "didn't", "not available",
			"unavailable", "broken", "not compatible", "incompatible", "aborted" };
		for (auto w : kFailure) if (s.find(w) != std::string::npos) return MessageKind::Failure;
		// "error" is a failure unless it is a measurement ("max error 0.2 wu")
		for (size_t p = s.find("error"); p != std::string::npos; p = s.find("error", p + 1))
		{
			const bool measured = (p >= 4 && (s.compare(p - 4, 4, "max ") == 0 || s.compare(p - 4, 4, "min ") == 0))
				|| (p >= 5 && s.compare(p - 5, 5, "mean ") == 0);
			if (!measured) return MessageKind::Failure;
		}

		// turning something OFF is not a success ("Player Model Swap off - ... the next time it is loaded"); "off-screen"
		// is a compound word, not "off"
		for (size_t p = s.find("off"); p != std::string::npos; p = s.find("off", p + 1))
		{
			const bool startOk = p == 0 || !isWordChar(s[p - 1]);
			const bool endOk = p + 3 >= s.size() || (!isWordChar(s[p + 3]) && s[p + 3] != '-');
			if (startOk && endOk) return MessageKind::Neutral;
		}
		if (hasWord(s, "disabled") || hasWord(s, "disabling")) return MessageKind::Neutral;

		// a negated or conditional sentence is never a success ("no trigger volumes loaded yet", "not ready")
		if (hasWord(s, "not") || hasWord(s, "no") || hasWord(s, "nothing") || hasWord(s, "never") || s.find("n't") != std::string::npos)
			return MessageKind::Neutral;

		static constexpr std::string_view kSuccess[] = {
			"enabled", "applied", "armed", "loaded", "saved", "started", "forced", "reverted", "attached", "installed",
			"activated", "engaged", "succeeded", "successful", "successfully", "done", "ready" };   // not "restored": that is a feature turning off
		for (auto w : kSuccess) if (hasWord(s, w)) return MessageKind::Success;
		if (saysTurnedOn(s)) return MessageKind::Success;
		return MessageKind::Neutral;
	}
}

void MessagesGUI::addMessage(std::string message, MessageKind kind)
{
	std::unique_lock<std::mutex> lock(addMessageMutex);
	if (kind == MessageKind::Auto) kind = classifyMessage(message);

	if (message.size() == 0)
	{
		HCMRuntimeException ex("Message added with zero length!");
		message = std::format("{}\n{}", ex.what(), ex.trace());
		PLOG_ERROR << message;
	}

	// split message to multiple lines if necessary
	__int64 lineCount;
	message = insertNewLines(message, 150, lineCount);
	PLOG_DEBUG << "Message added with linecount: " << lineCount;
	PLOG_INFO << "MessagesGUI added message: " << message;
	//instance->messages.emplace_back(temporaryMessage{message, std::chrono::high_resolution_clock::now(), lineCount});
	mMessages.insert(mMessages.begin(), temporaryMessage{ message, std::chrono::high_resolution_clock::now(), lineCount, 0.f, kind }); // new messages now go at the top instead of the bottom

}

