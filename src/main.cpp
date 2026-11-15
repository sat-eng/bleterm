// bleterm: an interactive BLE terminal.
// Scan with /list, /connect to a device, then read, write and subscribe to its
// characteristics. Lines that don't start with '/' are written to the selected
// characteristic, so a Nordic UART (NUS) device behaves like a serial port.

#include <simpleble/SimpleBLE.h>

#include "bt_status.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#ifdef HAVE_EDITLINE
#include <editline/readline.h>
#endif

namespace {

const std::string NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const std::string NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";  // we write here
const std::string NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";  // device notifies here
const std::string USER_DESCRIPTION = "00002901-0000-1000-8000-00805f9b34fb";

const std::map<std::string, std::string> KNOWN_SERVICES = {
    {"00001800-0000-1000-8000-00805f9b34fb", "Generic Access"},
    {"00001801-0000-1000-8000-00805f9b34fb", "Generic Attribute"},
    {"0000180a-0000-1000-8000-00805f9b34fb", "Device Information"},
    {"0000180f-0000-1000-8000-00805f9b34fb", "Battery"},
    {NUS_SERVICE, "Nordic UART"},
};

const char* HELP = R"(Commands:
  /list [seconds]          scan for devices (default 5 s)
  /connect <#|address|name> connect to a device from the last scan
  /disconnect              drop the connection
  /services                list characteristics (numbered) with their descriptions
  /use <#>                 select the characteristic plain text lines are written to
  /read [#]                read a characteristic (default: selected)
  /write <#> <text>        write text to a characteristic
  /notify <#> [stream]     print notifications/indications from a characteristic;
                           "stream" joins them into continuous text like a serial port
  /unnotify <#>            stop them
  /hex [on|off]            show data as hex, and parse typed data as hex bytes
  /eol none|lf|cr|crlf     line ending added to typed text (default none, lf for Nordic UART)
  /help                    this text
  /quit                    exit
Anything not starting with '/' is written to the selected characteristic.
Tab completes commands, /hex and /eol options, and device names/addresses for /connect.)";

// For tab completion; keep in step with HELP and Terminal::handle().
const std::vector<std::string> COMMANDS = {
    "/list", "/connect", "/disconnect", "/services", "/use", "/read", "/write",
    "/notify", "/unnotify", "/hex", "/eol", "/help", "/quit",
};

std::mutex outMutex;

// True when stdin is a terminal, so line editing and history are available.
bool interactive = false;

// True while readline() is showing the prompt (guarded by outMutex). Other threads may only
// clear and redraw the prompt then: redrawing at any other time leaves a stale "> " line,
// because readline draws a fresh prompt when it starts.
bool inReadline = false;

void say(const std::string& text) {
    std::lock_guard<std::mutex> lock(outMutex);
    std::cout << text << std::endl;
}

// For messages from BLE callback threads: print above the prompt and re-show it
// (with whatever the user had typed so far, when line editing is active).
void sayAsync(const std::string& text) {
    std::lock_guard<std::mutex> lock(outMutex);
#ifdef HAVE_EDITLINE
    if (interactive) {
        if (inReadline) {
            std::cout << "\r\033[K" << text << std::endl;
            rl_forced_update_display();
        } else {
            std::cout << text << std::endl;
        }
        return;
    }
#endif
    std::cout << "\n" << text << "\n> " << std::flush;
}

// In-memory only: history is never written to disk, since it can hold Wi-Fi passwords.
void remember(const std::string& line) {
#ifdef HAVE_EDITLINE
    static std::string last;
    if (interactive && line != last) {
        add_history(line.c_str());
        last = line;
    }
#else
    (void)line;
#endif
}

// Shows the prompt again (with any half-typed line, when line editing is active).
// Call with outMutex held.
void redrawPrompt() {
#ifdef HAVE_EDITLINE
    if (interactive) {
        rl_forced_update_display();
        return;
    }
#endif
    std::cout << "> " << std::flush;
}

// Output for UART-style notifications. Devices split their text into packets of ~20 bytes,
// so chunks are printed back to back as one stream, and the prompt is redrawn once the
// device has been quiet for a moment.
class StreamOutput {
  public:
    StreamOutput() { worker_ = std::thread([this] { run(); }); }

