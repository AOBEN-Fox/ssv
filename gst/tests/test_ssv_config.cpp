#include "ssv_config.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

class ScopedConfigEnvironment {
public:
    ScopedConfigEnvironment()
        : original_directory_(std::filesystem::current_path())
    {
        if (const char *value = std::getenv("SSV_CONFIG_PATH")) {
            had_config_path_ = true;
            original_config_path_ = value;
        }
        unsetenv("SSV_CONFIG_PATH");

        char path[] = "/tmp/ssv-config-test-XXXXXX";
        const char *created = mkdtemp(path);
        assert(created != nullptr);
        temporary_directory_ = created;
        std::filesystem::create_directories(
            temporary_directory_ / "config");
        std::filesystem::current_path(temporary_directory_);
    }

    ~ScopedConfigEnvironment()
    {
        std::filesystem::current_path(original_directory_);
        if (had_config_path_)
            setenv("SSV_CONFIG_PATH", original_config_path_.c_str(), 1);
        else
            unsetenv("SSV_CONFIG_PATH");
        std::filesystem::remove_all(temporary_directory_);
    }

private:
    std::filesystem::path original_directory_;
    std::filesystem::path temporary_directory_;
    bool had_config_path_ = false;
    std::string original_config_path_;
};

} // namespace

int main()
{
    ScopedConfigEnvironment environment;
    std::ofstream("config/ssv.example.yaml")
        << "redis:\n  host: example-only-host\n";

    bool loaded_example = false;
    try {
        const auto config = ssv::ssv_config_load();
        loaded_example = config.redis_host == "example-only-host";
    } catch (const std::runtime_error &) {
        // No runtime config is a valid result for this resolver contract.
    }
    assert(!loaded_example);

    const auto explicit_config =
        ssv::ssv_config_load("config/ssv.example.yaml");
    assert(explicit_config.redis_host == "example-only-host");
    return 0;
}
