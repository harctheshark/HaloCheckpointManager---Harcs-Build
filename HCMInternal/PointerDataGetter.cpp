#include "pch.h"
#include "PointerDataGetter.h"


namespace PointerDataGetter
{


    std::string getXMLDocument(std::string HCMdirPath)
    {
        constexpr std::string_view xmlFileName = "InternalPointerData.xml";



        auto pointerDataLocation = HCMdirPath + xmlFileName.data();

        // ⚠⚠⚠ THIS FORK NEVER DOWNLOADS POINTER DATA. DO NOT REINSTATE IT.
        //
        // Upstream HCM fetches InternalPointerData.xml from
        //     raw.githubusercontent.com/Burnt-o/HaloCheckpointManager/master/HCMInternal/InternalPointerData.xml
        // on startup and OVERWRITES the local file with it. That is correct for upstream and actively
        // destructive here: this fork maintains its own offsets in that file (the S5 ODST skull pointer,
        // the HaloCER entries, the Halo 5: Forge entries), none of which exist upstream. A download
        // silently replaces all of them with a file that does not know those games exist, so the user's
        // HCM breaks on next launch through no action of their own - and it re-breaks every launch.
        //
        // It was previously left in place behind `#if 0`. It is now DELETED outright, along with the URL
        // constant, so it cannot be revived by flipping a preprocessor switch or by a careless merge from
        // upstream. If a future merge reintroduces a call to downloadFileTo() pointing at Burnt-o/master,
        // that is a regression, not a feature.
        //
        // Keeping pointer data current is a RELEASE-TIME job for this fork: update the local XML, commit
        // it, and ship it in the zip.

        if (!fileExists(pointerDataLocation)) // no local copy? we're doomed, throw
        {
            throw HCMInitException(std::format("No pointer data existed at location {}", pointerDataLocation));
        }
        else
        {
            PLOG_VERBOSE << "Pointer Data getting contents of file at: " << pointerDataLocation;
        }

        return readFileContents(pointerDataLocation);

    }
}