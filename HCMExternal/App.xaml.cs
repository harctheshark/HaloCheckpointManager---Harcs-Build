using HCMExternal.Services.CheckpointIO.Impl;
using HCMExternal.Services.External.Impl;
using HCMExternal.Services.Interproc.Impl;
using HCMExternal.Services.PointerData.Impl;
using HCMExternal.Services.CheckpointIO;
using HCMExternal.Services.External;
using HCMExternal.Services.Interproc;
using HCMExternal.Services.PointerData;
using HCMExternal.Services.Hotkeys;
using HCMExternal.Services.Hotkeys.Impl;
using HCMExternal.ViewModels;
using HCMExternal.Views;

using Serilog;
using System;
using System.Diagnostics;
using System.IO;
using System.Windows;
using HCMExternal.Services.Hotkeys;

namespace HCMExternal
{

    public partial class App : Application
    {


        public string CurrentHCMVersion = "unset";

        // Held so Application_Exit can stop the interproc state machine before this process dies; see there.
        private IInterprocService? mInterproc = null;

        public App()
        {
            // Logging
            string timestamp = DateTime.Now.ToString("yyyyMMdd_HHmmss");

            Log.Logger = new LoggerConfiguration()
#if HCM_DEBUG
                .MinimumLevel.Verbose().WriteTo.Debug()
#endif
                .MinimumLevel.Verbose().WriteTo.File($"Logs\\HCMExternal_Logging_{timestamp}.txt",
                flushToDiskInterval: TimeSpan.FromSeconds(3))
                .CreateLogger();
            Log.Information("Logging started");

        }



        /// <summary>
        /// Swaps the colour theme (Themes/Dark.xaml or Themes/Light.xaml, always MergedDictionaries[0]) at runtime.
        /// All control styles live in Themes/Controls.xaml and reference theme colours with DynamicResource, so every
        /// open window re-themes immediately. Does not persist the choice; callers save Settings.DarkMode themselves.
        /// </summary>
        // Windows 10 20H1+ / 11: the window's own title bar in the dark style (DWMWA_USE_IMMERSIVE_DARK_MODE = 20; 19 on older builds)
        [System.Runtime.InteropServices.DllImport("dwmapi.dll")]
        private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);
        private static bool s_darkTheme = true;
        private static bool s_titleBarHookInstalled = false;
        public static void ApplyTitleBar(Window w)
        {
            try
            {
                var h = new System.Windows.Interop.WindowInteropHelper(w).Handle;
                if (h == IntPtr.Zero) return;
                int v = s_darkTheme ? 1 : 0;
                if (DwmSetWindowAttribute(h, 20, ref v, sizeof(int)) != 0) DwmSetWindowAttribute(h, 19, ref v, sizeof(int));
            }
            catch (Exception ex) { Log.Warning(ex, "Could not set the title bar colour"); }
        }

        public static void ApplyTheme(bool dark)
        {
            var app = Application.Current;
            if (app == null) return;
            s_darkTheme = dark;
            if (!s_titleBarHookInstalled)
            {
                // every window (main, dialogs) gets the title bar of the theme as it appears
                s_titleBarHookInstalled = true;
                EventManager.RegisterClassHandler(typeof(Window), FrameworkElement.LoadedEvent, new RoutedEventHandler((s, e) => { if (s is Window w) ApplyTitleBar(w); }));
            }
            foreach (Window w in app.Windows) ApplyTitleBar(w);
            var uri = new Uri($"/Themes/{(dark ? "Dark" : "Light")}.xaml", UriKind.Relative);
            var dicts = app.Resources.MergedDictionaries;
            if (dicts.Count > 0 && dicts[0].Source == uri) return;
            var themeDict = new ResourceDictionary { Source = uri };
            if (dicts.Count > 0) dicts[0] = themeDict; else dicts.Add(themeDict);
        }

