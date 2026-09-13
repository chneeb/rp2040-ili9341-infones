#ifndef FRENSHELPERS
#define FRENSHELPERS
#include <string.h>
#include <string>
#include <algorithm>
namespace Frens
{
    bool endsWith(std::string const &str, std::string const &suffix);
    std::string str_tolower(std::string s);

    bool cstr_endswith(const char *string, const char *width);

    /* Park core1 (the audio consumer) around a flash erase/program: XIP is
     * unusable while flash is being written, so a core1 that executes from
     * flash faults. No-ops when core1 is not running. Defined in main.cpp. */
    void flash_lockout_start();
    void flash_lockout_end();
} // namespace Frens


#endif