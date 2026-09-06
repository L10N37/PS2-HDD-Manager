#include "OplConfig.h"

#include <sstream>

namespace Ps2
{

std::string OplConfig::BuildInternalHddPreset(const OplPresetOptions &options)
{
    // Values below intentionally mirror current OPL enum/config values:
    // START_MODE_AUTO = 2 and HDD_MODE = 6.
    std::ostringstream out;
    out << "# PS2 HDD Manager - internal HDD plug-and-play preset\n";
    out << "# Generated settings can still be changed normally inside OPL.\n";
    out << "hdd_mode=2\n";
    out << "default_device=6\n";
    out << "app_mode=" << (options.enableApps ? 2 : 0) << "\n";
    if (options.disableUsbAtStartup)
        out << "usb_mode=0\n";
    if (options.disableNetworkAtStartup)
        out << "eth_mode=0\n";
    out << "enable_coverart=" << (options.enableCoverArt ? 1 : 0) << "\n";
    out << "enable_delete_rename=" << (options.enableWriteOperations ? 1 : 0) << "\n";
    out << "hdd_game_list_cache=" << (options.hddGameListCache ? 1 : 0) << "\n";
    out << "autorefresh=" << (options.autoRefresh ? 1 : 0) << "\n";
    out << "autosort=" << (options.autoSort ? 1 : 0) << "\n";
    out << "remember_last=0\n";
    out << "autostart_last=0\n";
    if (!options.exitPath.empty())
        out << "exit_path=" << options.exitPath << "\n";
    return out.str();
}

std::string OplConfig::SetKey(const std::string &config, const std::string &key,
        const std::string &value)
{
    if (key.empty() || key.find('=') != std::string::npos || key.find('\n') != std::string::npos ||
            key.find('\r') != std::string::npos || value.find('\n') != std::string::npos ||
            value.find('\r') != std::string::npos)
        return config;

    const std::string prefix = key + "=";
    std::istringstream input(config);
    std::ostringstream output;
    std::string line;
    bool written = false;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(prefix, 0) == 0)
        {
            if (!written)
            {
                output << prefix << value << "\n";
                written = true;
            }
        }
        else
            output << line << "\n";
    }
    if (!written)
        output << prefix << value << "\n";
    return output.str();
}

} // namespace Ps2