        private void Application_Startup(object sender, StartupEventArgs e)
        {
            // Theme first, so nothing (including early error dialogs) is ever shown in the wrong colours.
            try { ApplyTheme(HCMExternal.Properties.Settings.Default.DarkMode); }
            catch (Exception ex) { Log.Error(ex, "Failed to apply theme"); }

            AppDomain.CurrentDomain.UnhandledException += (sender, args) =>
                    ShowErrorAndShutdown("An unhandled exception occured!\n" + (args.ExceptionObject as Exception).Message + "\n\nStackTrace:\n" + (args.ExceptionObject as Exception).StackTrace);

            System.Reflection.Assembly assembly = System.Reflection.Assembly.GetExecutingAssembly();
            System.Diagnostics.FileVersionInfo fvi = System.Diagnostics.FileVersionInfo.GetVersionInfo(assembly.Location);
            CurrentHCMVersion = fvi.FileVersion ?? "no version info";

            try
            {
                CheckRequiredFoldersAndFiles();
            }
            catch (Exception ex)
            {
                ShowErrorAndShutdown(ex.Data.Contains("UserMessage") ? ex.Data["UserMessage"].ToString() : ex.ToString());
                return;
            }


            // Set up services and view models

            // Tell DataPointersService to load data
            IPointerDataService pointerData;
            try
            {
                (pointerData, string pointerErrors) = PointerDataServiceFactory.MakePointerDataService();
                if (pointerErrors != "")
                { 
                    ShowError("Some pointers failed to load. Yell at Burnt for making typos.\n" + pointerErrors); 
                }
            }
            catch (Exception ex)
            {
                ShowErrorAndShutdown(ex.ToString() + "\n" + ex.Message + "\n" + ex.Source);
                return;
            }


            ICheckpointIOService checkpointIO = new CheckpointIOService(pointerData);
            IExternalService external = new ExternalService(pointerData);
            // Kept as a field, not just a local: Application_Exit has to reach it to stop the state machine
            // before this process dies. See Application_Exit.
            IInterprocService interproc = new InterprocService();
            mInterproc = interproc;
            IHotkeyManager hotkey = new HotkeyManager();


            MainWindow mainWindow = new();

            // main view model will construct lower view models too
            MainViewModel mainViewModel = new MainViewModel(checkpointIO, external, interproc, hotkey);
            mainWindow.DataContext = mainViewModel;



            mainWindow.Title = "HaloCheckpointManager " + (CurrentHCMVersion.Length > 4 ? CurrentHCMVersion.Substring(0, 5) : CurrentHCMVersion);
            mainWindow.Show();

            interproc.initializeSharedMemory(mainViewModel.FileViewModel.SelectedCheckpoint, mainViewModel.FileViewModel.SelectedSaveFolder, mainViewModel.FileViewModel.SelectedGame);
            // Every game's dump folder, not just the visible tab's - HCMInternal dumps the running game wherever the
            // user is looking. Must come after initializeSharedMemory: publishes before it are dropped.
            mainViewModel.FileViewModel.PublishDumpFoldersForAllGames();

        }


        private void Application_Exit(object sender, ExitEventArgs e)
        {
            // ⚠ THIS MUST HAPPEN BEFORE WE EXIT, and it used to be a TODO.
            //
            // The interproc state machine runs on its own thread with no exit condition. As this process dies,
            // HCMInternal's heartbeat notices and reports Shutdown; the still-running state machine reads that,
            // walks InternalSuccess -> MCCNotFound -> InternalInjecting, and INJECTS A FRESH HCMInternal into
            // the game. That orphan has no external to talk to, fails, and pins HCMInternal.dll in the game -
            // so the next time HCM is opened, injection either refuses or silently re-uses the dead copy and
            // hangs on "initialising". The only cure was fully restarting the game.
            //
            // On MCC this was masked (its internal only dies when the external does). On Halo Campaign Evolved
            // it fires constantly, because the game destroys and recreates its window mid-session.
            try
            {
                mInterproc?.shutdownInterprocEx();
            }
            catch (Exception ex)
            {
                Log.Warning("interproc shutdown threw during exit: " + ex.Message);
            }

            Log.CloseAndFlush();
        }


        private void ShowErrorAndShutdown(string error)
        {
            ShowError(error);
            System.Windows.Application.Current.Shutdown();
        }

