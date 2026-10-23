/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Source touch daemon.
 *
 * The daemon owns the userspace half of the OPLUS HBP gesture path. It does
 * not change kernel state: the HBP driver already recognises the gesture and
 * publishes both a generic KEY_F4 wake event and the decoded gesture in
 * /proc/touchpanel/coordinate. This daemon translates that HBP event into the
 * per-gesture scan code expected by the Lineage TouchGestures HAL and injects
 * it through the touch input device's evdev node.
 *
 * The legacy oplus_touchscreen_v2 driver reports KEY_GESTURE_START + id from
 * the kernel. HBP does not, and the stock userspace daemon filled that gap in
 * userspace. This daemon prefers a small uinput virtual input device so the
 * legacy gesture scan codes are advertised even when the touch driver's keybit
 * does not include them; it falls back to evdev injection on the touch device.
 * No kernel changes are needed.
 *
 * The legacy sys_edge_touch_config.xml path is still applied when the legacy
 * kernel grip node exists. On HBP that node is absent, so it is skipped
 * cleanly instead of blocking the daemon forever.
 */

#define LOG_TAG "OplusTouchDaemon"

#include <android-base/logging.h>
#include <tinyxml2.h>

#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kGripNode = "/proc/touchpanel/kernel_grip_handle";
constexpr const char* kCoordinateNode = "/proc/touchpanel/coordinate";
constexpr const char* kEdgeConfigName = "sys_edge_touch_config.xml";
constexpr const char* kFeatureConfigName = "sys_touch_function_feature_config.xml";
constexpr int kGestureStartKey = 246;

volatile sig_atomic_t gReloadRequested = 0;

void onReloadSignal(int) {
    gReloadRequested = 1;
}

std::string trimWhitespace(const std::string& value) {
    const auto begin = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char c) { return std::isspace(c); });
    const auto end = std::find_if_not(value.rbegin(), value.rend(),
                                      [](unsigned char c) { return std::isspace(c); }).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

std::vector<std::string> findConfigs(const char* filename) {
    std::vector<std::string> paths;
    std::set<std::string> seen;

    const auto add = [&](const std::string& path) {
        if (seen.insert(path).second && access(path.c_str(), R_OK) == 0) {
            paths.push_back(path);
        }
    };

    add(std::string("/odm/firmware/tp/22825/") + filename);
    add(std::string("/vendor/firmware/tp/22825/") + filename);
    add(std::string("/odm/firmware/tp/") + filename);
    add(std::string("/vendor/firmware/tp/") + filename);

    // The panel directory is selected by the touch firmware on production
    // devices. Keep the explicit 22825 entry above, then fall back to any
    // panel folder present in the image.
    const std::string patterns[] = {
            std::string("/odm/firmware/tp/*/") + filename,
            std::string("/vendor/firmware/tp/*/") + filename,
    };
    for (const auto& pattern : patterns) {
        glob_t matches{};
        if (glob(pattern.c_str(), GLOB_NOSORT, nullptr, &matches) == 0) {
            for (size_t i = 0; i < matches.gl_pathc; ++i) {
                add(matches.gl_pathv[i]);
            }
        }
        globfree(&matches);
    }

    return paths;
}

bool writeGripCommand(const std::string& command) {
    const int fd = open(kGripNode, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        LOG(ERROR) << "cannot open " << kGripNode << ": " << std::strerror(errno);
        return false;
    }

    size_t written = 0;
    while (written < command.size()) {
        const ssize_t count = write(fd, command.data() + written, command.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG(ERROR) << "cannot write " << kGripNode << ": " << std::strerror(errno);
            close(fd);
            return false;
        }
        if (count == 0) {
            LOG(ERROR) << "short write to " << kGripNode;
            close(fd);
            return false;
        }
        written += static_cast<size_t>(count);
    }

    close(fd);
    return true;
}

