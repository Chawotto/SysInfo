#include <ncurses.h>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <sys/statvfs.h>
#include <unistd.h>
#include <cstdio>
#include <filesystem>
#include <chrono>
#include <thread>
#include <sys/wait.h>
#include <signal.h>
#include <memory>

// Утилитная функция для выполнения команд
std::string exec_command(const char* cmd, int timeout_seconds = 5) {
    std::string result;
    int pipefd[2];
    pipe(pipefd);
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char*)nullptr);
        _exit(127);
    }
    close(pipefd[1]);
    char buffer[128];
    std::vector<char> output;
    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        if (elapsed >= timeout_seconds) {
            kill(pid, SIGKILL);
            break;
        }
        int status; // Объявляем переменную status здесь
        if (waitpid(pid, &status, WNOHANG) == pid) {
            break;
        }
        ssize_t count = read(pipefd[0], buffer, sizeof(buffer) - 1);
        if (count <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        buffer[count] = '\0';
        output.insert(output.end(), buffer, buffer + count);
    }
    close(pipefd[0]);
    int status; // Объявляем переменную status здесь для второго waitpid
    waitpid(pid, &status, 0);
    result.assign(output.begin(), output.end());
    if (result.empty()) return "Error: command timed out or failed";
    return result;
}

// Утилитная функция для проверки наличия утилиты
bool is_utility_installed(const std::string& util) {
    return std::filesystem::exists("/usr/sbin/" + util) || std::filesystem::exists("/usr/bin/" + util);
}

// Базовый класс для провайдеров информации
class InfoProvider {
public:
    virtual std::string getInfo() = 0;
    virtual ~InfoProvider() {}
};

// Провайдер для CPU
class CPUInfoProvider : public InfoProvider {
public:
    std::string getInfo() override {
        std::string output = exec_command("lscpu 2>/dev/null");
        if (output.find("Error") != std::string::npos) {
            return "Error: lscpu utility not found\nPlease install util-linux package";
        }
        std::istringstream iss(output);
        std::string line, result;
        while (std::getline(iss, line)) {
            if (line.find("Model name:") != std::string::npos ||
                line.find("CPU(s):") != std::string::npos ||
                line.find("Thread(s) per core:") != std::string::npos ||
                line.find("Core(s) per socket:") != std::string::npos ||
                line.find("Socket(s):") != std::string::npos ||
                line.find("CPU MHz:") != std::string::npos) {
                result += line + "\n";
            }
        }
        return result.empty() ? "No CPU data found" : result;
    }
};

// Провайдер для System Usage
class SystemUsageProvider : public InfoProvider {
public:
    std::string getInfo() override {
        std::string result;
        std::ifstream stat_file("/proc/stat");
        if (!stat_file.is_open()) return "Error: cannot open /proc/stat";
        std::string line;
        std::getline(stat_file, line);
        std::istringstream iss(line);
        std::string cpu;
        long user, nice, system, idle, iowait, irq, softirq;
        iss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq;
        long total = user + nice + system + idle + iowait + irq + softirq;
        long idle_total = idle + iowait;
        static long prev_total = 0, prev_idle = 0;
        long delta_total = total - prev_total;
        long delta_idle = idle_total - prev_idle;
        double cpu_usage = (delta_total - delta_idle) * 100.0 / delta_total;
        prev_total = total;
        prev_idle = idle_total;
        result += "CPU Usage: " + std::to_string(cpu_usage).substr(0, 5) + "%\n";
        result += "-------------------\n";

        std::ifstream meminfo("/proc/meminfo");
        if (!meminfo.is_open()) return "Error: cannot open /proc/meminfo";
        std::string mem_line;
        long mem_total = 0, mem_free = 0, mem_available = 0;
        while (std::getline(meminfo, mem_line)) {
            if (mem_line.find("MemTotal:") != std::string::npos) {
                sscanf(mem_line.c_str(), "MemTotal: %ld kB", &mem_total);
            } else if (mem_line.find("MemFree:") != std::string::npos) {
                sscanf(mem_line.c_str(), "MemFree: %ld kB", &mem_free);
            } else if (mem_line.find("MemAvailable:") != std::string::npos) {
                sscanf(mem_line.c_str(), "MemAvailable: %ld kB", &mem_available);
            }
        }
        double mem_usage = (mem_total - mem_available) * 100.0 / mem_total;
        result += "Memory Usage: " + std::to_string(mem_usage).substr(0, 5) + "%\n";
        result += "-------------------\n";

        struct statvfs stat;
        if (statvfs("/", &stat) == 0) {
            double disk_total = stat.f_blocks * stat.f_frsize;
            double disk_free = stat.f_bfree * stat.f_frsize;
            double disk_usage = (disk_total - disk_free) * 100.0 / disk_total;
            result += "Disk Usage (/): " + std::to_string(disk_usage).substr(0, 5) + "%\n";
        } else {
            result += "Disk Usage: Error\n";
        }
        result += "-------------------\n";

        std::ifstream netdev("/proc/net/dev");
        if (!netdev.is_open()) return "Error: cannot open /proc/net/dev";
        std::string net_line;
        while (std::getline(netdev, net_line)) {
            if (net_line.find("enp") != std::string::npos || net_line.find("wlp") != std::string::npos) {
                std::istringstream net_iss(net_line);
                std::string iface, rx, tx;
                net_iss >> iface >> rx;
                for (int i = 0; i < 7; ++i) net_iss >> tx;
                result += "Network (" + iface.substr(0, iface.size() - 1) + "):\nRX: " + rx + " bytes\nTX: " + tx + " bytes\n";
                result += "-------------------\n";
            }
        }
        return result;
    }
};

