#include <crt_externs.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <poll.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;
using CpuTime = std::chrono::nanoseconds;

constexpr int kInvalidFd{-1};
constexpr int kUsageExitCode{2};
constexpr int kErrorExitCode{1};
constexpr int kSignalExitBase{128};
constexpr std::size_t kDefaultClients{8};
constexpr Seconds kDefaultSettle{3.0};
constexpr Seconds kDefaultMeasure{10.0};
constexpr auto kReadyTimeout{60s};
constexpr auto kFramesTimeout{2s};
constexpr auto kGrace{3s};
constexpr auto kPollStep{50ms};
constexpr int kPollStepMs{50};
constexpr std::size_t kReadChunk{4096};
constexpr double kBytesPerMegabyte{1024.0 * 1024.0};
constexpr double kPercent{100.0};
constexpr std::size_t kRevealsLineTokens{4};
constexpr std::size_t kGpuLineTokens{2};
constexpr std::size_t kVerifyLineTokens{3};
constexpr double kMicrosecondsPerMillisecond{1000.0};

constexpr std::string_view kUsage{
    "usage: bench --scene PATH [--clients N] [--settle SECONDS] [--measure SECONDS] [--output FILE]\n"
    "             [--release never|unmapped|occluded] [--redraw overdraw|full|damage] [--verify-redraw]\n"
    "             [--effects none|alpha,corners,shadows,blur,animation]\n"};

volatile std::sig_atomic_t g_stop_signal{0};

void on_stop_signal(int sig) { g_stop_signal = sig; }

struct Pid {
    pid_t value{};
};

struct Bytes {
    std::uint64_t value{};
};

struct MachTicks {
    std::uint64_t value{};
};

struct FrameCount {
    std::uint64_t value{};
};

struct RevealCount {
    std::uint64_t value{};
};

struct Microseconds {
    std::uint64_t value{};
};

struct RevealTotals {
    RevealCount count;
    Microseconds total;
    Microseconds max;
};

struct VerifyTotals {
    FrameCount checked;
    FrameCount mismatched;
};

struct Telemetry {
    FrameCount frames;
    RevealTotals reveals;
    Microseconds gpu;
    VerifyTotals verify;
};

class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Interrupted : public std::exception {
public:
    explicit Interrupted(int sig) : signal_{sig} {}
    [[nodiscard]] int signal() const { return signal_; }
    [[nodiscard]] const char* what() const noexcept override { return "interrupted"; }

private:
    int signal_{};
};

[[noreturn]] void throw_errno(std::string_view what) {
    throw std::system_error{errno, std::generic_category(), std::string{what}};
}

void check_rc(int rc, std::string_view what) {
    if (rc != 0) {
        throw std::system_error{rc, std::generic_category(), std::string{what}};
    }
}

void report(std::string_view message) noexcept {
    try {
        std::cerr << "bench: " << message << '\n';
    } catch (...) {
        // Nothing sensible remains when stderr itself fails during cleanup.
    }
}

void check_stop() {
    if (g_stop_signal != 0) {
        throw Interrupted{g_stop_signal};
    }
}

void install_signal_handlers() {
    struct sigaction action{};
    action.sa_handler = on_stop_signal;
    if (sigemptyset(&action.sa_mask) != 0) {
        throw_errno("sigemptyset");
    }
    for (const int sig : std::array{SIGINT, SIGTERM}) {
        if (sigaction(sig, &action, nullptr) != 0) {
            throw_errno("sigaction");
        }
    }
}

class Timebase {
public:
    static Timebase query() {
        mach_timebase_info_data_t info{};
        if (mach_timebase_info(&info) != KERN_SUCCESS || info.denom == 0) {
            throw std::runtime_error{"mach_timebase_info failed"};
        }
        return Timebase{info.numer, info.denom};
    }

    [[nodiscard]] CpuTime to_duration(MachTicks ticks) const {
        const std::uint64_t whole{ticks.value / denom_};
        const std::uint64_t rest{ticks.value % denom_};
        return CpuTime{static_cast<CpuTime::rep>(whole * numer_ + rest * numer_ / denom_)};
    }

private:
    Timebase(std::uint32_t numer, std::uint32_t denom) : numer_{numer}, denom_{denom} {}
    std::uint64_t numer_{1};
    std::uint64_t denom_{1};
};

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_{fd} {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_{std::exchange(other.fd_, kInvalidFd)} {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, kInvalidFd);
        }
        return *this;
    }
    ~UniqueFd() { reset(); }

    [[nodiscard]] int get() const { return fd_; }

    void reset() noexcept {
        if (fd_ != kInvalidFd) {
            if (::close(fd_) != 0) {
                report(std::format("close: {}", std::strerror(errno)));
            }
            fd_ = kInvalidFd;
        }
    }

private:
    int fd_{kInvalidFd};
};

struct Pipe {
    UniqueFd read;
    UniqueFd write;
};

Pipe make_pipe() {
    std::array<int, 2> fds{kInvalidFd, kInvalidFd};
    if (::pipe(fds.data()) != 0) {
        throw_errno("pipe");
    }
    return Pipe{UniqueFd{fds.at(0)}, UniqueFd{fds.at(1)}};
}

void set_nonblocking(const UniqueFd& fd) {
    const int flags{::fcntl(fd.get(), F_GETFL)};
    if (flags < 0 || ::fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK) != 0) {
        throw_errno("fcntl");
    }
}

