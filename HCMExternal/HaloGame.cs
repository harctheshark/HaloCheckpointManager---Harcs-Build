using System;
using System.Collections.Generic;

namespace HCMExternal
{

    // ⚠ THE VALUE IS THE TAB INDEX. MainWindow.xaml binds TabControl.SelectedIndex straight to
    // FileViewModel.SelectedGame through EnumConverter, and Settings.LastSelectedFolder is indexed by (int)value.
    // So a new game goes on the END, after the last existing one, and every existing value stays put.
    public enum HaloGame
    {
        Halo1 = 0,
        Halo2 = 1,
        Halo3 = 2,
        Halo3ODST = 3,
        HaloReach = 4,
        Halo4 = 5,
        ProjectCartographer = 6,

        // Halo Campaign Evolved. NOT an MCC game and NOT a Cartographer game - a separate UE5 title
        // (HaloCampaignEvolved.exe) that HCMInternal supports via its own HCEDumpCheckpoint/HCEInjectCheckpoint
        // cheats. HCMExternal never touches its process; this tab only browses files and publishes the selection
        // through shared memory, exactly like the MCC tabs do.
        HaloCER = 7
    }


    public static class HaloGameMethods
    {

        public static int ToInternalIndex(this HaloGame gameEnum)
        {
            switch (gameEnum)
            {
                case HaloGame.Halo1: return 0;
                case HaloGame.Halo2: return 1;
                case HaloGame.Halo3: return 2;
                case HaloGame.Halo3ODST: return 5;
                case HaloGame.HaloReach: return 6;
                case HaloGame.Halo4: return 3;
                case HaloGame.ProjectCartographer: return 7;
                // HCMInternal's GameState::Value::HaloCER. Deliberately 8, not 7: 7 is Cartographer's index here
                // and is not a GameState value at all, and 8 is past MCC's gameIndicator range so it can never
                // collide with one. See HCMInternal/GameState.h.
                case HaloGame.HaloCER: return 8;
                default: throw new System.Exception("Invalid HaloGame index: " + gameEnum);
            }
        }


        public static string ToRootFolderPath(this HaloGame gameEnum)
        {
            switch (gameEnum)
            {
                case HaloGame.Halo1: return @"Halo 1";
                case HaloGame.Halo2: return @"Halo 2";
                case HaloGame.Halo3: return @"Halo 3";
                case HaloGame.Halo3ODST: return @"Halo 3 ODST";
                case HaloGame.HaloReach: return @"Halo Reach";
                case HaloGame.Halo4: return @"Halo 4";
                case HaloGame.ProjectCartographer: return @"Project Cartographer";
                // Must stay in lockstep with App.CheckRequiredFoldersAndFiles' _requiredFolders - HCM creates the
                // root folders on startup and PopulateSaveFolderTree shouts at the user if one is missing.
                case HaloGame.HaloCER: return @"Halo Campaign Evolved";
                default: throw new System.Exception("Invalid HaloGame index: " + gameEnum);
            }
        }

        public static string ToAcronym(this HaloGame gameEnum)
        {
            switch (gameEnum)
            {
                case HaloGame.Halo1: return @"H1";
                case HaloGame.Halo2: return @"H2";
                case HaloGame.Halo3: return @"H3";
                case HaloGame.Halo3ODST: return @"OD";
                case HaloGame.HaloReach: return @"HR";
                case HaloGame.Halo4: return @"H4";
                case HaloGame.ProjectCartographer: return @"PC";
                // Used as a PointerData key prefix ("CE_CheckpointData_...") and an Images subfolder. Neither
                // exists, and neither needs to: DecodeCheckpointFile short-circuits HaloCER before it builds a
                // pointer key, and the level-image converter short-circuits before it builds an image path.
                case HaloGame.HaloCER: return @"CE";
                default: throw new System.Exception("Invalid HaloGame index: " + gameEnum);
            }
        }
        

        public static HaloGame FromAcronym(string acronym)
        {
            switch (acronym)
            {
                case "H1": return HaloGame.Halo1;
                case "H2": return HaloGame.Halo2;
                case "H3": return HaloGame.Halo3;
                case "OD": return HaloGame.Halo3ODST;
                case "HR": return HaloGame.HaloReach;
                case "H4": return HaloGame.Halo4;
                case "PC": return HaloGame.ProjectCartographer;
                case "CE": return HaloGame.HaloCER;
                default: throw new System.Exception("Invalid HaloGame acronym: " + acronym);
            }
        }
    }
}