bool applyEdgeConfig(const std::string& path) {
    tinyxml2::XMLDocument document;
    const tinyxml2::XMLError loadResult = document.LoadFile(path.c_str());
    if (loadResult != tinyxml2::XML_SUCCESS) {
        LOG(ERROR) << "cannot parse " << path << ": " << document.ErrorStr();
        return false;
    }

    const tinyxml2::XMLElement* root = document.RootElement();
    if (root == nullptr) {
        LOG(ERROR) << path << " has no root element";
        return false;
    }

    if (const tinyxml2::XMLElement* open = root->FirstChildElement("isOpen");
        open != nullptr && open->GetText() != nullptr &&
        trimWhitespace(open->GetText()) == "0") {
        LOG(INFO) << path << " reports isOpen=0; leaving kernel defaults untouched";
        return true;
    }

    const tinyxml2::XMLElement* device = root->FirstChildElement("device");
    if (device == nullptr) {
        LOG(ERROR) << path << " has no device element";
        return false;
    }

    std::set<std::string> applied;
    size_t commandCount = 0;
    bool ok = true;

    // Only walk direct <default> children. <default> elements nested inside
    // <packages> are app-specific and require package/activity context that
    // does not exist on YAAP. Applying them unconditionally would be wrong.
    for (const tinyxml2::XMLElement* defaults = device->FirstChildElement("default");
         defaults != nullptr; defaults = defaults->NextSiblingElement("default")) {
        for (const tinyxml2::XMLElement* command = defaults->FirstChildElement("command");
             command != nullptr; command = command->NextSiblingElement("command")) {
            const char* value = command->Attribute("value");
            if (value == nullptr) {
                continue;
            }

            const std::string trimmed = trimWhitespace(value);
            if (trimmed.empty()) {
                continue;
            }

            if (!applied.insert(trimmed).second) {
                continue;
            }

            ++commandCount;
            if (!writeGripCommand(trimmed)) {
                ok = false;
            }
        }
    }

    LOG(INFO) << "applied " << commandCount << " global edge commands from " << path
              << " (result=" << (ok ? "ok" : "partial") << ")";
    return ok;
}

void logFeatureConfig(const std::string& path) {
    tinyxml2::XMLDocument document;
    if (document.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS) {
        LOG(WARNING) << "cannot parse " << path << ": " << document.ErrorStr();
        return;
    }

    const tinyxml2::XMLElement* root = document.RootElement();
    if (root == nullptr) {
        LOG(WARNING) << path << " has no root element";
        return;
    }

    if (const tinyxml2::XMLElement* version = root->FirstChildElement("version");
        version != nullptr && version->GetText() != nullptr) {
        LOG(INFO) << path << " version=" << version->GetText();
    }

    for (const tinyxml2::XMLElement* device = root->FirstChildElement("device");
         device != nullptr; device = device->NextSiblingElement("device")) {
        const int deviceId = device->IntAttribute("id", 0);
        for (const tinyxml2::XMLElement* panel = device->FirstChildElement("panel");
             panel != nullptr; panel = panel->NextSiblingElement("panel")) {
            const char* name = panel->Attribute("name");
            LOG(INFO) << "touch feature device=" << deviceId
                      << " panel=" << (name != nullptr ? name : "<unnamed>");
        }
    }
}

bool applyAll() {
    bool result = true;

    if (access(kGripNode, F_OK) != 0) {
        LOG(INFO) << kGripNode
                  << " is not present (HBP); legacy edge XML is not applicable";
    } else {
        const auto edgeConfigs = findConfigs(kEdgeConfigName);
        if (edgeConfigs.empty()) {
            LOG(ERROR) << "no " << kEdgeConfigName << " found";
            result = false;
        } else if (!applyEdgeConfig(edgeConfigs.front())) {
            result = false;
        }
    }

    const auto featureConfigs = findConfigs(kFeatureConfigName);
    if (featureConfigs.empty()) {
        LOG(WARNING) << "no " << kFeatureConfigName << " found";
    } else {
        logFeatureConfig(featureConfigs.front());
    }

    return result;
}

// ---------------------------------------------------------------------------
// HBP gesture bridge
// ---------------------------------------------------------------------------

int32_t readFile(const char* path, std::string* result) {
    if (result == nullptr) {
        return -1;
    }
    result->clear();

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    char buffer[512];
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    const int savedErrno = errno;
    close(fd);
    if (count < 0) {
        errno = savedErrno;
        return -1;
    }

    result->assign(buffer, static_cast<size_t>(count));
    return static_cast<int32_t>(count);
}

int gestureTypeToKeycode(uint32_t type) {
    // These are the HBP / legacy gesture ids used by
    // drivers/input/touchscreen/.../touchpanel_proc.c. The Lineage HAL uses
    // KEY_GESTURE_START + id, not the raw HBP enum value.
    switch (type) {
        case 2:  return kGestureStartKey + 2;   // UpVee / V
        case 3:  return kGestureStartKey + 3;   // DownVee
        case 4:  return kGestureStartKey + 4;   // LeftVee
        case 5:  return kGestureStartKey + 5;   // RightVee
        case 6:  return kGestureStartKey + 6;   // Circle / O
        case 7:  return kGestureStartKey + 7;   // Double swipe
        case 8:  return kGestureStartKey + 8;   // Left to right
        case 9:  return kGestureStartKey + 9;   // Right to left
        case 10: return kGestureStartKey + 10;  // Up to down
        case 11: return kGestureStartKey + 11;  // Down to up
        case 12: return kGestureStartKey + 12;  // M
        case 13: return kGestureStartKey + 13;  // W
        case 16: return kGestureStartKey + 16;  // Single tap
        case 18: return kGestureStartKey + 18;  // S
        default: return -1;
    }
}

