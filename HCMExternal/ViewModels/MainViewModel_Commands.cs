using GongSolutions.Wpf.DragDrop;
using HCMExternal.Models;
using HCMExternal.Services.CheckpointIO;
using HCMExternal.ViewModels.Commands;
using HCMExternal.ViewModels.Interfaces;
using HCMExternal.Views;
using Serilog;
using System;
using System.Collections;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows;
using System.Windows.Data;
using System.Windows.Input;


namespace HCMExternal.ViewModels
{

    public partial class MainViewModel : Presenter
    {
       

        private void onDeleteCheckpoint()
        {
            _checkpointIOService.DeleteCheckpoint(FileViewModel.SelectedSaveFolder, FileViewModel.SelectedCheckpoint);
            App.Current.Dispatcher.Invoke((Action)delegate
            {
                FileViewModel.UpdateCheckpointCollection();
            });
        }


        private void onRenameCheckpoint()
        {
            _checkpointIOService.RenameCheckpoint(FileViewModel.SelectedSaveFolder, FileViewModel.SelectedCheckpoint);
        }


        private void onReVersionCheckpoint()
        {
            // ⚠⚠ REFUSED ON HALO CAMPAIGN EVOLVED, AND THIS IS NOT COSMETIC.
            // ReVersionCheckpoint stamps a 10-byte MCC version string over the LAST TEN BYTES OF THE FILE. For the
            // MCC games those bytes are HCM's own metadata (InjectCheckpoint zeroes them again on the way in). A
            // HaloCER dump is the engine's blob VERBATIM - there is no metadata tail, so those ten bytes are ten
            // bytes of GAME STATE. Worse, they would not be caught: HCMInternal always recomputes the SHA-1 before
            // injecting, so the corrupted file would sail through the checksum and go into a game where a bad
            // revert does not fail softly, it RESTARTS THE LEVEL. HaloCER also has no version string to set (every
            // build reports 0.0.0.0), so there is nothing this could legitimately do.
            if (FileViewModel.SelectedGame.Equals(HaloGame.HaloCER))
            {
                System.Windows.MessageBox.Show(
                    "Halo Campaign Evolved checkpoints don't carry a version string, and writing one would corrupt the checkpoint.",
                    "HaloCheckpointManager", System.Windows.MessageBoxButton.OK, System.Windows.MessageBoxImage.Information);
                return;
            }

            _checkpointIOService.ReVersionCheckpoint(FileViewModel.SelectedSaveFolder, FileViewModel.SelectedCheckpoint);
        }


        private void onSortCheckpoint()
        {
            Application.Current.Dispatcher.Invoke(delegate
            {
                SortCheckpointsView win = new(_checkpointIOService, FileViewModel)
                {
                    Owner = App.Current.MainWindow // makes the dialog be centered on the main window
                };
                win.ShowDialog();
            });
        }


        private void onOpenInExplorer()
        {
            _checkpointIOService.OpenInExplorer(FileViewModel.SelectedSaveFolder);
        }


        private void onRenameFolder()
        {
            _checkpointIOService.RenameFolder(FileViewModel.SelectedSaveFolder);
            App.Current.Dispatcher.Invoke((Action)delegate
            {
                FileViewModel.UpdateSaveFolderCollection();
            });
        }


        private void onDeleteFolder()
        {
            _checkpointIOService.DeleteFolder(FileViewModel.SelectedSaveFolder);
            App.Current.Dispatcher.Invoke((Action)delegate
            {
                FileViewModel.UpdateSaveFolderCollection();
                FileViewModel.UpdateCheckpointCollection();
            });

        }


        private void onNewFolder()
        {
            _checkpointIOService.NewFolder(FileViewModel.SelectedSaveFolder);
            App.Current.Dispatcher.Invoke((Action)delegate
            {
                FileViewModel.UpdateSaveFolderCollection();
            });
        }


        private void onForceCheckpoint()
        {
            _externalService.ForceCheckpoint();
        }


        private void onForceRevert()
        {
            _externalService.ForceRevert();
        }


        private void onForceDoubleRevert()
        {
            _externalService.ForceDoubleRevert();
        }


        private void onDumpCheckpoint()
        {
            _externalService.DumpCheckpoint(FileViewModel.SelectedSaveFolder);

            App.Current.Dispatcher.Invoke((Action)delegate
            {
                FileViewModel.UpdateCheckpointCollection();
            });
            
        }


        private void onInjectCheckpoint()
        {

            if (FileViewModel.SelectedCheckpoint == null)
                System.Windows.MessageBox.Show("Failed to Inject! \n" + "No checkpoint selected!", "HaloCheckpointManager Error", System.Windows.MessageBoxButton.OK);
            else
            {
                // Cartographer is the one game HCMExternal injects itself (it maps the game's own file handles).
                // Everything else - the six MCC games AND Halo Campaign Evolved - goes through shared memory: the
                // selection was already published by FileViewModel.SelectedCheckpoint's setter, and this just raises
                // the flag HCMInternal's HeartbeatTimer polls. HaloCER lands here deliberately; HCMInternal routes
                // the resulting injectCheckpointEvent to HCEInjectCheckpoint instead of InjectCheckpoint.
                if (FileViewModel.SelectedGame.Equals(HaloGame.ProjectCartographer))
                    _externalService.InjectCheckpoint(FileViewModel.SelectedCheckpoint);
                else
                    _interprocService.UpdateSharedMemQueueInjectCommand();
            }
                
        }

        private void onDisableCheckpoints(bool shouldDisableCheckpoint)
        {
            _externalService.DisableCheckpoints(shouldDisableCheckpoint);
        }



    }




}