    ~StreamOutput() {
        {
            std::lock_guard<std::mutex> lock(outMutex);
            stop_ = true;
        }
        worker_.join();
    }

    void write(const std::string& data) {
        std::lock_guard<std::mutex> lock(outMutex);
        if (!active_) {
            // Replace the prompt line with the incoming text (no escape codes when piped).
            if (!interactive) std::cout << "\n";
            else if (inReadline) std::cout << "\r\033[K";
            active_ = true;
            atLineStart_ = true;
            currentLine_.clear();
        }
        for (char c : data) {
            if (c == '\r') {  // CRLF becomes LF; a lone CR becomes LF too
                pendingCr_ = true;
                continue;
            }
            if (pendingCr_ && c != '\n') put('\n');
            pendingCr_ = false;
            put(c);
        }
        std::cout << std::flush;
        lastOutput_ = std::chrono::steady_clock::now();
    }

    // Waits (up to a limit) for incoming text to finish, so a new prompt isn't drawn
    // in the middle of a device's reply.
    void waitUntilQuiet() {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(outMutex);
                if (!active_) return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

  private:
    static constexpr auto QUIET_TIME = std::chrono::milliseconds(150);

    bool stop_ = false;
    bool active_ = false;  // stream text has replaced the prompt
    bool atLineStart_ = true;
    bool pendingCr_ = false;
    std::string currentLine_;  // text printed since the last newline
    std::chrono::steady_clock::time_point lastOutput_;
    std::thread worker_;

    void put(char c) {
        std::cout << c;
        atLineStart_ = c == '\n';
        if (atLineStart_) currentLine_.clear();
        else currentLine_ += c;
    }

    // A short unfinished line ending in a typical prompt character, e.g. "> " or "pico$ ".
    static bool looksLikePrompt(const std::string& line) {
        size_t end = line.find_last_not_of(' ');
        if (end == std::string::npos || line.size() > 20) return false;
        return std::strchr(">$#:", line[end]) != nullptr;
    }

    void run() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard<std::mutex> lock(outMutex);
            if (stop_) return;
            if (!active_ || std::chrono::steady_clock::now() - lastOutput_ < QUIET_TIME) continue;
            if (pendingCr_) {
                std::cout << '\n';
            } else if (!atLineStart_) {
                // Serial consoles often finish with their own prompt; replace it with
                // bleterm's rather than leaving it on a line of its own above ours.
                if (interactive && looksLikePrompt(currentLine_)) std::cout << "\r\033[K";
                else std::cout << '\n';
            }
            pendingCr_ = false;
            atLineStart_ = true;
            currentLine_.clear();
            active_ = false;
            // Outside readline the next readline() call draws the prompt itself.
            if (!interactive || inReadline) redrawPrompt();
            else std::cout << std::flush;
        }
    }
};

// Reads one line, with up/down history when interactive. Returns false at end of input.
bool readLine(std::string& line, StreamOutput& stream) {
#ifdef HAVE_EDITLINE
    if (interactive) {
        stream.waitUntilQuiet();
        {
            std::lock_guard<std::mutex> lock(outMutex);
            inReadline = true;
        }
        char* raw = readline("> ");
        {
            std::lock_guard<std::mutex> lock(outMutex);
            inReadline = false;
        }
        if (!raw) return false;
        line = raw;
        free(raw);
        return true;
    }
#else
    (void)stream;
#endif
    {
        std::lock_guard<std::mutex> lock(outMutex);
        std::cout << "> " << std::flush;
    }
    return static_cast<bool>(std::getline(std::cin, line));
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Splits "word rest of line" into ("word", "rest of line").
std::pair<std::string, std::string> splitFirst(const std::string& s) {
    size_t sp = s.find(' ');
    if (sp == std::string::npos) return {s, ""};
    size_t rest = s.find_first_not_of(' ', sp);
    return {s.substr(0, sp), rest == std::string::npos ? "" : s.substr(rest)};
}

std::optional<int> parseIndex(const std::string& s) {
    if (s.empty() || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) return std::nullopt;
    return std::stoi(s);
}

std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

// Truncates or pads to `width` characters, counting UTF-8 code points rather than bytes.
std::string fitUtf8(const std::string& s, size_t width) {
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < s.size() && chars < width; chars++) {
        size_t len = 1;
        unsigned char c = s[i];
        if (c >= 0xf0) len = 4;
        else if (c >= 0xe0) len = 3;
        else if (c >= 0xc0) len = 2;
        out += s.substr(i, len);
        i += len;
    }
    return out + std::string(width - chars, ' ');
}