class InputInjector {
  public:
    ~InputInjector() {
        close();
    }

    bool open() {
        mFd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (mFd < 0) {
            LOG(WARNING) << "cannot open /dev/uinput: " << std::strerror(errno);
            return false;
        }

        if (ioctl(mFd, UI_SET_EVBIT, EV_KEY) < 0) {
            LOG(WARNING) << "UI_SET_EVBIT failed: " << std::strerror(errno);
            close();
            return false;
        }

        for (int key = kGestureStartKey; key <= kGestureStartKey + 31; ++key) {
            if (ioctl(mFd, UI_SET_KEYBIT, key) < 0) {
                LOG(WARNING) << "UI_SET_KEYBIT " << key << " failed: "
                             << std::strerror(errno);
                close();
                return false;
            }
        }

        struct uinput_setup setup = {};
        setup.id.bustype = BUS_VIRTUAL;
        setup.id.vendor = 0x1d1d;
        setup.id.product = 0x0001;
        setup.id.version = 1;
        snprintf(setup.name, sizeof(setup.name), "oplus-touch-gesture");
        if (ioctl(mFd, UI_DEV_SETUP, &setup) < 0) {
            LOG(WARNING) << "UI_DEV_SETUP failed: " << std::strerror(errno);
            close();
            return false;
        }

        if (ioctl(mFd, UI_DEV_CREATE) < 0) {
            LOG(WARNING) << "UI_DEV_CREATE failed: " << std::strerror(errno);
            close();
            return false;
        }

        LOG(INFO) << "virtual gesture input device created";
        return true;
    }

    void sendKey(int keycode) {
        if (mFd < 0 || keycode < 0) {
            return;
        }

        struct input_event events[4] = {};
        events[0].type = EV_KEY;
        events[0].code = static_cast<uint16_t>(keycode);
        events[0].value = 1;
        events[1].type = EV_SYN;
        events[1].code = SYN_REPORT;
        events[1].value = 0;
        events[2].type = EV_KEY;
        events[2].code = static_cast<uint16_t>(keycode);
        events[2].value = 0;
        events[3].type = EV_SYN;
        events[3].code = SYN_REPORT;
        events[3].value = 0;

        writeEvents(events, sizeof(events));
    }

    bool valid() const {
        return mFd >= 0;
    }

    void close() {
        if (mFd >= 0) {
            ioctl(mFd, UI_DEV_DESTROY);
            ::close(mFd);
            mFd = -1;
        }
    }

  private:
    int mFd = -1;

    void writeEvents(const struct input_event* events, size_t size) {
        size_t written = 0;
        const auto* data = reinterpret_cast<const char*>(events);
        while (written < size) {
            const ssize_t count = write(mFd, data + written, size - written);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LOG(ERROR) << "failed to write virtual gesture event: "
                           << std::strerror(errno);
                return;
            }
            if (count == 0) {
                return;
            }
            written += static_cast<size_t>(count);
        }
    }
};

class GestureBridge {
  public:
    ~GestureBridge() {
        stop();
    }

    bool start() {
        mRunning.store(true);
        mThread = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        if (!mRunning.exchange(false)) {
            return;
        }
        if (mThread.joinable()) {
            mThread.join();
        }
    }

  private:
    std::atomic<bool> mRunning{false};
    std::thread mThread;
    int mEventFd = -1;
    bool mEventWritable = false;
    InputInjector mInjector;