class TempFile {
public:
    explicit TempFile(std::string_view stem) {
        path_ = (std::filesystem::temp_directory_path() / std::format("{}-XXXXXX", stem)).string();
        const int fd{::mkstemp(path_.data())};
        if (fd < 0) {
            path_.clear();
            throw_errno("mkstemp");
        }
        fd_ = UniqueFd{fd};
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;
    ~TempFile() {
        if (!path_.empty() && ::unlink(path_.c_str()) != 0) {
            report(std::format("unlink {}: {}", path_, std::strerror(errno)));
        }
    }

    [[nodiscard]] const UniqueFd& fd() const { return fd_; }

    [[nodiscard]] std::string read_all() const {
        std::string content;
        std::array<char, kReadChunk> chunk{};
        off_t offset{0};
        while (true) {
            const ssize_t n{::pread(fd_.get(), chunk.data(), chunk.size(), offset)};
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw_errno("pread");
            }
            if (n == 0) {
                return content;
            }
            content.append(chunk.data(), static_cast<std::size_t>(n));
            offset += n;
        }
    }

private:
    std::string path_;
    UniqueFd fd_;
};

class SpawnFileActions {
public:
    SpawnFileActions() { check_rc(posix_spawn_file_actions_init(&actions_), "posix_spawn_file_actions_init"); }
    SpawnFileActions(const SpawnFileActions&) = delete;
    SpawnFileActions& operator=(const SpawnFileActions&) = delete;
    SpawnFileActions(SpawnFileActions&&) = delete;
    SpawnFileActions& operator=(SpawnFileActions&&) = delete;
    ~SpawnFileActions() {
        if (const int rc{posix_spawn_file_actions_destroy(&actions_)}; rc != 0) {
            report(std::format("posix_spawn_file_actions_destroy: {}", std::strerror(rc)));
        }
    }

    void open(int fd, const char* path, int flags) {
        check_rc(posix_spawn_file_actions_addopen(&actions_, fd, path, flags, 0), "posix_spawn_file_actions_addopen");
    }
    void dup2(int from, int to) {
        check_rc(posix_spawn_file_actions_adddup2(&actions_, from, to), "posix_spawn_file_actions_adddup2");
    }
    void inherit(int fd) {
        check_rc(posix_spawn_file_actions_addinherit_np(&actions_, fd), "posix_spawn_file_actions_addinherit_np");
    }
    [[nodiscard]] const posix_spawn_file_actions_t* get() const { return &actions_; }

private:
    posix_spawn_file_actions_t actions_{};
};

class SpawnAttributes {
public:
    SpawnAttributes() {
        check_rc(posix_spawnattr_init(&attributes_), "posix_spawnattr_init");
        // Own process group: a terminal Ctrl-C reaches only bench, which then shuts children down in order.
        const auto flags{static_cast<short>(POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP |
                                            POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK)};
        check_rc(posix_spawnattr_setflags(&attributes_, flags), "posix_spawnattr_setflags");
        check_rc(posix_spawnattr_setpgroup(&attributes_, 0), "posix_spawnattr_setpgroup");
        sigset_t defaults{};
        sigset_t mask{};
        if (sigemptyset(&defaults) != 0 || sigemptyset(&mask) != 0) {
            throw_errno("sigemptyset");
        }
        for (const int sig : std::array{SIGINT, SIGTERM, SIGUSR1, SIGPIPE}) {
            if (sigaddset(&defaults, sig) != 0) {
                throw_errno("sigaddset");
            }
        }
        check_rc(posix_spawnattr_setsigdefault(&attributes_, &defaults), "posix_spawnattr_setsigdefault");
        check_rc(posix_spawnattr_setsigmask(&attributes_, &mask), "posix_spawnattr_setsigmask");
    }
    SpawnAttributes(const SpawnAttributes&) = delete;
    SpawnAttributes& operator=(const SpawnAttributes&) = delete;
    SpawnAttributes(SpawnAttributes&&) = delete;
    SpawnAttributes& operator=(SpawnAttributes&&) = delete;
    ~SpawnAttributes() {
        if (const int rc{posix_spawnattr_destroy(&attributes_)}; rc != 0) {
            report(std::format("posix_spawnattr_destroy: {}", std::strerror(rc)));
        }
    }

    [[nodiscard]] const posix_spawnattr_t* get() const { return &attributes_; }

private:
    posix_spawnattr_t attributes_{};
};

