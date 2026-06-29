// Linux file dialog via zenity
#include "file_dialog.hpp"

#include <cstdio>
#include <cstring>

namespace imv {

bool OpenVideoFiles(std::vector<std::string>& out_paths) {
    FILE* fp = popen(
        "zenity --file-selection --title='Open Video File' "
        "--file-filter='Video files | *.mp4 *.mkv *.avi *.mov *.webm *.flv' "
        "2>/dev/null",
        "r");
    if (!fp) return false;

    char buf[65536] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    int exit_code = pclose(fp);
    if (exit_code != 0 || n == 0) return false;
    buf[n] = '\0';

    if (buf[0]) out_paths.push_back(buf);
    return !out_paths.empty();
}

} // namespace imv
