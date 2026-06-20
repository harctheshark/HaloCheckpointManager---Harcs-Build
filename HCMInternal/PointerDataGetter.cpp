#include "pch.h"
#include "PointerDataGetter.h"


namespace PointerDataGetter
{


    std::string getXMLDocument(std::string HCMdirPath)
    {
        constexpr std::string_view xmlFileName = "InternalPointerData.xml";



        auto pointerDataLocation = HCMdirPath + xmlFileName.data();

        std::string downloadFailureMessage = "Download not attempted";

        // RELEASE normally downloads the latest pointer data from GitHub, OVERWRITING the local file.
        // DISABLED in this fork (#if 0): we maintain custom offsets in the local InternalPointerData.xml
        // (e.g. the S5 ODST skull pointer) that a download would clobber. Always use the local file
        // regardless of build configuration.
#if 0
        try
        {
            PLOG_INFO << "Downloading pointer data from Github repo";
            downloadFileTo(githubPath, pointerDataLocation);
            downloadFailureMessage = "Download succeeded!";
        }
        catch (HCMInitException ex)
        {
            ex.prepend("Failed to download pointer data from Github repo. Download error: ");
            downloadFailureMessage = ex.what();

            if (!fileExists(pointerDataLocation)) // no local copy? we're doomed, throw the DL exception
            {
                throw HCMInitException(ex);
            }

        }
#endif
        (void)downloadFailureMessage;

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