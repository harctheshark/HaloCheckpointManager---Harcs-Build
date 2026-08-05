
using HCMExternal.Models;
using Serilog;
using System;
using System.IO;
using System.Linq;

namespace HCMExternal.Services.CheckpointIO.Impl
{
    public partial class CheckpointIOService
    {
        /// <summary>
        /// Decodes a checkpoint file into a Checkpoint (model), reading off useful data like the level, difficulty, gametime, etc.
        /// </summary>
        /// <param name="checkpointFile">The checkpoint file.</param>
        /// <param name="gameEnum">Which game is this checkpoint from?</param>
        /// <returns></returns>
        public Checkpoint? DecodeCheckpointFile(FileInfo checkpointFile, HaloGame gameEnum)
        {


            // Setup checkpoint data we're going to try to populate
            string? checkpointName = null;
            string? checkpointLevelCode = null;
            int? checkpointDifficulty = null;
            int? checkpointGameTickCount = null;
            string? checkpointVersion = null;
            DateTime checkpointCreatedOn = checkpointFile.CreationTime;
            DateTime checkpointModifiedOn = checkpointFile.LastWriteTime;

            // We want to trim off the ".bin" part of the filename.
            checkpointName = checkpointFile.Name;
            checkpointName = checkpointName[..checkpointName.LastIndexOf(".")];

            // We don't allow checkpoints with empty/null filenames
            if (checkpointName == null || checkpointName == "")
            {
                return null;
            }

            // Next, check if the filesize is reasonable. If not we'll return null.
            // Valid range is 100kb to 100mb, pulled out my ass.
            if (checkpointFile.Length < (100 * 1000) || checkpointFile.Length > (100 * 1000 * 1000))
            {
                return null;
            }

            // Halo Campaign Evolved leaves here and never touches the MCC decode below. It has to: that path is
            // driven by PointerData entries keyed on an MCC version string, and a HaloCER dump has neither (the
            // file is the engine's blob VERBATIM - HCM must not stamp a version into it, because the engine's
            // SHA-1 covers the whole buffer and a mismatch does not fail softly, it restarts the level).
            if (gameEnum == HaloGame.HaloCER)
            {
                return DecodeHaloCERCheckpointFile(checkpointFile, checkpointName, checkpointCreatedOn, checkpointModifiedOn);
            }

            // Now load the whole damn file into memory and read some data from it
            // If it fails then we'll leave the associated values as null
            try
            {
                using (FileStream readStream = new FileStream(checkpointFile.FullName, FileMode.Open))
                {
                    using (BinaryReader readBinary = new BinaryReader(readStream))
                    {
                        // Go to the end of the checkpoint file and see if there's a valid version string written there.
                        // (Version string is 10 chars long)
                        readBinary.BaseStream.Seek(-10, SeekOrigin.End);
                        string? checkpointVersionGuess = new string(readBinary.ReadChars(10));
                        checkpointVersionGuess = checkpointVersionGuess.Replace("\0", string.Empty); // remove null chracters
                        Log.Verbose("read ver string: " + checkpointVersionGuess);

                        // If that string started with "1." (or "0." for project cartographer) then it's probably a valid version string, so we'll set checkpointVersion to it too
                        if (checkpointVersionGuess != null && (checkpointVersionGuess.StartsWith("1.") || checkpointVersionGuess.StartsWith("0.")))
                            checkpointVersion = checkpointVersionGuess;
                        else if (gameEnum == HaloGame.ProjectCartographer && PointerData.HighestSupportedCartographerVersion != null) //otherwise set it the highest supported version we loaded from git (cartographer)
                            checkpointVersionGuess = PointerData.HighestSupportedCartographerVersion;
                        else if (PointerData.HighestSupportedMCCVersion != null) //otherwise set it the highest supported version we loaded from git (mcc)
                            checkpointVersionGuess = PointerData.HighestSupportedMCCVersion;
                        else
                            checkpointVersionGuess = null; // otherwise.. give up

                        // Now let's try to actually read the data from the file
                        // Each will be in it's own trycatch so that one can fail without the others failing
                        if (checkpointVersionGuess != null)
                        {
                            // Need this for accessing pointercollection
                            string gameString = gameEnum.ToAcronym();
                            // Read LevelCode
                            try
                            {
                                // Grab the offset; if it exists
                                int? offsetLevelCode = PointerData.GetData<int>(gameString + "_CheckpointData_LevelCode", checkpointVersionGuess);
                                if (offsetLevelCode.HasValue)
                                {
                                    // Use the offset to seek to the correct location and read the data
                                    readBinary.BaseStream.Seek((long)offsetLevelCode, SeekOrigin.Begin);
                                    checkpointLevelCode = new string(readBinary.ReadChars(64));
                                    checkpointLevelCode = checkpointLevelCode.Substring(checkpointLevelCode.LastIndexOf("\\") + 1);
                                    checkpointLevelCode = checkpointLevelCode.Substring(checkpointLevelCode.LastIndexOf("\\") + 1);
                                    char[] exceptions = new char[] { '_' };
                                    checkpointLevelCode = string.Concat(checkpointLevelCode.Where(ch => char.IsLetterOrDigit(ch) || exceptions?.Contains(ch) == true));
                                    Log.Verbose("\\nlevelcode4: " + checkpointLevelCode);
                                }
                            }
                            catch { checkpointLevelCode = null; }

                            // Read GameTickCount
                            try
                            {
                                // Grab the offset; if it exists
                                int? offsetGameTickCount = PointerData.GetData<int>(gameString + "_CheckpointData_GameTickCount", checkpointVersionGuess);
                                if (offsetGameTickCount.HasValue)
                                {
                                    // Use the offset to seek to the correct location and read the data
                                    readBinary.BaseStream.Seek((long)offsetGameTickCount, SeekOrigin.Begin);
                                    // read integer
                                    checkpointGameTickCount = readBinary.ReadInt32();
                                }
                            }
                            catch { checkpointGameTickCount = null; }

                            // Read Difficulty
                            try
                            {
                                // Grab the offset; if it exists
                                int? offsetDifficulty = PointerData.GetData<int>(gameString + "_CheckpointData_Difficulty", checkpointVersionGuess);
                                if (offsetDifficulty.HasValue)
                                {
                                    // Use the offset to seek to the correct location and read the data
                                    readBinary.BaseStream.Seek((long)offsetDifficulty, SeekOrigin.Begin);
                                    // read 1 byte as int
                                    checkpointDifficulty = readBinary.ReadBytes(1)[0];
                                }
                            }
                            catch { checkpointDifficulty = null; }
                        }
                    }
                }
            }
            catch (Exception e) // Reading from file failed, so set those checkpoint values to null
            {
                Log.Error("Failed reading checkpoint from file: " + e.Message);
                checkpointLevelCode = null;
                checkpointDifficulty = null;
                checkpointGameTickCount = null;
            }

            // Return whatever data we were able to get (all might be null except checkpointName, createdOn, and ModifiedOn)
            return new Checkpoint(checkpointName, checkpointFile.FullName, checkpointLevelCode, checkpointDifficulty, checkpointGameTickCount, checkpointVersion, checkpointCreatedOn, checkpointModifiedOn); // Then return a new checkpoint with as many valid properties as we managed to get (checkpointName is definitely not null)
        }


