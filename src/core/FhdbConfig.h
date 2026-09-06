#ifndef FHDBCONFIG_H
#define FHDBCONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace Ps2
{

struct FhdbMenuItem
{
    int index = 0;
    std::string name;
    std::string path1;
    std::string path2;
    std::string path3;
};

class FhdbConfig
{
public:
    FhdbConfig();

    static FhdbConfig Parse(const std::string &text);
    static FhdbConfig CreateDefault();

    std::string Serialize() const;
    std::string Value(const std::string &key) const;
    void SetValue(const std::string &key, const std::string &value);
    void RemoveValue(const std::string &key);

    std::vector<FhdbMenuItem> MenuItems() const;
    void SetMenuItems(const std::vector<FhdbMenuItem> &items);

private:
    struct Line
    {
        bool assignment = false;
        std::string raw;
        std::string key;
        std::string value;
    };

    static std::string trim(const std::string &value);
    static bool parseMenuKey(const std::string &key, int &index, int &field);
    static Line parseLine(const std::string &text);
    static Line assignment(const std::string &key, const std::string &value);

    std::vector<Line> lines;
    std::string newline = "\n";
    bool finalNewline = true;
};

} // namespace Ps2

#endif // FHDBCONFIG_H