std::string describe_status(int status) {
    if (WIFEXITED(status)) {
        return std::format("exit code {}", WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        return std::format("killed by signal {} ({})", WTERMSIG(status), strsignal(WTERMSIG(status)));
    }
    return std::format("status {}", status);
}

class Child {
public:
    Child(std::string name, Pid pid, UniqueFd output) : name_{std::move(name)}, pid_{pid}, output_{std::move(output)} {}
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&&) = delete;
    Child& operator=(Child&&) = delete;
    ~Child() {
        if (!reaped()) {
            send_noexcept(SIGTERM);
            wait_until(Clock::now() + kGrace);
            kill_and_reap();
        }
    }

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] Pid pid() const { return pid_; }
    [[nodiscard]] UniqueFd& output() { return output_; }
    [[nodiscard]] bool reaped() const { return status_.has_value() || lost_; }

    [[nodiscard]] std::expected<bool, std::error_code> poll_exit() noexcept {
        if (reaped()) {
            return true;
        }
        int status{0};
        const pid_t rc{::waitpid(pid_.value, &status, WNOHANG)};
        if (rc == pid_.value) {
            status_ = status;
            return true;
        }
        if (rc == 0 || (rc < 0 && errno == EINTR)) {
            return false;
        }
        return std::unexpected{std::error_code{errno, std::generic_category()}};
    }

    [[nodiscard]] bool exited() {
        const auto result{poll_exit()};
        if (!result) {
            throw std::system_error{result.error(), std::format("waitpid {}", name_)};
        }
        return *result;
    }

    [[nodiscard]] std::string exit_description() const {
        return status_ ? describe_status(*status_) : std::string{"unknown status"};
    }

    void signal(int sig) {
        if (::kill(pid_.value, sig) != 0) {
            throw_errno(std::format("kill {}", name_));
        }
    }

    void send_noexcept(int sig) noexcept {
        if (!reaped() && ::kill(pid_.value, sig) != 0 && errno != ESRCH) {
            report(std::format("kill {}: {}", name_, std::strerror(errno)));
        }
    }

    bool wait_until(Clock::time_point deadline) noexcept {
        while (true) {
            const auto result{poll_exit()};
            if (!result) {
                report(std::format("waitpid {}: {}", name_, result.error().message()));
                lost_ = true;
                return true;
            }
            if (*result) {
                return true;
            }
            if (Clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(kPollStep / 2);
        }
    }

    void kill_and_reap() noexcept {
        if (reaped()) {
            return;
        }
        send_noexcept(SIGKILL);
        while (true) {
            int status{0};
            const pid_t rc{::waitpid(pid_.value, &status, 0)};
            if (rc == pid_.value) {
                status_ = status;
                return;
            }
            if (rc < 0 && errno == EINTR) {
                continue;
            }
            report(std::format("waitpid {}: {}", name_, std::strerror(errno)));
            lost_ = true;
            return;
        }
    }

private:
    std::string name_;
    Pid pid_;
    UniqueFd output_;
    std::optional<int> status_;
    bool lost_{false};
};

std::unique_ptr<Child> spawn(std::string name, const std::filesystem::path& executable,
                             const std::vector<std::string>& arguments, int stdout_fd, UniqueFd parent_end) {
    std::vector<std::string> storage{executable.string()};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& argument : storage) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    SpawnFileActions actions;
    actions.open(STDIN_FILENO, "/dev/null", O_RDONLY);
    actions.dup2(stdout_fd, STDOUT_FILENO);
    actions.inherit(STDERR_FILENO);
    const SpawnAttributes attributes;

    pid_t pid{0};
    check_rc(posix_spawn(&pid, storage.front().c_str(), actions.get(), attributes.get(), argv.data(), *_NSGetEnviron()),
             std::format("posix_spawn {}", executable.string()));
    return std::make_unique<Child>(std::move(name), Pid{pid}, std::move(parent_end));
}

struct Usage {
    Bytes footprint;
    CpuTime cpu;
};

Usage sample(const Child& child, const Timebase& timebase) {
    rusage_info_v4 info{};
    if (proc_pid_rusage(child.pid().value, RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&info)) != 0) {
        throw_errno(std::format("proc_pid_rusage {}", child.name()));
    }
    // ri_user_time and ri_system_time are Mach absolute time units, not nanoseconds, on Apple silicon.
    return Usage{Bytes{info.ri_phys_footprint},
                 timebase.to_duration(MachTicks{info.ri_user_time}) + timebase.to_duration(MachTicks{info.ri_system_time})};
}

std::vector<std::string_view> split_whitespace(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t position{0};
    while (position < line.size()) {
        const std::size_t start{line.find_first_not_of(" \t\r", position)};
        if (start == std::string_view::npos) {
            break;
        }
        const std::size_t end{std::min(line.find_first_of(" \t\r", start), line.size())};
        tokens.push_back(line.substr(start, end - start));
        position = end;
    }
    return tokens;
}

template <typename Number>
std::optional<Number> parse_number(std::string_view text) {
    Number value{};
    const char* const end{text.data() + text.size()};
    const auto [pointer, error]{std::from_chars(text.data(), end, value)};
    if (error != std::errc{} || pointer != end) {
        return std::nullopt;
    }
    return value;
}

struct Scene {
    std::filesystem::path path;
    long width{};
    long height{};
    std::string scale;
    std::size_t windows{};
    std::size_t mapped{};
};

Scene load_scene(const std::filesystem::path& path) {
    std::ifstream file{path};
    if (!file) {
        throw std::runtime_error{std::format("cannot open scene file {}", path.string())};
    }
    Scene scene{.path = path};
    bool have_display{false};
    std::string line;
    std::size_t number{0};
    while (std::getline(file, line)) {
        ++number;
        const auto tokens{split_whitespace(line)};
        if (tokens.empty() || tokens.front().starts_with('#')) {
            continue;
        }
        const auto fail{[&](std::string_view why) {
            return std::runtime_error{std::format("{}:{}: {}", path.string(), number, why)};
        }};
        if (!have_display) {
            if (tokens.size() != 4 || tokens.at(0) != "display") {
                throw fail("expected 'display <width> <height> <scale>'");
            }
            const auto width{parse_number<long>(tokens.at(1))};
            const auto height{parse_number<long>(tokens.at(2))};
            const auto scale{parse_number<double>(tokens.at(3))};
            if (!width || !height || !scale || *width <= 0 || *height <= 0 || *scale <= 0.0) {
                throw fail("invalid display size or scale");
            }
            scene.width = *width;
            scene.height = *height;
            scene.scale = std::string{tokens.at(3)};
            have_display = true;
            continue;
        }
        if (tokens.size() != 6) {
            throw fail("expected '<x> <y> <width> <height> <mapped> <layer>'");
        }
        for (const std::string_view token : tokens) {
            if (!parse_number<long>(token)) {
                throw fail(std::format("'{}' is not an integer", token));
            }
        }
        const auto mapped{parse_number<long>(tokens.at(4))};
        if (mapped != 0L && mapped != 1L) {
            throw fail("mapped must be 0 or 1");
        }
        ++scene.windows;
        if (mapped == 1L) {
            ++scene.mapped;
        }
    }
    if (file.bad()) {
        throw std::runtime_error{std::format("error reading scene file {}", path.string())};
    }
    if (!have_display) {
        throw std::runtime_error{std::format("{}: missing display line", path.string())};
    }
    return scene;
}