        // ============================================================================================================
        // Halo Campaign Evolved. Separate function on purpose - it shares NOTHING with the MCC decode above and must
        // not be able to affect it.
        //
        // A HaloCER checkpoint is a 0x1ED38-byte session header followed by the whole game state. The only field
        // worth showing in the list is the SCENARIO PATH at blob+0x008 (NUL-terminated ASCII, at most 0x100 bytes) -
        // it is the thing the engine strcmp's when it decides whether a checkpoint belongs to the level you are on,
        // so it is the closest analogue of MCC's level code. See HCMInternal/HCECheckpointBlob.h (HeaderOffsets).
        //
        // Difficulty, in-game time and game version are deliberately left null:
        //   * the difficulty byte lives inside the game-options struct and its encoding is NOT confirmed to match
        //     HCM's 0=Easy..3=Legendary map, and showing the wrong difficulty is worse than showing "???";
        //   * HaloCER has no version resource (every build reports 0.0.0.0) and nothing may be stamped into a dump.
        // HCMInternal validates all of this against the LIVE session header at inject time anyway, so none of it is
        // load-bearing here - it is list decoration.
        // ============================================================================================================
        private const long HaloCERScenarioPathOffset = 0x008;
        private const int HaloCERScenarioPathMaxLength = 0x100;

        private Checkpoint DecodeHaloCERCheckpointFile(FileInfo checkpointFile, string checkpointName, DateTime createdOn, DateTime modifiedOn)
        {
            string? levelCode = null;

            try
            {
                using FileStream readStream = new FileStream(checkpointFile.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                using BinaryReader readBinary = new BinaryReader(readStream);

                readBinary.BaseStream.Seek(HaloCERScenarioPathOffset, SeekOrigin.Begin);
                byte[] raw = readBinary.ReadBytes(HaloCERScenarioPathMaxLength);

                // Bounded, NUL-terminated. An unterminated field just uses the whole 0x100 bytes.
                int length = Array.IndexOf<byte>(raw, 0);
                if (length < 0) length = raw.Length;

                string scenarioPath = System.Text.Encoding.ASCII.GetString(raw, 0, length);

                // Take the leaf, the way the MCC decode does - the list's Level column is 35px wide.
                int lastSeparator = scenarioPath.LastIndexOfAny(new[] { '\\', '/' });
                string leaf = lastSeparator >= 0 ? scenarioPath.Substring(lastSeparator + 1) : scenarioPath;
                leaf = string.Concat(leaf.Where(ch => char.IsLetterOrDigit(ch) || ch == '_'));

                if (leaf.Length > 0)
                    levelCode = leaf;

                Log.Verbose("HaloCER scenario path: '" + scenarioPath + "' -> level code '" + (levelCode ?? "(none)") + "'");
            }
            catch (Exception e)
            {
                // A file we cannot read the header of is still a file the user can select and inject - HCMInternal
                // does the real validation. So this never refuses the checkpoint, it just leaves the column blank.
                Log.Error("Failed reading HaloCER checkpoint header from file: " + e.Message);
                levelCode = null;
            }

            return new Checkpoint(checkpointName, checkpointFile.FullName, levelCode, null, null, null, createdOn, modifiedOn);
        }
    }
}
