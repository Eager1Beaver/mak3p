#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

int run(const std::vector<std::string>& args) {
    const auto name = std::filesystem::path(args[0]).filename().string();
    if (args.size() > 1 && args[1] == "-version") {
        std::cout << (name.find("ffprobe") != std::string::npos ? "ffprobe" : "ffmpeg")
                  << " version test-helper\n";
        return 0;
    }
    for (const auto& arg : args) {
        if (arg == "-encoders") {
            std::cout << (name.find("no-lame") == std::string::npos ?
                           " A....D libmp3lame MP3\n" : " A....D pcm_s16le PCM\n");
            return 0;
        }
    }
    if (name.find("ffprobe-invalid") != std::string::npos) {
        std::cout << "{invalid";
        return 0;
    }
    if (name.find("ffprobe-bitrate-") != std::string::npos) {
        std::string reported_bitrate = "128000";
        const auto start = name.find("-stream-");
        if (start != std::string::npos) {
            const auto value = name.substr(start + 8);
            reported_bitrate = value.substr(0, value.find('.'));
        }
        std::cout << "{\"streams\":[{\"index\":0,\"codec_type\":\"audio\",\"codec_name\":\"pcm_s16le\","
            "\"sample_rate\":\"44100\",\"channels\":1";
        if (name.find("-stream") != std::string::npos) std::cout << ",\"bit_rate\":\"" << reported_bitrate << "\"";
        std::cout << "}";
        if (name.find("-multi") != std::string::npos) {
            std::cout << ",{\"index\":1,\"codec_type\":\"video\",\"codec_name\":\"h264\"}";
        }
        std::cout << "],\"format\":{\"format_name\":\"wav\",\"duration\":\"0.6\",\"bit_rate\":\""
            << (name.find("-unknown") != std::string::npos ? "N/A" :
                name.find("-stream") != std::string::npos ? "9000000" : "128000") << "\"}}";
        return 0;
    }
    if (name.find("ffprobe-streams") != std::string::npos) {
        std::cout << R"({"streams":[{"index":99,"codec_type":"audio","codec_name":"pcm_s16le","sample_rate":"44100","channels":1},{"index":0,"codec_type":"audio","codec_name":"pcm_s16le","sample_rate":"44100","channels":1}],"format":{"format_name":"wav","duration":"0.6"}})";
        return 0;
    }
    if (name.find("ffprobe-wrong-output") != std::string::npos) {
        std::cout << R"({"streams":[{"index":0,"codec_type":"audio","codec_name":"pcm_s16le","sample_rate":"8000","channels":9}],"format":{"format_name":"wav","duration":"0.6"}})";
        return 0;
    }
    if (name.find("ffmpeg-fail") != std::string::npos) {
        std::cerr << "simulated encoder failure\n";
        return 7;
    }
    if (args.size() < 2) return 1;
    if (args[1] == "arguments") {
        for (std::size_t index = 2; index < args.size(); ++index) {
            std::cout << args[index].size() << ':' << args[index] << '\n';
        }
        return 0;
    }
    if (args[1] == "flood") {
        for (int index = 0; index < 128; ++index) {
            std::cout << std::string(8192, 'o') << std::flush;
            std::cerr << std::string(8192, 'e') << std::flush;
        }
        return 0;
    }
    if (args[1] == "exit") return 7;
    if (args[1] == "sleep" || args[1] == "ignore" || args[1] == "descendant") {
#ifndef _WIN32
        if (args[1] == "ignore") ::signal(SIGTERM, SIG_IGN);
        if (args[1] == "descendant") {
            const auto child = ::fork();
            if (child < 0) return 9;
            if (child == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(900));
                std::ofstream(args.at(2)) << "descendant survived";
                return 0;
            }
        }
        std::cout << ::getpid() << '\n' << std::flush;
#else
        std::cout << GetCurrentProcessId() << '\n' << std::flush;
#endif
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    return 2;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    if (argc == 3 && std::wstring_view(argv[1]) == L"mark") {
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        std::ofstream(std::filesystem::path(argv[2])) << "descendant survived";
        return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"descendant") {
        auto command = std::wstring(L"\"") + argv[0] + L"\" mark \"" + argv[2] + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(argv[0], command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return 9;
        std::cout << child.dwProcessId << '\n' << std::flush;
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    std::vector<std::string> args;
    for (int index = 0; index < argc; ++index) {
        const auto bytes = std::filesystem::path(argv[index]).u8string();
        args.emplace_back(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return run(args);
}
#else
int main(int argc, char** argv) { return run({argv, argv + argc}); }
#endif