// Провайдер для Temperatures
class TemperaturesProvider : public InfoProvider {
public:
    std::string getInfo() override {
        std::string output = exec_command("sensors 2>/dev/null");
        if (output.find("Error") != std::string::npos) {
            return "Error: sensors utility not found\nPlease install lm_sensors and run 'sudo sensors-detect'";
        }
        std::istringstream iss(output);
        std::string line, result;
        while (std::getline(iss, line)) {
            result += line + "\n";
            if (line.empty()) {
                result += "-------------------\n";
            }
        }
        return result.empty() ? "No temperature data found\nRun 'sudo sensors-detect' to configure sensors" : result;
    }
};

// Провайдер для Motherboard
class MotherboardProvider : public InfoProvider {
public:
    std::string getInfo() override {
        if (getuid() != 0) {
            return "Warning: dmidecode requires root privileges\nRun with sudo for full info";
        }
        std::string output = exec_command("dmidecode -t baseboard 2>/dev/null");
        if (output.find("Error") != std::string::npos) {
            return "Error: dmidecode utility not found\nPlease install dmidecode package\nTry: sudo dnf install dmidecode";
        }
        return output.empty() ? "No motherboard data found" : output;
    }
};

// Провайдер для Memory
class MemoryProvider : public InfoProvider {
public:
    std::string getInfo() override {
        if (getuid() != 0) {
            return "Warning: dmidecode requires root privileges\nRun with sudo for full info";
        }
        if (!is_utility_installed("dmidecode")) {
            return "Error: dmidecode utility not found\nPlease install dmidecode package";
        }
        std::string output = exec_command("dmidecode -t memory 2>/dev/null");
        std::istringstream iss(output);
        std::string line, result;
        bool in_memory_device = false;
        while (std::getline(iss, line)) {
            if (line.find("Memory Device") != std::string::npos) {
                if (in_memory_device) {
                    result += "-------------------\n";
                }
                in_memory_device = true;
                continue;
            }
            if (in_memory_device && (line.find("Size:") != std::string::npos ||
                                     line.find("Type:") != std::string::npos ||
                                     line.find("Speed:") != std::string::npos ||
                                     line.find("Manufacturer:") != std::string::npos ||
                                     line.find("Part Number:") != std::string::npos)) {
                result += line + "\n";
            }
        }
        return result.empty() ? "No memory data found" : result;
    }
};

// Провайдер для Disks
class DisksProvider : public InfoProvider {
public:
    std::string getInfo() override {
        std::string output = exec_command("lsblk -d -o NAME,SIZE,MODEL 2>/dev/null");
        if (output.find("Error") != std::string::npos) {
            return "Error: lsblk utility not found\nPlease install util-linux package";
        }
        std::istringstream iss(output);
        std::string line, result;
        bool first = true;
        while (std::getline(iss, line)) {
            if (line.find("NAME") != std::string::npos) continue;
            if (!first) {
                result += "-------------------\n";
            }
            result += line + "\n";
            first = false;
        }
        return result.empty() ? "No disk data found" : result;
    }
};

// Провайдер для Network
class NetworkProvider : public InfoProvider {
public:
    std::string getInfo() override {
        std::string result;
        std::string ip_output = exec_command("ip -br link 2>/dev/null");
        if (ip_output.find("Error") != std::string::npos) {
            return "Error: ip utility not found\nPlease install iproute package";
        }
        std::istringstream iss(ip_output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("enp") != std::string::npos || line.find("wlp") != std::string::npos) {
                std::istringstream line_iss(line);
                std::string iface, state;
                line_iss >> iface >> state;
                result += "Adapter: " + iface + "\nState: " + state + "\n";
                result += "-------------------\n";
            }
        }
        if (is_utility_installed("bluetoothctl")) {
            std::string bt_output = exec_command("bluetoothctl show 2>/dev/null");
            if (bt_output.find("Controller") != std::string::npos) {
                result += "Bluetooth: Enabled\n";
            } else {
                result += "Bluetooth: Disabled or not found\n";
            }
        } else {
            result += "Bluetooth: bluetoothctl not found\n";
        }
        return result;
    }
};

