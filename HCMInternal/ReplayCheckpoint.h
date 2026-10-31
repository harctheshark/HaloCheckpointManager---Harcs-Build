#pragma once
#include "pch.h"
#include "GameState.h"
#include "DIContainer.h"
#include "PointerDataStore.h"
#include "MultilevelPointer.h"
#include "InjectRequirements.h"
#include "IGetMCCVersion.h"

// The game's current checkpoint as bytes, for a replay file to carry (so one file is the whole replay: no separate
// dump / inject / revert). read() is DumpCheckpoint's read (the current buffer of a double-buffered game, the game DLL's
// version in the last 10 bytes); inject() is InjectCheckpoint's write (preserve locations, last 10 bytes zeroed, SHA,
// loaded-BSP data). Keep the three in step if either of those changes.
class ReplayCheckpoint
{
    GameState mGame;
    std::weak_ptr<IGetMCCVersion> getMCCVerWeak;
    std::shared_ptr<InjectRequirements> mReq;
    std::shared_ptr<int64_t> mLength;
    std::shared_ptr<MultilevelPointer> mLoc1, mLoc2, mDoubleRevertFlag, mLoadedBSP1, mLoadedBSP2;
    std::shared_ptr<PreserveLocations> mPreserve;
    std::shared_ptr<offsetLengthPair> mSHA, mBSP;

    uintptr_t currentLocation() const
    {
        uintptr_t loc = 0;
        if (mReq->singleCheckpoint) mLoc1->resolve(&loc);
        else
        {
            bool first = false;
            if (!mDoubleRevertFlag->readData(&first)) throw HCMRuntimeException("Replay checkpoint: could not read the double-revert flag");
            (first ? mLoc1 : mLoc2)->resolve(&loc);
        }
        if (!loc) throw HCMRuntimeException("Replay checkpoint: the checkpoint location did not resolve");
        return loc;
    }

public:
    // throws HCMInitException when the game has no checkpoint data
    ReplayCheckpoint(GameState game, IDIContainer& dicon)
        : mGame(game), getMCCVerWeak(dicon.Resolve<IGetMCCVersion>())
    {
        auto ptr = dicon.Resolve<PointerDataStore>().lock();
        mReq = ptr->getData<std::shared_ptr<InjectRequirements>>("injectRequirements", game);
        mLength = ptr->getData<std::shared_ptr<int64_t>>("checkpointLength", game);
        mLoc1 = ptr->getData<std::shared_ptr<MultilevelPointer>>("checkpointLocation1", game);
        if (!mReq->singleCheckpoint)
        {
            mLoc2 = ptr->getData<std::shared_ptr<MultilevelPointer>>("checkpointLocation2", game);
            mDoubleRevertFlag = ptr->getData<std::shared_ptr<MultilevelPointer>>("doubleRevertFlag", game);
        }
        if (mReq->preserveLocations) mPreserve = ptr->getData<std::shared_ptr<PreserveLocations>>("preserveLocations", game);
        if (mReq->SHA) mSHA = ptr->getData<std::shared_ptr<offsetLengthPair>>("SHAdata", game);
        if (mReq->BSP)
        {
            mBSP = ptr->getData<std::shared_ptr<offsetLengthPair>>("BSPdata", game);
            mLoadedBSP1 = ptr->getData<std::shared_ptr<MultilevelPointer>>("loadedBSP1", game);
            if (!mReq->singleCheckpoint) mLoadedBSP2 = ptr->getData<std::shared_ptr<MultilevelPointer>>("loadedBSP2", game);
        }
    }

    // the checkpoint a revert loads right now (as a dump writes it)
    std::vector<byte> read() const
    {
        const int64_t length = *mLength;
        const uintptr_t loc = currentLocation();
        if (IsBadReadPtr((void*)loc, length)) throw HCMRuntimeException(std::format("Replay checkpoint: bad read at 0x{:X}, length 0x{:X}", loc, length));
        std::vector<byte> data((size_t)length);
        std::memcpy(data.data(), (const void*)loc, (size_t)length);
        auto getMCCVer = getMCCVerWeak.lock();
        if (!getMCCVer) throw HCMRuntimeException("Replay checkpoint: the version service is gone");
        const std::string version(getMCCVer->getGameDataVersionAsString(mGame));
        if (version.size() != 10) throw HCMRuntimeException(std::format("Replay checkpoint: version string has size {}, expected 10", version.size()));
        std::copy(version.begin(), version.end(), data.end() - 10);
        return data;
    }

    // the game build a read() came from ("" if it carries none)
    static std::string versionOf(const std::vector<byte>& data)
    {
        if (data.size() < 10) return {};
        std::string v(data.end() - 10, data.end());
        return v.find('\0') == std::string::npos ? v : std::string{};
    }

    bool lengthMatches(const std::vector<byte>& data) const { return (int64_t)data.size() == *mLength; }

    // write it over the game's current checkpoint (a revert then loads it)
    void inject(std::vector<byte> data) const
    {
        const int64_t length = *mLength;
        if ((int64_t)data.size() != length) throw HCMRuntimeException(std::format("Replay checkpoint: size 0x{:X}, this game's is 0x{:X}", data.size(), length));
        const uintptr_t loc = currentLocation();
        if (IsBadWritePtr((void*)loc, length)) throw HCMRuntimeException(std::format("Replay checkpoint: bad write at 0x{:X}, length 0x{:X}", loc, length));

        if (mReq->preserveLocations)
            for (auto& [offset, vec] : mPreserve->locations)
                std::memcpy(data.data() + offset, (const void*)(loc + offset), vec.size());
        std::fill(data.end() - 10, data.end(), (byte)0);
        // ⚠ As InjectCheckpoint ACTUALLY behaves: it zeroes the SHA bytes (inclusive range) and its computed SHA never
        // lands (SHAout is reserve()d, so its size() - the copy length - is 0). Every injection HCM has done went in with a
        // zeroed SHA, so that is the tested state; writing a real one here would be the untested path.
        if (mReq->SHA)
            std::fill(data.begin() + mSHA->offset, data.begin() + mSHA->offset + mSHA->length + 1, (byte)0);
        std::memcpy((void*)loc, data.data(), (size_t)length);

        if (mReq->BSP)
        {
            const byte* bsp = (const byte*)(loc + mBSP->offset);
            std::vector<byte> current(bsp, bsp + mBSP->length);
            std::shared_ptr<MultilevelPointer> target = mLoadedBSP1;
            if (!mReq->singleCheckpoint)
            {
                bool first = false;
                mDoubleRevertFlag->readData(&first);
                target = first ? mLoadedBSP1 : mLoadedBSP2;
            }
            if (!target->writeArrayData(current.data(), current.size()))
                throw HCMRuntimeException(std::format("Replay checkpoint: failed to write the loaded-BSP data: {}", MultilevelPointer::GetLastError()));
        }
    }
};
