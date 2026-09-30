using GongSolutions.Wpf.DragDrop;
using HCMExternal.Models;
using HCMExternal.Services.CheckpointIO;
using HCMExternal.Services.External;
using HCMExternal.Services.Interproc;
using HCMExternal.ViewModels.Commands;
using HCMExternal.ViewModels.Interfaces;
using Serilog;
using System;
using System.Collections;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Windows;
using System.Windows.Data;


namespace HCMExternal.ViewModels
{

    public partial class FileViewModel : Presenter, IDropTarget
    {


        public ObservableCollection<Checkpoint> CheckpointCollection { get; private set; }
        public ObservableCollection<SaveFolder> SaveFolderHierarchy { get; private set; }
        public SaveFolder RootSaveFolder { get; private set; }


        private Checkpoint? _selectedCheckpoint = null;
        public Checkpoint? SelectedCheckpoint
        {
            get => _selectedCheckpoint;
            set
            {
                _selectedCheckpoint = value;
                OnPropertyChanged(nameof(SelectedCheckpoint));
                _interprocService.UpdateSharedMemCheckpoint(SelectedGame, SelectedCheckpoint);

                if (value == null)
                {
                    Log.Debug("Selected checkpoint set to null (ie no checkpoint selected)");
                }

                // serialise
                Properties.Settings.Default.LastSelectedCheckpoint = _selectedCheckpoint?.CheckpointName;
                Properties.Settings.Default.Save();

            }
        }

        private SaveFolder _selectedSaveFolder;
        public SaveFolder SelectedSaveFolder
        {
            get => _selectedSaveFolder;
            set
            {
                if (_selectedSaveFolder != value)
                {
                    Log.Verbose(string.Format("Setting saveFolderPath to {0}", value.SaveFolderPath));
                }

                _selectedSaveFolder = value;
                OnPropertyChanged(nameof(SelectedSaveFolder));
                _interprocService.UpdateSharedMemSaveFolder(SelectedGame, SelectedSaveFolder);
                PublishDumpFolder(SelectedGame, _selectedSaveFolder.SaveFolderPath);

                // serialise
                // ⚠ GROW it, never replace it. This collection is indexed by (int)HaloGame, so the moment a game is
                // added every previously-saved collection is one short - and the old "replace with N blanks" would
                // then have thrown away every MCC tab's remembered folder on the user's first launch of this build.
                // Padding keeps them. The length tracks the enum, so adding another game needs no edit here.
                int gameCount = Enum.GetValues(typeof(HaloGame)).Length;
                if (Properties.Settings.Default.LastSelectedFolder == null)
                {
                    Properties.Settings.Default.LastSelectedFolder = new StringCollection();
                }
                while (Properties.Settings.Default.LastSelectedFolder.Count < gameCount)
                {
                    Properties.Settings.Default.LastSelectedFolder.Add("");
                }
                Properties.Settings.Default.LastSelectedFolder[(int)SelectedGame] = _selectedSaveFolder.SaveFolderPath;
                Properties.Settings.Default.Save();

                UpdateCheckpointCollection();
                // set selected checkpoint to top of the list
                if (CheckpointCollection.Count > 0)
                {
                    SelectedCheckpoint = CheckpointCollection.ElementAt(0);
                }
            }
        }

        private HaloGame _selectedGame = HaloGame.Halo1;
        public HaloGame SelectedGame
        {
            get => _selectedGame;
            set
            {
                Log.Verbose("SelectedGame changed from " + SelectedGame.ToString() + " to " + value.ToString());
                _selectedGame = value;
                OnPropertyChanged(nameof(SelectedGame)); // update ui

                // update save folder and checkpoints
                UpdateSaveFolderCollection();
                UpdateCheckpointCollection();

                // serialise
                HCMExternal.Properties.Settings.Default.LastSelectedGameTab = (int)SelectedGame;
                Properties.Settings.Default.Save();
            }
        }

        [Obsolete("Only for design data", true)]
        public FileViewModel()
        {
            if (!IsInDesignModeStatic)
            {
                throw new Exception("Use only for design mode");
            }

        }


        public MainVMCommands MainVMCommands { get; init; }

