#pragma once

namespace PointerDataGetter
{
	// ⚠ THE UPSTREAM DOWNLOAD URL IS DELIBERATELY ABSENT. This fork reads InternalPointerData.xml from
	// disk and never fetches it - see the note in PointerDataGetter.cpp for why fetching upstream's copy
	// destroys this fork's own offsets. Do not re-add a githubPath constant here.
	std::string getXMLDocument(std::string HCMdirPath);
}