enum class ReleasePolicy { never, unmapped, occluded };

struct ReleasePolicyName {
    ReleasePolicy policy;
    std::string_view name;
};

constexpr std::array kReleasePolicies{
    ReleasePolicyName{ReleasePolicy::never, "never"},
    ReleasePolicyName{ReleasePolicy::unmapped, "unmapped"},
    ReleasePolicyName{ReleasePolicy::occluded, "occluded"},
};

std::optional<ReleasePolicy> parse_release_policy(std::string_view text) {
    for (const ReleasePolicyName& entry : kReleasePolicies) {
        if (entry.name == text) {
            return entry.policy;
        }
    }
    return std::nullopt;
}

std::string_view release_policy_name(ReleasePolicy policy) {
    for (const ReleasePolicyName& entry : kReleasePolicies) {
        if (entry.policy == policy) {
            return entry.name;
        }
    }
    throw std::logic_error{"unknown release policy"};
}

enum class RedrawMode { overdraw, full, damage };

struct RedrawModeName {
    RedrawMode mode;
    std::string_view name;
};

constexpr std::array kRedrawModes{
    RedrawModeName{RedrawMode::overdraw, "overdraw"},
    RedrawModeName{RedrawMode::full, "full"},
    RedrawModeName{RedrawMode::damage, "damage"},
};

std::optional<RedrawMode> parse_redraw_mode(std::string_view text) {
    for (const RedrawModeName& entry : kRedrawModes) {
        if (entry.name == text) {
            return entry.mode;
        }
    }
    return std::nullopt;
}

std::string_view redraw_mode_name(RedrawMode mode) {
    for (const RedrawModeName& entry : kRedrawModes) {
        if (entry.mode == mode) {
            return entry.name;
        }
    }
    throw std::logic_error{"unknown redraw mode"};
}

constexpr std::array kEffectNames{
    std::string_view{"alpha"}, std::string_view{"corners"}, std::string_view{"shadows"},
    std::string_view{"blur"},  std::string_view{"animation"},
};

bool valid_effects(std::string_view text) {
    if (text == "none") {
        return true;
    }
    std::vector<std::string_view> seen;
    for (;;) {
        const std::size_t comma{text.find(',')};
        const std::string_view name{text.substr(0, comma)};
        if (std::ranges::find(kEffectNames, name) == kEffectNames.end() ||
            std::ranges::find(seen, name) != seen.end()) {
            return false;
        }
        seen.push_back(name);
        if (comma == std::string_view::npos) {
            return true;
        }
        text.remove_prefix(comma + 1);
    }
}

struct Options {
    std::filesystem::path scene;
    std::size_t clients{kDefaultClients};
    Seconds settle{kDefaultSettle};
    Seconds measure{kDefaultMeasure};
    std::optional<std::filesystem::path> output;
    std::optional<ReleasePolicy> release;
    std::optional<RedrawMode> redraw;
    std::optional<std::string> effects;
    bool verify_redraw{false};
};

Options parse_options(std::span<char*> argv) {
    Options options;
    bool have_scene{false};
    for (std::size_t i{1}; i < argv.size(); ++i) {
        const std::string_view flag{argv[i]};
        if (flag == "--verify-redraw") {
            options.verify_redraw = true;
            continue;
        }
        if (i + 1 >= argv.size()) {
            throw UsageError{std::format("missing value for {}", flag)};
        }
        const std::string_view value{argv[++i]};
        if (flag == "--scene") {
            options.scene = value;
            have_scene = true;
        } else if (flag == "--clients") {
            const auto clients{parse_number<std::size_t>(value)};
            if (!clients || *clients == 0) {
                throw UsageError{"--clients must be a positive integer"};
            }
            options.clients = *clients;
        } else if (flag == "--settle") {
            const auto settle{parse_number<double>(value)};
            if (!settle || *settle < 0.0) {
                throw UsageError{"--settle must be a non-negative number of seconds"};
            }
            options.settle = Seconds{*settle};
        } else if (flag == "--measure") {
            const auto measure{parse_number<double>(value)};
            if (!measure || *measure <= 0.0) {
                throw UsageError{"--measure must be a positive number of seconds"};
            }
            options.measure = Seconds{*measure};
        } else if (flag == "--output") {
            options.output = value;
        } else if (flag == "--release") {
            const auto release{parse_release_policy(value)};
            if (!release) {
                throw UsageError{"--release must be never, unmapped or occluded"};
            }
            options.release = *release;
        } else if (flag == "--redraw") {
            const auto redraw{parse_redraw_mode(value)};
            if (!redraw) {
                throw UsageError{"--redraw must be overdraw, full or damage"};
            }
            options.redraw = *redraw;
        } else if (flag == "--effects") {
            if (!valid_effects(value)) {
                throw UsageError{"--effects must be none or a comma-separated list of distinct names from "
                                 "alpha, corners, shadows, blur, animation"};
            }
            options.effects = std::string{value};
        } else {
            throw UsageError{std::format("unknown argument {}", flag)};
        }
    }
    if (!have_scene) {
        throw UsageError{"--scene is required"};
    }
    return options;
}