        private void ShowError(string error)
        {
            Log.Error(error);
            System.Windows.MessageBox.Show(error, "HaloCheckpointManager Error", System.Windows.MessageBoxButton.OK);
        }



        #region RequiredFoldersAndFiles
        //Required folders that HCM needs to be able to run. We can try to create these if they don't exist already.
        private static readonly string[] _requiredFolders =
            {
            @"Logs",
            @"Saves",
            @"Saves\Halo 1",
            @"Saves\Halo 2",
            @"Saves\Halo 3",
            @"Saves\Halo 3 ODST",
            @"Saves\Halo Reach",
            @"Saves\Halo 4",
            @"Saves\Project Cartographer",
            // Halo Campaign Evolved. Must match HaloGameMethods.ToRootFolderPath(HaloGame.HaloCER) - the tab's
            // folder tree is built from it, and PopulateSaveFolderTree MessageBoxes at the user if it is missing.
            @"Saves\Halo Campaign Evolved",
        };

        //Required files that HCM needs to be able to run.
        private static readonly string[] _requiredAssemblies =
        {
                       @"HCMInternal.dll",
                       @"HCMInterproc.dll"
        };


        #endregion // RequiredFoldersAndFiles

        private void CheckRequiredFoldersAndFiles()
        {
            #region Have File Access?
            //Check if HCM has file access to it's local directory. We test this by making a temporary subdirectory, checking it exists, then deleting it.

            try
            {
                string localFolder = Directory.GetCurrentDirectory();
                string testFolder = localFolder + $@"\testDir";

                DirectoryInfo obj = Directory.CreateDirectory(testFolder);

                if (Directory.Exists(testFolder))
                {
                    Directory.Delete(testFolder, false);
                }
                else
                {
                    throw new Exception("Test directory didn't exist: " + testFolder);
                }

            }
            catch (Exception ex)
            {
                ex.Data.Add("UserMessage",
                     "HCM needs file read/write permissions in it's local directory." +
                    "\nDouble check the security permissions of the directory HCM is stored inside."
                    + "\n\nError: " + ex.ToString()
                    );
                throw;
            }

            #endregion

            #region Have Required Folders?
            //Check if we have the required files and folders. Create them with default values if we don't, only returns false if it still failed to create them.


            foreach (string folder in _requiredFolders)
            {
                string folderPath = Directory.GetCurrentDirectory() + $@"\{folder}";
                try
                {
                    Directory.CreateDirectory(folderPath);
                }
                catch (Exception ex)
                {
                    ex.Data.Add("UserMessage",
                    "HCM tried to create it's required working directories but failed." +
                    "\nDouble check the security permissions of the directory HCM is stored inside."
                    + "\n\nError: " + ex.ToString()
                    );
                    throw;
                }
            }


            #endregion

            #region Have Required Files?
            //Check if we have the required assemblies, like BurntMemory.dll, etc. Get's a list of missing files if any are missing.

            string missingAssemblies = "";
            foreach (string assembly in _requiredAssemblies)
            {
                string filePath = Path.Combine(Directory.GetCurrentDirectory(), assembly);
                if (!System.IO.File.Exists(filePath))
                {
                    missingAssemblies = missingAssemblies + (missingAssemblies == "" ? "" : ", ") + assembly;
                }
                else
                {
                    // file exists: check that it has the current version information
                    FileVersionInfo assemblyVersion = FileVersionInfo.GetVersionInfo(filePath);
                    string versionString = string.Format("{0}.{1}.{2}.{3}", assemblyVersion.FileMajorPart,
                                                                        assemblyVersion.FileMinorPart,
                                                                          assemblyVersion.FileBuildPart,
                                                                          assemblyVersion.FilePrivatePart);
                    if (versionString != CurrentHCMVersion)
                    {
                        missingAssemblies = missingAssemblies + (missingAssemblies == "" ? "" : ", ") + assembly + "(wrong file version! was " + versionString + ")";
                    }
                }


            }

            if (missingAssemblies != "")
            {
                throw new Exception("HCM is missing required files from it's local directory!" +
                    "\nYou may have to re-download HCM to get the required files."
                    + "\n\nMissing files: " + missingAssemblies);
            }



            #endregion

        }


    }
}