struct CharRef {
    SimpleBLE::BluetoothUUID service;
    SimpleBLE::BluetoothUUID uuid;
    std::string description;
    bool canRead, canWriteRequest, canWriteCommand, canNotify, canIndicate;
};

class Terminal {
  public:
    Terminal(SimpleBLE::Adapter adapter, StreamOutput& stream) : adapter_(std::move(adapter)), stream_(stream) {}

    ~Terminal() {
        if (connected()) peer_->disconnect();
    }

    // Returns false when the user asks to quit.
    bool handle(const std::string& line) {
        if (line[0] != '/') {
            // In serial mode the device's own reply shows the line arrived.
            sendText(selected_, line, uartMode_);
            return true;
        }
        auto [cmd, args] = splitFirst(line);
        cmd = lower(cmd);
        if (cmd == "/quit" || cmd == "/exit") return false;
        if (cmd == "/help" || cmd == "/?") say(HELP);
        else if (cmd == "/list" || cmd == "/scan") list(args);
        else if (cmd == "/connect") connect(args);
        else if (cmd == "/disconnect") disconnect();
        else if (cmd == "/services" || cmd == "/chars") printChars();
        else if (cmd == "/use") use(args);
        else if (cmd == "/read") read(args);
        else if (cmd == "/write") write(args);
        else if (cmd == "/notify") subscribe(args, true);
        else if (cmd == "/unnotify") subscribe(args, false);
        else if (cmd == "/hex") setHex(args);
        else if (cmd == "/eol") setEol(args);
        else say("Unknown command " + cmd + " (try /help)");
        return true;
    }

    // Tab-completion candidates for the argument of `cmd` (lower case, e.g. "/hex").
    std::vector<std::string> argumentCompletions(const std::string& cmd) {
        if (cmd == "/hex") return {"on", "off"};
        if (cmd == "/eol") return {"none", "lf", "cr", "crlf"};
        if (cmd == "/connect") {
            std::vector<std::string> out;
            for (auto& p : scan_) {
                // Names with spaces can't be completed as one word; the address still can.
                std::string name = p.identifier();
                if (!name.empty() && name.find(' ') == std::string::npos) out.push_back(name);
                out.push_back(p.address());
            }
            return out;
        }
        return {};
    }

  private:
    SimpleBLE::Adapter adapter_;
    std::vector<SimpleBLE::Peripheral> scan_;
    std::optional<SimpleBLE::Peripheral> peer_;
    std::vector<CharRef> chars_;
    int selected_ = -1;  // index into chars_
    std::atomic<bool> hex_{false};
    std::string eol_;
    bool eolChosen_ = false;  // the user picked a line ending, so don't change it automatically
    bool uartMode_ = false;   // connected to a Nordic UART device
    StreamOutput& stream_;

    bool connected() { return peer_ && peer_->is_connected(); }

    bool requireConnected() {
        if (connected()) return true;
        say("Not connected (use /list then /connect).");
        return false;
    }

