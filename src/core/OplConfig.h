#ifndef OPLCONFIG_H
#define OPLCONFIG_H

#include <string>

namespace Ps2
{

struct OplPresetOptions
{
    bool enableCoverArt = true;
    bool enableApps = true;
    bool enableWriteOperations = true;
    bool hddGameListCache = true;
    bool autoRefresh = true;
    bool autoSort = true;
    bool disableUsbAtStartup = true;
    bool disableNetworkAtStartup = true;
    std::string exitPath;
};

class OplConfig
{
public:
    // Builds a deliberately small conf_opl.cfg containing only settings that
    // PS2 HDD Manager owns. OPL will supply defaults for every omitted key.
    static std::string BuildInternalHddPreset(const OplPresetOptions &options);

    // Replaces all existing occurrences of key with one canonical key=value
    // line, or appends it if absent. Every unrelated line is preserved.
    static std::string SetKey(const std::string &config, const std::string &key,
            const std::string &value);
};

} // namespace Ps2

#endif // OPLCONFIG_H
