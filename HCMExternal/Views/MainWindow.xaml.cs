using System.Windows;

namespace HCMExternal.Views
{
    /// <summary>
    /// Interaction logic for MainWindow.xaml
    /// </summary>
    public partial class MainWindow : Window
    {
        public MainWindow()
        {
            InitializeComponent();
            try { DarkModeCheckBox.IsChecked = HCMExternal.Properties.Settings.Default.DarkMode; }
            catch { DarkModeCheckBox.IsChecked = true; }
            // the hook status sits over the right end of the game tabs: keep that much of the tab strip free, so tabs wrap
            // before running under it (its width changes with the state's text)
            GameTabs.Loaded += (s, e) => ReserveStatusSpace();
            HookStatus.SizeChanged += (s, e) => ReserveStatusSpace();
        }

        private void ReserveStatusSpace()
        {
            GameTabs.ApplyTemplate();
            if (GameTabs.Template?.FindName("HeaderPanel", GameTabs) is FrameworkElement header)
                header.Margin = new Thickness(2, 2, HookStatus.ActualWidth + 6, 0);
        }

        // Click (not Checked/Unchecked) so setting IsChecked from the constructor never triggers a save.
        private void DarkModeCheckBox_Click(object sender, RoutedEventArgs e)
        {
            bool dark = DarkModeCheckBox.IsChecked == true;
            App.ApplyTheme(dark);
            try
            {
                HCMExternal.Properties.Settings.Default.DarkMode = dark;
                HCMExternal.Properties.Settings.Default.Save();
            }
            catch (System.Exception ex)
            {
                Serilog.Log.Warning(ex, "Could not save DarkMode setting");
            }
        }










    }







}