// Провайдер для GPU
class GPUProvider : public InfoProvider {
public:
    std::string getInfo() override {
        std::string result;
        std::string lspci_output = exec_command("lspci | grep -E 'VGA|3D' 2>/dev/null");
        if (lspci_output.find("Error") != std::string::npos) {
            return "Error: lspci utility not found\nPlease install pciutils package";
        }
        std::istringstream lspci_iss(lspci_output);
        std::string lspci_line;
        bool first_gpu = true;
        while (std::getline(lspci_iss, lspci_line)) {
            if (!first_gpu) {
                result += "-------------------\n";
            }
            result += "GPU Model: " + lspci_line + "\n";
            first_gpu = false;
        }

        if (is_utility_installed("nvidia-smi")) {
            std::string nvidia_output = exec_command("nvidia-smi --query-gpu=name,driver_version,memory.total,memory.used,utilization.gpu,temperature.gpu --format=csv,noheader 2>/dev/null", 3);
            if (!nvidia_output.empty() && nvidia_output.find("Error") == std::string::npos) {
                std::istringstream nvidia_iss(nvidia_output);
                std::string nvidia_line;
                while (std::getline(nvidia_iss, nvidia_line)) {
                    std::istringstream line_iss(nvidia_line);
                    std::string name, driver, mem_total, mem_used, util, temp;
                    std::getline(line_iss, name, ',');
                    std::getline(line_iss, driver, ',');
                    std::getline(line_iss, mem_total, ',');
                    std::getline(line_iss, mem_used, ',');
                    std::getline(line_iss, util, ',');
                    std::getline(line_iss, temp, ',');
                    result += "-------------------\n";
                    result += "NVIDIA GPU Details:\n";
                    result += "Name: " + name + "\n";
                    result += "Driver Version: " + driver + "\n";
                    result += "Memory Total: " + mem_total + "\n";
                    result += "Memory Used: " + mem_used + "\n";
                    result += "GPU Utilization: " + util + "\n";
                    result += "Temperature: " + temp + "\n";
                }
            } else {
                result += "-------------------\n";
                result += "NVIDIA GPU: Data unavailable\n";
            }
        }

        if (is_utility_installed("radeontop")) {
            std::string amd_output = exec_command("LC_ALL=en_US.UTF-8 radeontop -d -l 1 2>/dev/null", 3);
            if (!amd_output.empty() && amd_output.find("Error") == std::string::npos) {
                std::istringstream amd_iss(amd_output);
                std::string amd_line;
                result += "-------------------\n";
                result += "AMD GPU Details:\n";
                while (std::getline(amd_iss, amd_line)) {
                    if (amd_line.find("Dumping to") != std::string::npos) continue;
                    if (amd_line.find("Unknown Radeon card") != std::string::npos) {
                        result += "Warning: " + amd_line + "\n";
                        continue;
                    }
                    std::istringstream line_iss(amd_line);
                    std::string token;
                    while (line_iss >> token) {
                        if (token == "bus") {
                            line_iss >> token;
                            result += "Bus: " + token + "\n";
                        } else if (token == "gpu") {
                            line_iss >> token;
                            result += "GPU Usage: " + token + "\n";
                        } else if (token == "vram") {
                            line_iss >> token;
                            result += "VRAM Usage: " + token + "\n";
                        } else if (token == "mclk") {
                            line_iss >> token;
                            result += "Memory Clock: " + token + " (percentage of max)\n";
                        } else if (token == "sclk") {
                            line_iss >> token;
                            result += "Shader Clock: " + token + "\n";
                        }
                    }
                }
            } else {
                result += "-------------------\n";
                result += "AMD GPU: Data unavailable\n";
            }
        }

        return result.empty() ? "No GPU data found" : result;
    }
};

// Класс для управления интерфейсом и обновлением данных
class SystemMonitor {
public:
    SystemMonitor() {
        providers.push_back(std::make_unique<CPUInfoProvider>());
        providers.push_back(std::make_unique<SystemUsageProvider>());
        providers.push_back(std::make_unique<TemperaturesProvider>());
        providers.push_back(std::make_unique<MotherboardProvider>());
        providers.push_back(std::make_unique<MemoryProvider>());
        providers.push_back(std::make_unique<DisksProvider>());
        providers.push_back(std::make_unique<NetworkProvider>());
        providers.push_back(std::make_unique<GPUProvider>());
        menu_items = {"CPU", "System Usage", "Temperatures", "Motherboard", "Memory", "Disks", "Network", "GPU"};
    }