    std::string render(const SimpleBLE::ByteArray& data) const {
        std::string text = data;
        bool printable = std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return (c >= 0x20 && c < 0x7f) || c == '\r' || c == '\n' || c == '\t';
        });
        if (hex_ || !printable) return data.size() ? data.toHex(true) : "(empty)";
        return text;
    }

    // Bluetooth can report "not ready" briefly after launch, so give it a moment before
    // explaining what's actually wrong.
    bool bluetoothReady() {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!adapter_.bluetooth_enabled() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::string problem = bluetoothProblem(adapter_);
        if (!problem.empty()) say(problem);
        return problem.empty();
    }

    void list(const std::string& args) {
        if (!bluetoothReady()) return;
        int secs = parseIndex(args).value_or(5);
        say(format("Scanning for %d s...", secs));
        adapter_.scan_for(secs * 1000);
        scan_ = adapter_.scan_get_results();
        // 127 is CoreBluetooth's "RSSI unknown"; sort those last.
        auto strength = [](SimpleBLE::Peripheral& p) { return p.rssi() == 127 ? -1000 : p.rssi(); };
        std::sort(scan_.begin(), scan_.end(), [&](auto& a, auto& b) { return strength(a) > strength(b); });
        if (scan_.empty()) {
            say("No devices found.");
            return;
        }
        say(format("  #  %-24s %-38s %5s", "Name", "Address", "RSSI"));
        for (size_t i = 0; i < scan_.size(); i++) {
            auto& p = scan_[i];
            std::string name = p.identifier().empty() ? "(no name)" : p.identifier();
            std::string rssi = p.rssi() == 127 ? "?" : std::to_string(p.rssi());
            say(format("%3zu  %s %-38s %5s%s", i + 1, fitUtf8(name, 24).c_str(), p.address().c_str(), rssi.c_str(),
                       p.is_connectable() ? "" : "  (not connectable)"));
        }
    }

    std::optional<SimpleBLE::Peripheral> findPeripheral(const std::string& target) {
        if (auto idx = parseIndex(target)) {
            if (*idx >= 1 && *idx <= (int)scan_.size()) return scan_[*idx - 1];
            return std::nullopt;
        }
        std::string t = lower(target);
        for (auto& p : scan_) {
            if (lower(p.address()) == t) return p;
        }
        for (auto& p : scan_) {
            if (lower(p.identifier()).find(t) != std::string::npos) return p;
        }
        return std::nullopt;
    }

    void connect(const std::string& target) {
        if (target.empty()) {
            say("Usage: /connect <#|address|name>");
            return;
        }
        if (scan_.empty()) list("");
        auto p = findPeripheral(target);
        if (!p) {
            say("No device matching '" + target + "' in the last scan.");
            return;
        }
        if (connected()) disconnect();

        std::string name = p->identifier().empty() ? p->address() : p->identifier();
        say("Connecting to " + name + "...");
        p->set_callback_on_disconnected([name]() { sayAsync("[disconnected from " + name + "]"); });
        p->connect();
        peer_ = p;
        say(format("Connected (MTU %u).", peer_->mtu()));

        loadChars();
        autoSelectUart();
        printChars();
    }

    void disconnect() {
        if (!peer_) {
            say("Not connected.");
            return;
        }
        if (peer_->is_connected()) peer_->disconnect();
        peer_.reset();
        chars_.clear();
        selected_ = -1;
        uartMode_ = false;
    }

    void loadChars() {
        chars_.clear();
        selected_ = -1;
        uartMode_ = false;
        for (auto& svc : peer_->services()) {
            for (auto& c : svc.characteristics()) {
                CharRef ref{svc.uuid(), c.uuid(), "", c.can_read(), c.can_write_request(), c.can_write_command(),
                            c.can_notify(), c.can_indicate()};
                for (auto& d : c.descriptors()) {
                    if (lower(d.uuid()) != USER_DESCRIPTION) continue;
                    try {
                        ref.description = peer_->read(svc.uuid(), c.uuid(), d.uuid());
                    } catch (const std::exception&) {
                        // Description is optional; ignore devices that refuse the read.
                    }
                }
                chars_.push_back(ref);
            }
        }
    }

    void autoSelectUart() {
        for (size_t i = 0; i < chars_.size(); i++) {
            if (chars_[i].service != NUS_SERVICE) continue;
            if (chars_[i].uuid == NUS_RX) selected_ = (int)i;
            if (chars_[i].uuid == NUS_TX) startNotify((int)i, true);
        }
        if (selected_ < 0) return;
        uartMode_ = true;
        // Serial consoles generally act on a line only once they receive a newline.
        if (!eolChosen_) eol_ = "\n";
        say(std::string("Nordic UART service found: typed lines go to the device, replies are printed. ") +
            "Line ending: " + (eol_.empty() ? "none" : eol_ == "\n" ? "lf" : eol_ == "\r" ? "cr" : "crlf") +
            " (change with /eol).");
    }

    void printChars() {
        if (!requireConnected()) return;
        std::string lastService;
        for (size_t i = 0; i < chars_.size(); i++) {
            auto& c = chars_[i];
            if (c.service != lastService) {
                lastService = c.service;
                auto known = KNOWN_SERVICES.find(c.service);
                say("Service " + c.service + (known != KNOWN_SERVICES.end() ? "  (" + known->second + ")" : ""));
            }
            std::string props;
            props += c.canRead ? 'R' : '-';
            props += (c.canWriteRequest || c.canWriteCommand) ? 'W' : '-';
            props += c.canNotify ? 'N' : (c.canIndicate ? 'I' : '-');
            say(format("%s[%2zu] %s  %s  %s", (int)i == selected_ ? " *" : "  ", i + 1, c.uuid.c_str(),
                       props.c_str(), c.description.c_str()));
        }
        if (chars_.empty()) say("No characteristics.");
    }

    // Resolves a 1-based characteristic number (or the selection when empty). Returns -1 on error.
    int charIndex(const std::string& arg) {
        if (arg.empty()) {
            if (selected_ < 0) say("No characteristic selected (use /use <#>).");
            return selected_;
        }
        auto idx = parseIndex(arg);
        if (!idx || *idx < 1 || *idx > (int)chars_.size()) {
            say("No characteristic " + arg + " (see /services).");
            return -1;
        }
        return *idx - 1;
    }

    void use(const std::string& args) {
        if (!requireConnected()) return;
        int i = charIndex(args);
        if (i < 0) return;
        selected_ = i;
        say(format("Typed lines now go to [%d] %s", i + 1,
                   chars_[i].description.empty() ? chars_[i].uuid.c_str() : chars_[i].description.c_str()));
    }

    void read(const std::string& args) {
        if (!requireConnected()) return;
        int i = charIndex(args);
        if (i < 0) return;
        auto& c = chars_[i];
        if (!c.canRead) {
            say(format("[%d] is not readable.", i + 1));
            return;
        }
        say(format("< [%d] ", i + 1) + render(peer_->read(c.service, c.uuid)));
    }

    void write(const std::string& args) {
        auto [idx, text] = splitFirst(args);
        if (idx.empty()) {
            say("Usage: /write <#> <text>");
            return;
        }
        if (!requireConnected()) return;
        int i = charIndex(idx);
        if (i >= 0) sendText(i, text);
    }

    void sendText(int i, const std::string& text, bool quiet = false) {
        if (!requireConnected()) return;
        if (i < 0) {
            say("No characteristic selected (use /use <#>, or /write <#> <text>).");
            return;
        }
        auto& c = chars_[i];
        SimpleBLE::ByteArray data;
        if (hex_) {
            std::string digits;
            for (char ch : text) {
                if (!std::isspace((unsigned char)ch)) digits += ch;
            }
            if (digits.size() % 2 || !std::all_of(digits.begin(), digits.end(), [](unsigned char ch) { return std::isxdigit(ch); })) {
                say("Hex mode: type pairs of hex digits, e.g. 48 65 6c 6c 6f");
                return;
            }
            data = SimpleBLE::ByteArray::fromHex(digits);
        } else {
            data = SimpleBLE::ByteArray(text + eol_);
        }
        if (c.canWriteRequest) {
            peer_->write_request(c.service, c.uuid, data);
        } else if (c.canWriteCommand) {
            peer_->write_command(c.service, c.uuid, data);
        } else {
            say(format("[%d] is not writable.", i + 1));
            return;
        }
        if (!quiet) say(format("> [%d] %zu bytes", i + 1, data.size()));
    }

    void subscribe(const std::string& args, bool on) {
        if (!requireConnected()) return;
        auto [idx, mode] = splitFirst(args);
        int i = charIndex(idx);
        if (i < 0) return;
        if (!on) {
            if (!chars_[i].canNotify && !chars_[i].canIndicate) {
                say(format("[%d] does not support notifications.", i + 1));
                return;
            }
            peer_->unsubscribe(chars_[i].service, chars_[i].uuid);
            say(format("Stopped notifications from [%d].", i + 1));
            return;
        }
        if (!mode.empty() && lower(mode) != "stream") {
            say("Usage: /notify <#> [stream]");
            return;
        }
        startNotify(i, !mode.empty());
    }

    // stream: print payloads back to back as continuous text (serial-port style) instead of
    // one labelled line per notification.
    void startNotify(int i, bool stream) {
        auto& c = chars_[i];
        if (!c.canNotify && !c.canIndicate) {
            say(format("[%d] does not support notifications.", i + 1));
            return;
        }
        std::function<void(SimpleBLE::ByteArray)> callback;
        if (stream) {
            callback = [this](SimpleBLE::ByteArray payload) {
                stream_.write(hex_ ? payload.toHex(true) + " " : std::string(payload));
            };
        } else {
            callback = [this, i](SimpleBLE::ByteArray payload) {
                sayAsync(format("< [%d] ", i + 1) + render(payload));
            };
        }
        if (c.canNotify) peer_->notify(c.service, c.uuid, callback);
        else peer_->indicate(c.service, c.uuid, callback);
        say(format("Listening to [%d]%s.", i + 1, stream ? " as a text stream" : ""));
    }

    void setHex(const std::string& args) {
        std::string a = lower(args);
        if (a == "on") hex_ = true;
        else if (a == "off") hex_ = false;
        else if (a.empty()) hex_ = !hex_;
        else {
            say("Usage: /hex [on|off]");
            return;
        }
        say(std::string("Hex mode ") + (hex_ ? "on" : "off"));
    }

    void setEol(const std::string& args) {
        std::string a = lower(args);
        if (a == "none") eol_ = "";
        else if (a == "lf") eol_ = "\n";
        else if (a == "cr") eol_ = "\r";
        else if (a == "crlf") eol_ = "\r\n";
        else {
            say("Usage: /eol none|lf|cr|crlf");
            return;
        }
        eolChosen_ = true;
        say("Line ending: " + a);
    }
};

