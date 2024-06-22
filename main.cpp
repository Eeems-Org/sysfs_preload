#include <atomic>
#include <chrono>
#include <dirent.h>
#include <dlfcn.h>
#include <filesystem>
#include <linux/fcntl.h>
#include <mutex>
#include <sys/socket.h>
#include <systemd/sd-journal.h>
#include <thread>
#include <unistd.h>
#include <sys/prctl.h>

#define forever for(;;)

static int(*func_open)(const char*, int, mode_t) = nullptr;
static std::mutex logMutex;
static std::thread stateThread;
static std::atomic<bool> stateThreadRunning = false;
static bool exitThread = false;
static int stateFds[2] = {-1, -1};
static std::mutex stateMutex;

inline void __printf_header(int priority){
    std::string level;
    switch(priority){
        case LOG_INFO:
            level = "Info";
            break;
        case LOG_WARNING:
            level = "Warning";
            break;
        case LOG_CRIT:
            level = "Critical";
            break;
        default:
            level = "Debug";
    }
    char name[16];
    prctl(PR_GET_NAME, name);
    auto selfpath = realpath("/proc/self/exe", NULL);
    fprintf(
        stderr,
        "[%i:%i:%i %s - %s] %s: ",
        getpgrp(),
        getpid(),
        gettid(),
        selfpath,
        name,
        level.c_str()
    );
    free(selfpath);
}

inline void __printf_footer(const char* file, unsigned int line, const char* func){
    fprintf(
        stderr,
        " (%s:%u, %s)\n",
        file,
        line,
        func
    );
}
#define _PRINTF(priority, ...) \
    if(std::getenv("OXIDE_PRELOAD_DEBUG") != nullptr){ \
        logMutex.lock(); \
        __printf_header(priority); \
        fprintf(stderr, __VA_ARGS__); \
        __printf_footer(__FILE__, __LINE__, __PRETTY_FUNCTION__); \
        logMutex.unlock(); \
    }
#define _DEBUG(...) _PRINTF(LOG_DEBUG, __VA_ARGS__)
#define _WARN(...) _PRINTF(LOG_WARNING, __VA_ARGS__)
#define _INFO(...) _PRINTF(LOG_INFO, __VA_ARGS__)
#define _CRIT(...) _PRINTF(LOG_CRIT, __VA_ARGS__)

inline std::string trim(std::string& str){
    str.erase(str.find_last_not_of(' ') + 1);
    str.erase(0, str.find_first_not_of(' '));
    return str;
}

void __thread_run(int fd){
    char line[PIPE_BUF];
    forever{
        if(exitThread){
            break;
        }
        int res = read(fd, line, PIPE_BUF);
        if(res == -1){
            if(errno == EINTR){
                continue;
            }
            if(errno == EAGAIN || errno == EIO){
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            _WARN("/sys/power/state pipe failed to read: %s", strerror(errno));
            _DEBUG("/sys/power/state pip fd: %d", fd);
            break;
        }
        if(res == 0){
            continue;
        }
        auto data = std::string(line, res);
        trim(data);
        if(data == "mem" || data == "freeze" || data == "standby"){
            _INFO("Suspending system due to %s request", data.c_str());
            system("systemctl suspend");
        }else{
            _WARN("Unknown power state call: %s", data.c_str());
        }
    }
    stateThreadRunning = false;
}

int __open(const char* pathname, int flags){
    if(strcmp(pathname, "/sys/power/state") != 0){
        return -2;
    }
    // TODO - handle O_RDWR somehow
    if(flags == O_RDONLY){
        return -2;
    }
    stateMutex.lock();
    if(stateFds[0] != -1){
        if(!stateThreadRunning){
            stateThread.join();
            stateThreadRunning = true;
            stateThread = std::thread(__thread_run, stateFds[1]);
        }
        stateMutex.unlock();
        _INFO("Getting /sys/power/state pipe");
        return stateFds[0];
    }
    _INFO("Opening /sys/power/state pipe");
    int socketFlags = SOCK_STREAM;
    if((flags & O_NONBLOCK) || (flags & O_NDELAY)){
        socketFlags |= SOCK_NONBLOCK;
    }
    if(socketpair(AF_UNIX, socketFlags, 0, stateFds) == 0){
        stateThreadRunning = true;
        stateThread = std::thread(__thread_run, stateFds[1]);
        _INFO("/sys/power/state pipe opened");
    }else{
        _WARN("Unable to open /sys/power/state pipe: %s", strerror(errno));
    }
    stateMutex.unlock();
    return stateFds[0];
}

extern "C" {
    __attribute__((visibility("default")))
    int open64(const char* pathname, int flags, mode_t mode = 0){
        static const auto func_open64 = (int(*)(const char*, int, mode_t))dlsym(RTLD_NEXT, "open64");
        int fd = __open(pathname, flags);
        if(fd == -2){
            fd = func_open64(pathname, flags, mode);
        }
        return fd;
    }

    __attribute__((visibility("default")))
    int openat(int dirfd, const char* pathname, int flags, mode_t mode = 0){
        static const auto func_openat = (int(*)(int, const char*, int, mode_t))dlsym(RTLD_NEXT, "openat");
        int fd = __open(pathname, flags);
        if(fd == -2){
            DIR* save = opendir(".");
            fchdir(dirfd);
            char path[PATH_MAX+1];
            getcwd(path, PATH_MAX);
            fchdir(::dirfd(save));
            closedir(save);
            std::string filepath(path);
            filepath += "/";
            filepath += pathname;
            fd = __open(filepath.c_str(), flags);
        }
        if(fd == -2){
            fd = func_openat(dirfd, pathname, flags, mode);
        }
        return fd;
    }

    __attribute__((visibility("default")))
    int open(const char* pathname, int flags, mode_t mode = 0){
        int fd = __open(pathname, flags);
        if(fd == -2){
            fd = func_open(pathname, flags, mode);
        }
        return fd;
    }

    void __attribute__ ((constructor)) init(void);
    void init(void){
        _INFO("Starting sysfs preload")
        func_open = (int(*)(const char*, int, mode_t))dlsym(RTLD_NEXT, "open");
    }

    __attribute__((visibility("default")))
    int __libc_start_main(
        int(*_main)(int, char**, char**),
        int argc,
        char** argv,
        int(*init)(int, char**, char**),
        void(*fini)(void),
        void(*rtld_fini)(void),
        void* stack_end
    ){
        auto func_main = (decltype(&__libc_start_main))dlsym(RTLD_NEXT, "__libc_start_main");
        int res = func_main(
            _main,
            argc,
            argv,
            init,
            fini,
            rtld_fini,
            stack_end
        );
        if(stateThreadRunning){
            _DEBUG("Waiting for thread to exit");
            exitThread = true;
            stateThread.join();
        }
        return res;
    }
}