    void run() {
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        start_color();
        init_pair(1, COLOR_CYAN, COLOR_BLACK);
        init_pair(2, COLOR_GREEN, COLOR_BLACK);
        init_pair(3, COLOR_YELLOW, COLOR_BLACK);
        timeout(3000); // Устанавливаем таймаут 3 секунды

        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);

        WINDOW* menu_win = newwin(9, 20, 1, 1);
        WINDOW* info_win = newwin(max_y - 11, max_x - 22, 1, 22);
        WINDOW* status_win = newwin(1, max_x, max_y - 1, 0);

        int highlight = 0;
        int scroll_offset = 0;
        std::string current_info = providers[highlight]->getInfo(); // Инициализация при старте

        auto last_update = std::chrono::steady_clock::now();

        while (true) {
            if (is_term_resized(max_y, max_x)) {
                resize_windows(menu_win, info_win, status_win, max_y, max_x);
            }

            display_menu(menu_win, highlight, menu_items);

            wattron(status_win, COLOR_PAIR(3));
            mvwprintw(status_win, 0, 0, "Use arrows to navigate, Page Up/Down to scroll, q to quit, r to refresh");
            wattroff(status_win, COLOR_PAIR(3));
            wrefresh(status_win);

            wattron(info_win, COLOR_PAIR(3));
            mvwprintw(info_win, 0, 1, "%s Info", menu_items[highlight].c_str());
            wattroff(info_win, COLOR_PAIR(3));
            wattron(info_win, COLOR_PAIR(2));

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_update).count() >= 3) {
                current_info = providers[highlight]->getInfo();
                last_update = now;
            }

            display_info(info_win, current_info, scroll_offset, max_y - 11, max_x - 22);
            wattroff(info_win, COLOR_PAIR(2));

            int ch = getch();
            if (ch == ERR) {
                continue;
            }
            switch (ch) {
                case KEY_UP:
                    highlight = (highlight == 0) ? menu_items.size() - 1 : highlight - 1;
                    scroll_offset = 0;
                    current_info = providers[highlight]->getInfo(); // Обновление при переключении
                    break;
                case KEY_DOWN:
                    highlight = (highlight == menu_items.size() - 1) ? 0 : highlight + 1;
                    scroll_offset = 0;
                    current_info = providers[highlight]->getInfo(); // Обновление при переключении
                    break;
                case KEY_PPAGE:
                    scroll_offset = (scroll_offset > 0) ? scroll_offset - 1 : 0;
                    break;
                case KEY_NPAGE:
                    scroll_offset++;
                    break;
                case 'q':
                    goto cleanup;
                case 'r':
                    current_info = providers[highlight]->getInfo(); // Принудительное обновление
                    break;
            }
        }

    cleanup:
        delwin(menu_win);
        delwin(info_win);
        delwin(status_win);
        endwin();
    }

private:
    std::vector<std::unique_ptr<InfoProvider>> providers;
    std::vector<std::string> menu_items;

    void resize_windows(WINDOW*& menu_win, WINDOW*& info_win, WINDOW*& status_win, int& max_y, int& max_x) {
        getmaxyx(stdscr, max_y, max_x);
        wresize(menu_win, std::min(9, max_y - 2), 20);
        wresize(info_win, max_y - 11, max_x - 22);
        wresize(status_win, 1, max_x);
        mvwin(menu_win, 1, 1);
        mvwin(info_win, 1, 22);
        mvwin(status_win, max_y - 1, 0);
    }

    void display_menu(WINDOW* menu_win, int highlight, const std::vector<std::string>& items) {
        box(menu_win, 0, 0);
        for (int i = 0; i < items.size(); ++i) {
            if (i == highlight) wattron(menu_win, A_REVERSE | COLOR_PAIR(1));
            mvwprintw(menu_win, i + 1, 2, "%s", items[i].c_str());
            wattroff(menu_win, A_REVERSE | COLOR_PAIR(1));
        }
        wrefresh(menu_win);
    }

    void display_info(WINDOW* info_win, const std::string& info, int scroll_offset, int max_lines, int max_cols) {
        werase(info_win);
        box(info_win, 0, 0);
        std::istringstream iss(info);
        std::string line;
        std::vector<std::string> lines;
        while (std::getline(iss, line)) {
            while (line.length() > max_cols - 4) {
                lines.push_back(line.substr(0, max_cols - 4));
                line = "  " + line.substr(max_cols - 4);
            }
            lines.push_back(line);
        }
        int y = 1;
        for (int i = scroll_offset; i < lines.size() && y < max_lines - 2; ++i) {
            mvwprintw(info_win, y++, 1, "%s", lines[i].c_str());
        }
        wrefresh(info_win);
    }
};

int main() {
    SystemMonitor monitor;
    monitor.run();
    return 0;
}