        private readonly FileSystemWatcher? SaveFileWatcher = new FileSystemWatcher(@"Saves\", "*.bin");
        private readonly FileSystemWatcher? SaveDirWatcher = new FileSystemWatcher(@"Saves\");



        private readonly SynchronizationContext _syncContext;
        private ICheckpointIOService _checkpointIOService { get; init; }
        private IExternalService _externalService { get; init; }
        private IInterprocService _interprocService { get; init; }
        public FileViewModel(ICheckpointIOService cpio, IExternalService ex, IInterprocService ip, MainVMCommands mvmCommands)
        {
            MainVMCommands = mvmCommands;
            _syncContext = SynchronizationContext.Current ?? throw new Exception("Null Synchronization Context ?!?!");
            Log.Verbose("FileViewModel constructing");
            _checkpointIOService = cpio;
            _externalService = ex;
            _interprocService = ip;
            CheckpointCollection = new();
            SaveFolderHierarchy = new();
            RootSaveFolder = null;

            // checkpoint collection is always sorted by LastWriteTime. 
            ListCollectionView view = (ListCollectionView)CollectionViewSource
                    .GetDefaultView(CheckpointCollection);

            view.CustomSort = new SortCheckpointsByLastWriteTime();
        }
          



        public void UpdateCheckpointCollection()
        {

            // store old selected checkpoint
            string? oldCP = SelectedCheckpoint?.CheckpointName;

            CheckpointCollection.Clear();
            Log.Debug("Populating checkpoint list with data from folder: " + SelectedSaveFolder.SaveFolderPath);

            ObservableCollection<Checkpoint> newCollection = _checkpointIOService.PopulateCheckpointList(SelectedSaveFolder, SelectedGame);
            foreach (Checkpoint c in newCollection)
            {
                CheckpointCollection.Add(c);
            }
            Log.Debug("refreshed CheckpointCollection, count: " + CheckpointCollection.Count);

            // try to reselect old checkpoint
            if (oldCP != null)
            {
                foreach (Checkpoint cp in CheckpointCollection)
                {
                    if (cp.CheckpointName == oldCP)
                    {
                        Log.Verbose("Succesfully reselected old checkpoint, name: " + oldCP);
                        SelectedCheckpoint = cp;
                        break;
                    }
                }
            }
        }

        public void UpdateSaveFolderCollection()
        {

            SaveFolderHierarchy.Clear();
            ObservableCollection<SaveFolder> newHierarchy = _checkpointIOService.PopulateSaveFolderTree(out SaveFolder rootFolder, SelectedGame);
            RootSaveFolder = rootFolder;
            foreach (SaveFolder s in newHierarchy)
            {
                SaveFolderHierarchy.Add(s);
            }


            // Now let's try to set the selected folder to whatever folder was last selected on this tab
            if (RootSaveFolder != null)
            {
                bool ableToSetLastFolder = false; // a flag that can be set in foreach loop and used for root folder fallback if not set

                string? lastSelectedFolder = null;
                if (Properties.Settings.Default.LastSelectedFolder != null && Properties.Settings.Default.LastSelectedFolder.Count > (int)SelectedGame)
                {
                    lastSelectedFolder = Properties.Settings.Default.LastSelectedFolder?[(int)SelectedGame];
                }

                if (lastSelectedFolder != null)
                {
                    IEnumerable<SaveFolder> flattenedTree = FlattenTree(RootSaveFolder);
                    foreach (SaveFolder sf in flattenedTree)
                    {
                        // If it matches!
                        if (sf.SaveFolderPath == lastSelectedFolder && Directory.Exists(lastSelectedFolder))
                        {
                            Trace.WriteLine("Resetting last selected folder to " + sf.SaveFolderPath);
                            sf.IsSelected = true;
                            ableToSetLastFolder = true;
                            SelectedSaveFolder = sf;
                            break;
                        }
                    }
                }
                if (!ableToSetLastFolder)
                {
                    // If we weren't able to reset it then default to root folder
                    RootSaveFolder.IsSelected = true;
                    SelectedSaveFolder = RootSaveFolder;
                }

            }
            else
            {
                Log.Error("Oh dear god, the root folder is null");
            }




            IEnumerable<SaveFolder> FlattenTree(SaveFolder node)
            {
                if (node == null)
                {
                    yield break;
                }
                yield return node;
                foreach (SaveFolder n in node.Children)
                {
                    foreach (SaveFolder innerN in FlattenTree(n))
                    {
                        yield return innerN;
                    }
                }
            }
        }




        // ⚠ HCMINTERNAL DUMPS THE RUNNING GAME, NOT THE VISIBLE TAB. It used to demand that the visible tab matched the
        // game being played ("Wrong game tab selected in external window!"), because the selected-folder slot in shared
        // memory holds one folder for whichever tab is showing. So EVERY game's folder is published as well, and
        // HCMInternal picks the running game's. Call after initializeSharedMemory (anything earlier is dropped) and
        // whenever the Saves\ tree changes, so a remembered folder deleted in Explorer falls back to the root.
        public void PublishDumpFoldersForAllGames()
        {
            foreach (HaloGame game in Enum.GetValues(typeof(HaloGame)))
            {
                // The visible tab: exactly what it has selected. Every other game: what its tab WOULD select if it
                // were opened now, so the dump shows up there when the user switches to it.
                string path = game == SelectedGame && SelectedSaveFolder != null
                    ? SelectedSaveFolder.SaveFolderPath
                    : ResolveRememberedDumpFolder(game);
                PublishDumpFolder(game, path);
            }
        }

        private void PublishDumpFolder(HaloGame game, string folderPath)
        {
            // HCMInternal never dumps for Cartographer, and its internal index (7) is not a GameState at all.
            if (game == HaloGame.ProjectCartographer)
            {
                return;
            }
            _interprocService.UpdateSharedMemGameDumpFolder(game, DumpFolderLabel(folderPath), folderPath);
        }

        // Mirrors UpdateSaveFolderCollection's restore without building a tree (and without its "root folder missing"
        // MessageBox): the remembered folder if it still exists and is inside this game's root, otherwise the root.
        // LastSelectedFolder is indexed by the TAB index ((int)game) - that is how the SelectedSaveFolder setter stores it.
        private static string ResolveRememberedDumpFolder(HaloGame game)
        {
            string root = Path.GetFullPath(Path.Combine("Saves", game.ToRootFolderPath()));
            StringCollection? remembered = Properties.Settings.Default.LastSelectedFolder;
            string? last = remembered != null && remembered.Count > (int)game ? remembered[(int)game] : null;

            // Ordinal, like the tree match (sf.SaveFolderPath == lastSelectedFolder): both sides are DirectoryInfo.FullName.
            if (!string.IsNullOrEmpty(last)
                && (last == root || last.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.Ordinal))
                && ExistsWithTreeCase(root, last))
            {
                return last;
            }
            return root;
        }

        // ⚠ NOT Directory.Exists, which ignores case. The tab's restore only matches a node whose path is built from the
        // ENUMERATED directory names, compared ordinally - so after a case-only rename ("speedrun" -> "Speedrun") the tab
        // opens on the root. Accepting the old casing here would dump into a folder the tab does not open on.
        private static bool ExistsWithTreeCase(string root, string path)
        {
            try
            {
                if (!Directory.Exists(root))
                {
                    return false;
                }
                if (path == root)
                {
                    return true;
                }
                string current = root;
                foreach (string segment in path.Substring(root.Length + 1).Split(Path.DirectorySeparatorChar))
                {
                    string? match = Directory.EnumerateDirectories(current)
                        .FirstOrDefault(d => string.Equals(Path.GetFileName(d), segment, StringComparison.Ordinal));
                    if (match == null)
                    {
                        return false;
                    }
                    current = match;
                }
                return true;
            }
            catch (Exception ex)
            {
                Log.Debug("ExistsWithTreeCase: " + ex.Message);
                return false;
            }
        }

        // How HCMInternal names the folder in its "Dumped checkpoint" message: the path below Saves\, e.g.
        // "Halo 3\Speedrun". It names the GAME as well as the folder, which matters now that a dump can land in a tab
        // that is not on screen.
        private static string DumpFolderLabel(string folderPath)
        {
            try
            {
                string relative = Path.GetRelativePath(Path.GetFullPath("Saves"), folderPath);
                if (!Path.IsPathRooted(relative) && relative != ".."
                    && !relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal))
                {
                    return relative;
                }
            }
            catch (Exception ex)
            {
                Log.Debug("DumpFolderLabel: " + ex.Message);
            }
            return Path.GetFileName(folderPath.TrimEnd(Path.DirectorySeparatorChar));
        }


        public void TreeFolderChanged(object sender, RoutedPropertyChangedEventArgs<object> e)
        {
            Trace.WriteLine("TreeFolderChanged. Sender: " + sender + ", currentgame?: " + SelectedGame);
            SaveFolder? saveFolder = (SaveFolder?)e.NewValue;

            if (saveFolder == null)
            {
                return;
            }

            Trace.WriteLine("Selected Folder Path: " + saveFolder.SaveFolderPath);

            if (Directory.Exists(saveFolder.SaveFolderPath))
                SelectedSaveFolder = saveFolder;
            else
                MessageBox.Show("Error with new selected save folder at path: " + saveFolder.SaveFolderPath);



        }


        public class SortCheckpointsByLastWriteTime : IComparer
        {
            public int Compare(object x, object y)
            {
                Checkpoint cx = (Checkpoint)x;
                Checkpoint cy = (Checkpoint)y;

                if (cx.ModifiedOn == null || cy.ModifiedOn == null)
                { return 0; }

                int? diff = (int?)(cx.ModifiedOn - cy.ModifiedOn)?.TotalSeconds;
                return diff == null ? 0 : (int)diff;
            }
        }



    }




}