std::filesystem::path executable_directory() {
    std::uint32_t size{0};
    if (_NSGetExecutablePath(nullptr, &size) == 0 || size == 0) {
        throw std::runtime_error{"cannot determine executable path"};
    }
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        throw std::runtime_error{"cannot determine executable path"};
    }
    buffer.resize(std::strlen(buffer.c_str()));
    return std::filesystem::absolute(buffer).lexically_normal().parent_path();
}

std::filesystem::path require_executable(const std::filesystem::path& directory, std::string_view name) {
    const std::filesystem::path path{directory / name};
    if (::access(path.c_str(), X_OK) != 0) {
        throw std::runtime_error{std::format("missing executable {}: {}", path.string(), std::strerror(errno))};
    }
    return path;
}

std::string sysctl_string(const char* name) {
    std::size_t size{0};
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0) {
        throw_errno(std::format("sysctl {}", name));
    }
    std::string value(size, '\0');
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
        throw_errno(std::format("sysctl {}", name));
    }
    value.resize(size);
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

std::string local_time_now() {
    const std::time_t now{std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())};
    std::tm parts{};
    if (localtime_r(&now, &parts) == nullptr) {
        throw_errno("localtime_r");
    }
    std::ostringstream text;
    text << std::put_time(&parts, "%Y-%m-%d %H:%M:%S %z");
    return text.str();
}

enum class Motion { idle, animate, move, reveal };

struct StateSpec {
    Motion motion;
    std::string_view name;
    std::optional<std::string_view> client_flag;
};

constexpr std::array kStates{
    StateSpec{Motion::idle, "idle", std::nullopt},
    StateSpec{Motion::animate, "animate", "--animate-top"},
    StateSpec{Motion::move, "move", "--orbit-top"},
    StateSpec{Motion::reveal, "reveal", "--blink-top"},
};

struct StateResult {
    std::string_view name;
    Bytes compositor_footprint;
    Bytes client_footprint;
    double compositor_cpu_percent{};
    double client_cpu_percent{};
    double frames_per_second{};
    std::optional<double> gpu_ms_per_frame;
    FrameCount frames_checked;
    FrameCount frames_mismatched;
    double reveals_per_second{};
    std::optional<double> reveal_mean_ms;
    std::optional<double> reveal_max_ms;
};

struct Executables {
    std::filesystem::path compositor;
    std::filesystem::path client;
};

class StateRun {
public:
    StateRun(const Executables& executables, const Scene& scene, const Options& options)
        : executables_{executables}, scene_{scene}, options_{options} {}
    StateRun(const StateRun&) = delete;
    StateRun& operator=(const StateRun&) = delete;
    StateRun(StateRun&&) = delete;
    StateRun& operator=(StateRun&&) = delete;
    ~StateRun() { shutdown(); }

    void start_compositor() {
        std::vector<std::string> arguments{"--display", std::format("{}x{}", scene_.width, scene_.height), "--no-input"};
        if (options_.release) {
            arguments.emplace_back("--release");
            arguments.emplace_back(release_policy_name(*options_.release));
        }
        if (options_.redraw) {
            arguments.emplace_back("--redraw");
            arguments.emplace_back(redraw_mode_name(*options_.redraw));
        }
        if (options_.effects) {
            arguments.emplace_back("--effects");
            arguments.emplace_back(*options_.effects);
        }
        if (options_.verify_redraw) {
            arguments.emplace_back("--verify-redraw");
        }
        compositor_ = spawn("compositor", executables_.compositor, arguments, frames_file_.fd().get(), UniqueFd{});
    }

    void start_client(std::size_t index, std::size_t count, std::optional<std::string_view> flag) {
        Pipe pipe{make_pipe()};
        std::vector<std::string> arguments{"--scene", scene_.path.string(), "--slice", std::format("{}/{}", index, count)};
        if (flag) {
            arguments.emplace_back(*flag);
        }
        clients_.push_back(spawn(std::format("client {}/{}", index, count), executables_.client, arguments,
                                 pipe.write.get(), std::move(pipe.read)));
        pipe.write.reset();
        wait_ready(*clients_.back());
        set_nonblocking(clients_.back()->output());
    }

    void hold(Seconds duration) {
        const auto deadline{Clock::now() + std::chrono::duration_cast<Clock::duration>(duration)};
        while (true) {
            check_stop();
            check_alive();
            drain_clients();
            const auto now{Clock::now()};
            if (now >= deadline) {
                return;
            }
            std::this_thread::sleep_for(std::min<Clock::duration>(kPollStep, deadline - now));
        }
    }

    void request_frames() {
        check_alive();
        compositor_->signal(SIGUSR1);
    }

    struct Snapshot {
        Usage compositor;
        Usage clients;
    };

    [[nodiscard]] Snapshot snapshot(const Timebase& timebase) {
        check_alive();
        Snapshot result{sample(*compositor_, timebase), {}};
        for (const auto& client : clients_) {
            const Usage usage{sample(*client, timebase)};
            result.clients.footprint.value += usage.footprint.value;
            result.clients.cpu += usage.cpu;
        }
        return result;
    }

    [[nodiscard]] std::pair<Telemetry, Telemetry> read_frames() {
        const auto deadline{Clock::now() + kFramesTimeout};
        while (true) {
            const auto frames{parse_telemetry(frames_file_.read_all())};
            if (frames.size() >= 2) {
                return {frames.at(0), frames.at(1)};
            }
            check_stop();
            check_alive();
            if (Clock::now() >= deadline) {
                throw std::runtime_error{std::format(
                    "compositor printed {} complete 'frames', 'reveals', 'gpu' and 'verify' line group(s) after two SIGUSR1, "
                    "expected 2",
                    frames.size())};
            }
            std::this_thread::sleep_for(kPollStep);
        }
    }

