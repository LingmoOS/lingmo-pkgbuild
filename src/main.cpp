#include "lingmo_pkgbuild.h"
#include <iostream>
#include <filesystem>

void printUsage(const char* programName) {
    std::cout << "Lingmo OS 包构建工具\n\n"
              << "用法:\n"
              << "  lingmo-pkgbuild [选项] <目录>\n"
              << "选项:\n"
              << "  -h, --help     显示帮助信息\n"
              << "  -o, --output   指定输出目录 (默认: pkg_out)\n"
              << "  -b, --build-dir 指定构建目录 (默认: .build_deb_lingmo)\n"
              << "  -j, --jobs     指定并行构建数量 (默认: 1)\n"
              << "  --no-sign      不对包进行签名\n"
              << "  -k, --key      指定签名密钥\n"
              << "  --no-deps      跳过构建依赖检查\n"
              << "  -c, --clean    在构建前后清理构建目录\n"
              << "注意: 构建依赖检查需要 root 权限\n";
}

bool buildAllPackages(const std::filesystem::path& sourceDir, 
                     const std::filesystem::path& outputDir) {
    bool success = true;
    
    for (const auto& entry : std::filesystem::directory_iterator(sourceDir)) {
        if (!entry.is_directory()) continue;
        
        auto debianDir = entry.path() / "debian";
        if (!std::filesystem::exists(debianDir)) continue;
        
        std::cout << "正在构建 " << entry.path().filename() << "...\n";
        
        auto outputPath = outputDir / (entry.path().filename().string() + ".deb");
        if (!LingmoPkgBuilder::buildFromDirectory(debianDir, outputPath.string())) {
            std::cerr << "构建 " << entry.path().filename() << " 失败\n";
            success = false;
        } else {
            std::cout << "成功构建 " << outputPath << "\n";
        }
    }
    
    return success;
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            printUsage(argv[0]);
            return 1;
        }

        std::filesystem::path sourceDir;
        std::filesystem::path outputDir = "pkg_out";
        std::filesystem::path buildDir = ".build_deb_lingmo";
        int threadCount = 1;
        bool sign = true;
        std::string signKey;
        bool checkDeps = true;  // 默认检查依赖
        bool clean = false;     // 默认不清理

        // 解析命令行参数
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-h" || arg == "--help") {
                printUsage(argv[0]);
                return 0;
            } else if (arg == "-o" || arg == "--output") {
                if (++i >= argc) {
                    std::cerr << "错误: 输出目录参数缺失\n";
                    return 1;
                }
                outputDir = argv[i];
            } else if (arg == "-b" || arg == "--build-dir") {
                if (++i >= argc) {
                    std::cerr << "错误: 构建目录参数缺失\n";
                    return 1;
                }
                buildDir = argv[i];
            } else if (arg.substr(0, 2) == "-j") {
                // 处理 -j20 这样的格式
                std::string numStr = arg.substr(2);
                if (numStr.empty() && ++i < argc) {
                    numStr = argv[i];
                }
                try {
                    threadCount = std::stoi(numStr);
                    if (threadCount < 1) {
                        std::cerr << "错误: 并行构建数量必须大于0\n";
                        return 1;
                    }
                } catch (const std::exception&) {
                    std::cerr << "错误: 无效的并行构建数量\n";
                    return 1;
                }
            } else if (arg == "--jobs") {
                if (++i >= argc) {
                    std::cerr << "错误: 并行构建数量参数缺失\n";
                    return 1;
                }
                try {
                    threadCount = std::stoi(argv[i]);
                    if (threadCount < 1) {
                        std::cerr << "错误: 并行构建数量必须大于0\n";
                        return 1;
                    }
                } catch (const std::exception&) {
                    std::cerr << "错误: 无效的并行构建数量\n";
                    return 1;
                }
            } else if (arg == "--no-sign") {
                sign = false;
            } else if (arg == "-k" || arg == "--key") {
                if (++i >= argc) {
                    std::cerr << "错误: 签名密钥参数缺失\n";
                    return 1;
                }
                signKey = argv[i];
            } else if (arg == "--no-deps") {
                checkDeps = false;
            } else if (arg == "-c" || arg == "--clean") {
                clean = true;
            } else if (arg[0] == '-' && arg != "-j") {
                std::cerr << "错误: 未知选项 " << arg << "\n";
                return 1;
            } else {
                sourceDir = arg;
            }
        }

        if (sourceDir.empty()) {
            std::cerr << "错误: 请指定源目录\n";
            return 1;
        }

        if (!std::filesystem::exists(sourceDir)) {
            std::cerr << "错误: 源码目录不存在: " << sourceDir << "\n";
            return 1;
        }

        // 如果指定了清理选项，先清理构建目录
        if (clean) {
            LingmoPkgBuilder::cleanBuildDir();
        }

        // 设置全局配置
        LingmoPkgBuilder::setGlobalBuildDir(buildDir);
        LingmoPkgBuilder::setGlobalOutputDir(outputDir);
        LingmoPkgBuilder::setThreadCount(threadCount);
        LingmoPkgBuilder::setSignBuild(sign);
        if (!signKey.empty()) {
            LingmoPkgBuilder::setSignKey(signKey);
        }

        // 检查构建依赖
        if (checkDeps && !LingmoPkgBuilder::checkBuildDependencies(sourceDir)) {
            std::cerr << "构建依赖检查失败\n";
            return 1;
        }

        // 遍历源码目录中的每个包目录
        bool allSuccess = true;
        for (const auto& entry : std::filesystem::directory_iterator(sourceDir)) {
            if (!entry.is_directory()) continue;

            std::cout << "正在构建 \"" << entry.path().filename().string() << "\"...\n";
            if (!LingmoPkgBuilder::buildFromDirectory(entry.path(), outputDir.string())) {
                std::cerr << "构建 \"" << entry.path().filename().string() << "\" 失败\n";
                allSuccess = false;
            }
        }

        if (!allSuccess) {
            std::cerr << "部分包构建失败\n";
            return 1;
        }

        std::cout << "所有包构建完成\n";
        std::cout << "构建产物位于: " << std::filesystem::absolute(outputDir) << "\n";

        // 如果指定了清理选项，构建完成后再次清理
        if (clean) {
            LingmoPkgBuilder::cleanBuildDir();
        }

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << "\n";
        return 1;
    }
} 