#ifdef HAVE_EDITLINE
Terminal* completingTerminal = nullptr;
std::vector<std::string> completionCandidates;

// libedit calls this repeatedly (state 0, 1, 2...) until it returns null.
char* completionGenerator(const char* text, int state) {
    static size_t next;
    if (state == 0) next = 0;
    size_t len = strlen(text);
    while (next < completionCandidates.size()) {
        const std::string& c = completionCandidates[next++];
        if (lower(c).compare(0, len, lower(text)) == 0) return strdup(c.c_str());
    }
    return nullptr;
}

char** completeLine(const char* text, int start, int) {
    rl_attempted_completion_over = 1;  // never fall back to file names
    std::string before(rl_line_buffer, start);
    auto [cmd, rest] = splitFirst(trim(before));
    if (cmd.empty()) completionCandidates = COMMANDS;
    else if (rest.empty()) completionCandidates = completingTerminal->argumentCompletions(lower(cmd));
    else completionCandidates.clear();  // only the first argument is completed
    return rl_completion_matches(text, completionGenerator);
}
#endif

}  // namespace

int main() {
    interactive = isatty(STDIN_FILENO);
    say("bleterm - type /help for commands");
    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        say("No Bluetooth adapter found.");
        return 1;
    }

    // Declared before the terminal so it outlives it: notification callbacks write to it
    // until the terminal disconnects.
    StreamOutput stream;
    Terminal terminal(adapters[0], stream);
#ifdef HAVE_EDITLINE
    completingTerminal = &terminal;
    rl_attempted_completion_function = completeLine;
#endif
    std::string line;
    while (readLine(line, stream)) {
        line = trim(line);
        if (line.empty()) continue;
        remember(line);
        try {
            if (!terminal.handle(line)) break;
        } catch (const std::exception& e) {
            say(std::string("Error: ") + e.what());
        }
    }
    return 0;
}