    void shutdown() noexcept {
        if (compositor_) {
            compositor_->send_noexcept(SIGTERM);
            if (!compositor_->wait_until(Clock::now() + kGrace)) {
                compositor_->kill_and_reap();
            }
        }
        for (const auto& client : clients_) {
            if (!client->wait_until(Clock::now())) {
                client->send_noexcept(SIGTERM);
            }
        }
        const auto deadline{Clock::now() + kGrace};
        for (const auto& client : clients_) {
            if (!client->wait_until(deadline)) {
                client->kill_and_reap();
            }
        }
    }

private:
    static std::vector<Telemetry> parse_telemetry(std::string_view content) {
        std::vector<Telemetry> frames;
        std::size_t start{0};
        while (true) {
            const std::size_t end{content.find('\n', start)};
            if (end == std::string_view::npos) {
                return frames;
            }
            const auto tokens{split_whitespace(content.substr(start, end - start))};
            start = end + 1;
            if (tokens.empty() || tokens.front() != "frames") {
                continue;
            }
            const auto count{tokens.size() == 2 ? parse_number<std::uint64_t>(tokens.at(1)) : std::nullopt};
            if (!count) {
                throw std::runtime_error{"compositor printed a malformed 'frames' line"};
            }
            const std::size_t reveals_end{content.find('\n', start)};
            if (reveals_end == std::string_view::npos) {
                return frames;
            }
            const auto reveal_tokens{split_whitespace(content.substr(start, reveals_end - start))};
            start = reveals_end + 1;
            if (reveal_tokens.empty() || reveal_tokens.front() != "reveals") {
                throw std::runtime_error{"compositor printed no 'reveals' line after a 'frames' line"};
            }
            const auto reveal_count{reveal_tokens.size() == kRevealsLineTokens ? parse_number<std::uint64_t>(reveal_tokens.at(1)) : std::nullopt};
            const auto reveal_total{reveal_tokens.size() == kRevealsLineTokens ? parse_number<std::uint64_t>(reveal_tokens.at(2)) : std::nullopt};
            const auto reveal_max{reveal_tokens.size() == kRevealsLineTokens ? parse_number<std::uint64_t>(reveal_tokens.at(3)) : std::nullopt};
            if (!reveal_count || !reveal_total || !reveal_max) {
                throw std::runtime_error{"compositor printed a malformed 'reveals' line"};
            }
            const std::size_t gpu_end{content.find('\n', start)};
            if (gpu_end == std::string_view::npos) {
                return frames;
            }
            const auto gpu_tokens{split_whitespace(content.substr(start, gpu_end - start))};
            start = gpu_end + 1;
            if (gpu_tokens.empty() || gpu_tokens.front() != "gpu") {
                throw std::runtime_error{"compositor printed no 'gpu' line after a 'reveals' line"};
            }
            const auto gpu_total{gpu_tokens.size() == kGpuLineTokens ? parse_number<std::uint64_t>(gpu_tokens.at(1)) : std::nullopt};
            if (!gpu_total) {
                throw std::runtime_error{"compositor printed a malformed 'gpu' line"};
            }
            const std::size_t verify_end{content.find('\n', start)};
            if (verify_end == std::string_view::npos) {
                return frames;
            }
            const auto verify_tokens{split_whitespace(content.substr(start, verify_end - start))};
            start = verify_end + 1;
            if (verify_tokens.empty() || verify_tokens.front() != "verify") {
                throw std::runtime_error{"compositor printed no 'verify' line after a 'gpu' line"};
            }
            const auto verify_checked{verify_tokens.size() == kVerifyLineTokens ? parse_number<std::uint64_t>(verify_tokens.at(1)) : std::nullopt};
            const auto verify_mismatched{verify_tokens.size() == kVerifyLineTokens ? parse_number<std::uint64_t>(verify_tokens.at(2)) : std::nullopt};
            if (!verify_checked || !verify_mismatched) {
                throw std::runtime_error{"compositor printed a malformed 'verify' line"};
            }
            frames.push_back(Telemetry{FrameCount{*count},
                                       RevealTotals{RevealCount{*reveal_count}, Microseconds{*reveal_total},
                                                    Microseconds{*reveal_max}},
                                       Microseconds{*gpu_total},
                                       VerifyTotals{FrameCount{*verify_checked}, FrameCount{*verify_mismatched}}});
        }
    }

    void check_alive() {
        if (compositor_->exited()) {
            throw std::runtime_error{std::format("compositor exited unexpectedly ({})", compositor_->exit_description())};
        }
        for (const auto& client : clients_) {
            if (client->exited()) {
                throw std::runtime_error{
                    std::format("{} exited unexpectedly ({})", client->name(), client->exit_description())};
            }
        }
    }

    void drain_clients() {
        std::array<char, kReadChunk> chunk{};
        for (const auto& client : clients_) {
            UniqueFd& fd{client->output()};
            while (fd.get() != kInvalidFd) {
                const ssize_t n{::read(fd.get(), chunk.data(), chunk.size())};
                if (n > 0) {
                    continue;
                }
                if (n == 0) {
                    fd.reset();
                } else if (errno != EAGAIN && errno != EINTR) {
                    throw_errno(std::format("read from {}", client->name()));
                }
                break;
            }
        }
    }