    int findTouchEvent() {
        for (int i = 0; i < 64; ++i) {
            char path[64];
            snprintf(path, sizeof(path), "/dev/input/event%d", i);
            int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            bool writable = fd >= 0;
            if (fd < 0) {
                fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            }
            if (fd < 0) {
                continue;
            }

            char name[128] = {};
            if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
                close(fd);
                continue;
            }

            if (strstr(name, "touchpanel") != nullptr ||
                strstr(name, "synaptics") != nullptr) {
                mEventWritable = writable;
                LOG(INFO) << "using " << path << " (" << name << ", "
                          << (writable ? "read-write" : "read-only") << ")";
                return fd;
            }

            close(fd);
        }
        return -1;
    }

    void injectKey(int keycode) {
        if (keycode < 0) {
            return;
        }

        if (mInjector.valid()) {
            mInjector.sendKey(keycode);
            LOG(INFO) << "injected gesture key " << keycode;
            return;
        }

        if (mEventFd < 0 || !mEventWritable) {
            return;
        }

        struct input_event events[4] = {};
        events[0].type = EV_KEY;
        events[0].code = static_cast<uint16_t>(keycode);
        events[0].value = 1;
        events[1].type = EV_SYN;
        events[1].code = SYN_REPORT;
        events[1].value = 0;
        events[2].type = EV_KEY;
        events[2].code = static_cast<uint16_t>(keycode);
        events[2].value = 0;
        events[3].type = EV_SYN;
        events[3].code = SYN_REPORT;
        events[3].value = 0;

        size_t written = 0;
        const auto* data = reinterpret_cast<const char*>(events);
        while (written < sizeof(events)) {
            const ssize_t count = write(mEventFd, data + written, sizeof(events) - written);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LOG(ERROR) << "failed to inject gesture key " << keycode << ": "
                           << std::strerror(errno);
                return;
            }
            if (count == 0) {
                LOG(ERROR) << "short write while injecting gesture key";
                return;
            }
            written += static_cast<size_t>(count);
        }
        LOG(INFO) << "injected gesture key " << keycode;
    }

    void handleGesture() {
        std::string value;
        if (readFile(kCoordinateNode, &value) < 0) {
            LOG(DEBUG) << "cannot read " << kCoordinateNode << ": " << std::strerror(errno);
            return;
        }

        const std::string trimmed = trimWhitespace(value);
        if (trimmed.empty()) {
            return;
        }

        char* end = nullptr;
        errno = 0;
        const unsigned long type = strtoul(trimmed.c_str(), &end, 10);
        if (errno != 0 || end == trimmed.c_str()) {
            LOG(DEBUG) << "cannot parse gesture coordinate: " << trimmed;
            return;
        }

        const int keycode = gestureTypeToKeycode(static_cast<uint32_t>(type));
        if (keycode < 0) {
            LOG(DEBUG) << "ignoring unsupported HBP gesture type " << type;
            return;
        }
        injectKey(keycode);
    }

    void run() {
        if (!mInjector.open()) {
            LOG(WARNING) << "falling back to evdev injection on the touch device";
        }

        while (mRunning.load()) {
            if (mEventFd < 0) {
                mEventFd = findTouchEvent();
                if (mEventFd < 0) {
                    LOG(WARNING) << "touch input device not found; retrying";
                    sleep(1);
                    continue;
                }
            }

            struct pollfd pfd = {};
            pfd.fd = mEventFd;
            pfd.events = POLLIN;
            const int pollResult = poll(&pfd, 1, 1000);
            if (pollResult < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LOG(ERROR) << "poll on touch input failed: " << std::strerror(errno);
                close(mEventFd);
                mEventFd = -1;
                continue;
            }
            if (pollResult == 0) {
                continue;
            }
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                LOG(WARNING) << "touch input device disappeared";
                close(mEventFd);
                mEventFd = -1;
                continue;
            }

            struct input_event events[32];
            const ssize_t count = read(mEventFd, events, sizeof(events));
            if (count < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    continue;
                }
                LOG(ERROR) << "read from touch input failed: " << std::strerror(errno);
                close(mEventFd);
                mEventFd = -1;
                continue;
            }

            const size_t eventCount = static_cast<size_t>(count) / sizeof(events[0]);
            for (size_t i = 0; i < eventCount; ++i) {
                const struct input_event& event = events[i];
                if (event.type == EV_KEY && event.code == KEY_F4 && event.value == 1) {
                    handleGesture();
                }
            }
        }

        if (mEventFd >= 0) {
            close(mEventFd);
            mEventFd = -1;
        }
    }
};

}  // namespace

int main() {
    android::base::InitLogging(nullptr, android::base::LogdLogger());
    LOG(INFO) << "source touchDaemon starting";
    LOG(INFO) << "HBP gestures are translated to KEY_GESTURE_START + id in userspace";

    struct sigaction action {};
    action.sa_handler = &onReloadSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGUSR1, &action, nullptr) != 0) {
        LOG(ERROR) << "cannot install SIGUSR1 handler: " << std::strerror(errno);
        return 1;
    }

    GestureBridge gestureBridge;
    gestureBridge.start();

    if (!applyAll()) {
        LOG(WARNING) << "touch configuration incomplete; daemon keeps running";
    }

    while (true) {
        pause();
        if (gReloadRequested != 0) {
            gReloadRequested = 0;
            LOG(INFO) << "reloading touch configuration";
            if (!applyAll()) {
                LOG(WARNING) << "reload was incomplete; keeping current state";
            }
        }
    }

    gestureBridge.stop();
    return 0;
}
