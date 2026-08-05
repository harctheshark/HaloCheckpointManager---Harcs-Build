
using Serilog;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows.Data;

namespace HCMExternal.Converters
{
    public class LevelNameConverter : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        {
            string? levelCode = value as string;
            if (levelCode == null)
            {
                return "???";
            }


            // So I'd like this to check what the current tab index is directly, but haven't figured out how.
            // ConverterParameter won't seem to work since this is from a control under the tab control.
            // For now we'll used the saved setting which should always be accurate.
            HaloGame gameEnum = (HaloGame)HCMExternal.Properties.Settings.Default.LastSelectedGameTab;

            // Halo Campaign Evolved has no LevelInfo table (and no fixed set of level codes to build one from), so
            // the scenario leaf DecodeCheckpointFile read out of the blob is already the best name available.
            // Without this the LevelInfo lookup below would throw and log an error for every row on the tab.
            if (gameEnum == HaloGame.HaloCER)
            {
                return levelCode;
            }

            try
            {
                LevelInfo levelInfo = new LevelInfo(gameEnum, levelCode);
                return levelInfo.FullName;
            }
            catch(Exception ex)
            {
                Log.Error("Level converter error: " + ex.Message + "\n" + ex.Source + "n" + "for levelcode: " + levelCode);
                return levelCode;
            }

        }

        public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        {
            throw new NotImplementedException();
        }
    }
}