    void wait_ready(Child& client) {
        const auto deadline{Clock::now() + kReadyTimeout};
        std::string buffer;
        std::array<char, kReadChunk> chunk{};
        bool end_of_output{false};
        while (true) {
            check_stop();
            if (end_of_output) {
                std::this_thread::sleep_for(kPollStep);
            } else {
                pollfd request{.fd = client.output().get(), .events = POLLIN, .revents = 0};
                const int ready{::poll(&request, 1, kPollStepMs)};
                if (ready < 0 && errno != EINTR) {
                    throw_errno("poll");
                }
                if (ready > 0) {
                    const ssize_t n{::read(client.output().get(), chunk.data(), chunk.size())};
                    if (n < 0 && errno != EINTR) {
                        throw_errno(std::format("read from {}", client.name()));
                    }
                    if (n == 0) {
                        end_of_output = true;
                    }
                    if (n > 0) {
                        buffer.append(chunk.data(), static_cast<std::size_t>(n));
                        if (take_ready_line(buffer)) {
                            return;
                        }
                    }
                }
            }
            if (client.exited()) {
                throw std::runtime_error{
                    std::format("{} exited before printing 'ready' ({})", client.name(), client.exit_description())};
            }
            check_alive();
            if (Clock::now() >= deadline) {
                throw std::runtime_error{std::format("{} did not print 'ready' within {} seconds", client.name(),
                                                     std::chrono::seconds{kReadyTimeout}.count())};
            }
        }
    }

    static bool take_ready_line(std::string& buffer) {
        while (true) {
            const std::size_t end{buffer.find('\n')};
            if (end == std::string::npos) {
                return false;
            }
            const auto tokens{split_whitespace(std::string_view{buffer}.substr(0, end))};
            if (tokens.size() == 1 && tokens.front() == "ready") {
                return true;
            }
            buffer.erase(0, end + 1);
        }
    }

    const Executables& executables_;
    const Scene& scene_;
    const Options& options_;
    TempFile frames_file_{"bench-frames"};
    std::unique_ptr<Child> compositor_;
    std::vector<std::unique_ptr<Child>> clients_;
};

double cpu_percent(CpuTime cpu, Seconds interval) {
    return kPercent * std::chrono::duration_cast<Seconds>(cpu).count() / interval.count();
}

StateResult run_state(const StateSpec& state, const Options& options, const Executables& executables,
                      const Scene& scene, const Timebase& timebase) {
    std::cerr << std::format("bench: state {}: starting compositor and {} client(s)\n", state.name, options.clients);
    StateRun run{executables, scene, options};
    run.start_compositor();
    for (std::size_t index{0}; index < options.clients; ++index) {
        check_stop();
        run.start_client(index, options.clients, state.client_flag);
    }
    std::cerr << std::format("bench: state {}: all clients ready, measuring\n", state.name);
    run.hold(options.settle);

    const auto first_signal{Clock::now()};
    run.request_frames();
    const auto first_sample{Clock::now()};
    const auto first{run.snapshot(timebase)};
    run.hold(options.measure);
    const auto second_sample{Clock::now()};
    const auto second{run.snapshot(timebase)};
    const auto second_signal{Clock::now()};
    run.request_frames();
    const auto [before, after]{run.read_frames()};
    run.shutdown();

    if (after.frames.value < before.frames.value) {
        throw std::runtime_error{"compositor frame count decreased between the two 'frames' lines"};
    }
    if (after.reveals.count.value < before.reveals.count.value || after.reveals.total.value < before.reveals.total.value) {
        throw std::runtime_error{"compositor reveal count or total time decreased between the two 'reveals' lines"};
    }
    if (after.gpu.value < before.gpu.value) {
        throw std::runtime_error{"compositor GPU time decreased between the two 'gpu' lines"};
    }
    if (after.verify.checked.value < before.verify.checked.value ||
        after.verify.mismatched.value < before.verify.mismatched.value) {
        throw std::runtime_error{"compositor verify counts decreased between the two 'verify' lines"};
    }
    const std::uint64_t frame_count{after.frames.value - before.frames.value};
    const std::uint64_t gpu_total{after.gpu.value - before.gpu.value};
    const std::uint64_t reveal_count{after.reveals.count.value - before.reveals.count.value};
    const std::uint64_t reveal_total{after.reveals.total.value - before.reveals.total.value};
    const Seconds sample_interval{second_sample - first_sample};
    const Seconds frame_interval{second_signal - first_signal};
    return StateResult{
        .name = state.name,
        .compositor_footprint = second.compositor.footprint,
        .client_footprint = second.clients.footprint,
        .compositor_cpu_percent = cpu_percent(second.compositor.cpu - first.compositor.cpu, sample_interval),
        .client_cpu_percent = cpu_percent(second.clients.cpu - first.clients.cpu, sample_interval),
        .frames_per_second = static_cast<double>(frame_count) / frame_interval.count(),
        .gpu_ms_per_frame = frame_count == 0 ? std::nullopt
                                             : std::optional{static_cast<double>(gpu_total) /
                                                             static_cast<double>(frame_count) / kMicrosecondsPerMillisecond},
        .frames_checked = FrameCount{after.verify.checked.value - before.verify.checked.value},
        .frames_mismatched = FrameCount{after.verify.mismatched.value - before.verify.mismatched.value},
        .reveals_per_second = static_cast<double>(reveal_count) / frame_interval.count(),
        .reveal_mean_ms = reveal_count == 0 ? std::nullopt
                                            : std::optional{static_cast<double>(reveal_total) /
                                                            static_cast<double>(reveal_count) / kMicrosecondsPerMillisecond},
        .reveal_max_ms = after.reveals.count.value == 0
                             ? std::nullopt
                             : std::optional{static_cast<double>(after.reveals.max.value) / kMicrosecondsPerMillisecond},
    };
}

std::string milliseconds_cell(std::optional<double> value) {
    return value ? std::format("{:.2f}", *value) : std::string{"-"};
}

std::string gpu_cell(std::optional<double> value) {
    return value ? std::format("{:.3f}", *value) : std::string{"-"};
}

double megabytes(Bytes bytes) { return static_cast<double>(bytes.value) / kBytesPerMegabyte; }

struct Facts {
    std::string date;
    std::string model;
    std::string cpu;
    std::string os_version;
};

std::string render(const Facts& facts, const Scene& scene, const Options& options,
                   const std::vector<StateResult>& results) {
    std::string out;
    out += "# Compositor benchmark\n\n";
    out += std::format("- Date: {}\n", facts.date);
    out += std::format("- Machine: {}, {}\n", facts.model, facts.cpu);
    out += std::format("- macOS: {}\n", facts.os_version);
    out += std::format("- Scene: {}\n", scene.path.filename().string());
    out += std::format("- Display: {}x{} points, scale {}\n", scene.width, scene.height, scene.scale);
    out += std::format("- Windows: {} ({} mapped)\n", scene.windows, scene.mapped);
    out += std::format("- Client processes: {}\n", options.clients);
    out += std::format("- Buffer release: {}\n", options.release ? release_policy_name(*options.release) : "default");
    out += std::format("- Redraw: {}\n", options.redraw ? redraw_mode_name(*options.redraw) : "default");
    out += std::format("- Effects: {}\n", options.effects ? *options.effects : "default");
    if (options.verify_redraw) {
        out += "- Redraw verification: on\n";
    }
    out += std::format("- Settle: {:g} s, measure: {:g} s\n\n", options.settle.count(), options.measure.count());
    out += "| State | Compositor footprint (MB) | Client footprint (MB) | Total footprint (MB) "
           "| Compositor CPU (%) | Client CPU (%) | Total CPU (%) | Frames/s | GPU (ms/frame) ";
    std::string separator{"|---|---:|---:|---:|---:|---:|---:|---:|---:|"};
    if (options.verify_redraw) {
        out += "| Frames checked | Frames mismatched ";
        separator += "---:|---:|";
    }
    out += "| Reveals/s | Reveal mean (ms) | Reveal max since start (ms) |\n";
    out += separator;
    out += "---:|---:|---:|\n";
    for (const StateResult& result : results) {
        out += std::format("| {} | {:.1f} | {:.1f} | {:.1f} | {:.1f} | {:.1f} | {:.1f} | {:.1f} | {} ", result.name,
                           megabytes(result.compositor_footprint), megabytes(result.client_footprint),
                           megabytes(Bytes{result.compositor_footprint.value + result.client_footprint.value}),
                           result.compositor_cpu_percent, result.client_cpu_percent,
                           result.compositor_cpu_percent + result.client_cpu_percent, result.frames_per_second,
                           gpu_cell(result.gpu_ms_per_frame));
        if (options.verify_redraw) {
            out += std::format("| {} | {} ", result.frames_checked.value, result.frames_mismatched.value);
        }
        out += std::format("| {:.1f} | {} | {} |\n", result.reveals_per_second, milliseconds_cell(result.reveal_mean_ms),
                           milliseconds_cell(result.reveal_max_ms));
    }
    return out;
}

int run(std::span<char*> argv) {
    const Options options{parse_options(argv)};
    install_signal_handlers();
    const std::filesystem::path directory{executable_directory()};
    const Executables executables{require_executable(directory, "compositor"), require_executable(directory, "demo-client")};
    const Scene scene{load_scene(options.scene)};
    const Facts facts{local_time_now(), sysctl_string("hw.model"), sysctl_string("machdep.cpu.brand_string"),
                      sysctl_string("kern.osproductversion")};
    const Timebase timebase{Timebase::query()};

    std::vector<StateResult> results;
    for (const StateSpec& state : kStates) {
        results.push_back(run_state(state, options, executables, scene, timebase));
    }

    bool mismatch{false};
    const std::string report_text{render(facts, scene, options, results)};
    std::cout << report_text << std::flush;
    if (!std::cout) {
        throw std::runtime_error{"failed to write to stdout"};
    }
    if (options.output) {
        std::ofstream file{*options.output, std::ios::trunc};
        file << report_text;
        file.close();
        if (!file) {
            throw std::runtime_error{std::format("failed to write {}", options.output->string())};
        }
    }
    if (options.verify_redraw) {
        for (const StateResult& result : results) {
            if (result.frames_mismatched.value > 0) {
                std::cerr << std::format("bench: redraw verification failed: state {} had {} mismatched of {} checked frames\n",
                                         result.name, result.frames_mismatched.value, result.frames_checked.value);
                mismatch = true;
            }
        }
    }
    return mismatch ? kErrorExitCode : 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(std::span<char*>{argv, static_cast<std::size_t>(argc)});
    } catch (const UsageError& error) {
        std::cerr << "bench: " << error.what() << '\n' << kUsage;
        return kUsageExitCode;
    } catch (const Interrupted& error) {
        std::cerr << std::format("bench: interrupted by signal {}, children stopped\n", error.signal());
        return kSignalExitBase + error.signal();
    } catch (const std::exception& error) {
        std::cerr << "bench: error: " << error.what() << '\n';
        return kErrorExitCode;
    }
}
