#include "lingmo_pkgbuild.h"
#include <iostream>
#include <fstream>
#include <cstdlib>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

// 添加静态成员初始化
std::filesystem::path LingmoPkgBuilder::s_buildDir = ".build_deb_lingmo";
std::filesystem::path LingmoPkgBuilder::s_outputDir = "pkg_out";
int LingmoPkgBuilder::s_threadCount = 1;  // 默认单线程
bool LingmoPkgBuilder::s_signBuild = true;        // 默认进行签名
std::string LingmoPkgBuilder::s_signKey = "";     // 默认使用默认密钥

LingmoPkgBuilder::LingmoPkgBuilder(const std::filesystem::path& sourceDir, PackageType type)
    : m_packageType(type) {
    // 先从 changelog 获取正确的包名
    std::string correctName;
    {
        std::ifstream changelog(sourceDir / "debian/changelog");
        if (!changelog.is_open()) {
            throw std::runtime_error("无法打开 changelog 文件");
        }

        std::string line;
        if (std::getline(changelog, line)) {
            // changelog 的第一行格式: package (version) distribution; urgency=level
            size_t spacePos = line.find(' ');
            if (spacePos != std::string::npos) {
                correctName = line.substr(0, spacePos);
            }
        }
    }

    if (correctName.empty()) {
        throw std::runtime_error("无法从 changelog 获取包名");
    }

    // 设置临时目录为 .build_deb_lingmo/正确的包名
    m_tempDir = s_buildDir / correctName;
    std::filesystem::create_directories(m_tempDir);

    // 如果源目录不在构建目录中，需要复制
    if (sourceDir.parent_path() != s_buildDir) {
        // 复制源码到构建目录
        for (const auto& entry : std::filesystem::directory_iterator(sourceDir)) {
            const auto& path = entry.path();
            auto destPath = m_tempDir / entry.path().filename();
            if (entry.is_directory()) {
                std::filesystem::copy(path, destPath,
                    std::filesystem::copy_options::recursive |
                    std::filesystem::copy_options::overwrite_existing);
            } else {
                std::filesystem::copy(path, destPath,
                    std::filesystem::copy_options::overwrite_existing);
            }
        }
    }

    // 从源目录的 debian 目录读取信息
    if (!parseChangelogFile(m_tempDir / "debian/changelog")) {
        std::cerr << "警告: 无法从 changelog 获取版本号\n";
    }
    
    if (!parseControlFile(m_tempDir / "debian/control")) {
        throw std::runtime_error("无法解析 control 文件");
    }
    
    // 检查包类型
    auto formatFile = m_tempDir / "debian/source/format";
    if (std::filesystem::exists(formatFile)) {
        std::ifstream format(formatFile);
        std::string formatStr;
        std::getline(format, formatStr);
        if (formatStr == "3.0 (quilt)") {
            m_packageType = PackageType::Quilt;
        } else if (formatStr == "3.0 (native)") {
            m_packageType = PackageType::Native;
        }
    }

    m_packageName = correctName;
}

void LingmoPkgBuilder::setMaintainer(const std::string& maintainer) {
    m_maintainer = maintainer;
}

void LingmoPkgBuilder::setDescription(const std::string& description) {
    m_description = description;
}

bool LingmoPkgBuilder::createControlFile() {
    auto controlDir = m_tempDir / "DEBIAN";
    std::filesystem::create_directories(controlDir);
    
    std::ofstream control(controlDir / "control");
    if (!control.is_open()) return false;

    control << "Package: " << m_packageName << "\n"
            << "Version: " << m_version << "\n"
            << "Architecture: " << m_architecture << "\n"
            << "Maintainer: " << m_maintainer << "\n"
            << "Description: " << m_description << "\n";
    
    return true;
}

void LingmoPkgBuilder::addFile(const std::string& sourcePath, const std::string& destPath) {
    auto targetPath = m_tempDir / destPath;
    std::filesystem::create_directories(targetPath.parent_path());
    std::filesystem::copy_file(sourcePath, targetPath);
}

bool LingmoPkgBuilder::createOrigTarball() const {
    if (isNativePackage()) {
        return true;  // 原生包不需要 orig tarball
    }

    try {
        // 获取上游版本号（移除 debian 修订版本）
        std::string upstreamVersion = m_version;
        size_t dashPos = m_version.find('-');
        if (dashPos != std::string::npos) {
            upstreamVersion = m_version.substr(0, dashPos);
        }
        
        // 创建一个临时的源码目录
        auto tempSourceDir = m_tempDir / m_packageName;
        std::filesystem::create_directories(tempSourceDir);

        // 复制源文件到临时目录
        for (const auto& entry : std::filesystem::directory_iterator(m_tempDir)) {
            const auto& filename = entry.path().filename();
            if (filename != "debian" && filename != m_packageName) {
                if (entry.is_directory()) {
                    std::filesystem::copy(entry.path(), tempSourceDir,
                        std::filesystem::copy_options::recursive);
                } else {
                    std::filesystem::copy(entry.path(), tempSourceDir / filename);
                }
            }
        }

        // 创建 orig tarball，使用正确的目录名
        std::string tarCmd = "cd " + m_tempDir.string() + " && "
                          + "tar -Jcf " + m_packageName + "_" + upstreamVersion + ".orig.tar.xz "
                          + m_packageName;
        
        bool result = std::system(tarCmd.c_str()) == 0;
        
        // 清理临时源码目录
        std::filesystem::remove_all(tempSourceDir);
        
        return result;
    } catch (const std::exception& e) {
        std::cerr << "创建源码包失败: " << e.what() << "\n";
        return false;
    }
}

bool LingmoPkgBuilder::build(const std::filesystem::path& sourceDir) {
    try {
        // 1. 复制源码到构建目录
        std::string cpCmd = "cp -r " + sourceDir.string() + "/* " + m_tempDir.string();
        if (!runCommand(cpCmd)) {
            std::cerr << "Failed to copy source files\n";
            return false;
        }

        // 2. 如果是 quilt 格式，创建 orig tarball
        if (!isNativePackage()) {
            // 获取上游版本号
            std::string upstreamVersion = m_version;
            size_t dashPos = m_version.find('-');
            if (dashPos != std::string::npos) {
                upstreamVersion = m_version.substr(0, dashPos);
            }

            // 创建 orig tarball，注意 tar 参数的顺序
            std::string tarCmd = "cd " + m_tempDir.parent_path().string() + " && "
                              + "tar --exclude=debian -Jcf " + m_packageName + "_" + upstreamVersion + ".orig.tar.xz "
                              + "-C " + m_packageName + " .";

            if (!runCommand(tarCmd)) {
                std::cerr << "Failed to create orig tarball\n";
                return false;
            }
        }

        // 3. 在源码目录下执行构建
        std::string buildCmd = "cd " + m_tempDir.string() + " && dpkg-buildpackage -b";
        
        // 添加并行构建参数
        if (s_threadCount > 1) {
            buildCmd += " -j" + std::to_string(s_threadCount);
        }
        
        // 添加签名选项
        if (!s_signBuild) {
            buildCmd += " -us -uc --no-sign";
        } else if (!s_signKey.empty()) {
            buildCmd += " -k" + s_signKey;
        }
        
        // 如果是非原生包，添加 -sa 选项
        if (!isNativePackage()) {
            buildCmd += " -sa";
        }

        if (!runCommand(buildCmd)) {
            std::cerr << "Build command failed\n";
            return false;
        }

        if (!copyArtifacts(m_packageName)) {
            std::cerr << "Failed to copy artifacts\n";
            return false;
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "构建失败: " << e.what() << "\n";
        return false;
    }
}

bool LingmoPkgBuilder::parseChangelogFile(const std::filesystem::path& changelogFile) {
    std::cout << "正在解析 changelog 文件: " << changelogFile << "\n";
    
    std::ifstream file(changelogFile);
    if (!file.is_open()) {
        std::cerr << "无法打开 changelog 文件\n";
        return false;
    }

    std::string line;
    if (std::getline(file, line)) {
        // changelog 的第一行格式: package (version) distribution; urgency=level
        size_t start = line.find('(');
        if (start != std::string::npos) {
            size_t end = line.find(')', start);
            if (end != std::string::npos) {
                m_version = line.substr(start + 1, end - start - 1);
                std::cout << "从 changelog 找到版本: " << m_version << "\n";
                return true;
            }
        }
    }

    return false;
}

bool LingmoPkgBuilder::parseControlFile(const std::filesystem::path& controlFile) {
    std::cout << "正在解析 control 文件: " << controlFile << "\n";
    
    std::ifstream file(controlFile);
    if (!file.is_open()) {
        std::cerr << "无法打开 control 文件\n";
        return false;
    }

    // 重置除版本号外的所有字段
    m_packageName.clear();
    m_architecture.clear();
    m_maintainer.clear();
    m_description.clear();
    // 不重置 m_version，保留从 changelog 读取的版本号

    std::string line;
    bool foundSource = false;
    while (std::getline(file, line)) {
        // 跳过空行和注释
        if (line.empty() || line[0] == '#') continue;
        
        // 如果遇到新段落（以空格开头的行是上一行的延续）
        if (!line.empty() && !std::isspace(line[0])) {
            if (line.compare(0, 9, "Package: ") == 0) {
                m_packageName = line.substr(9);
                std::cout << "找到包名: " << m_packageName << "\n";
            }
            else if (line.compare(0, 9, "Version: ") == 0) {
                // 只有在没有从 changelog 读取到版本号时才使用 control 文件中的版本
                if (m_version.empty()) {
                    m_version = line.substr(9);
                    std::cout << "从 control 文件找到版本: " << m_version << "\n";
                }
            }
            else if (line.compare(0, 14, "Architecture: ") == 0) {
                m_architecture = line.substr(14);
                std::cout << "找到架构: " << m_architecture << "\n";
            }
            else if (line.compare(0, 12, "Maintainer: ") == 0) {
                m_maintainer = line.substr(12);
                std::cout << "找到维护者: " << m_maintainer << "\n";
            }
            else if (line.compare(0, 13, "Description: ") == 0) {
                m_description = line.substr(13);
                std::cout << "找到描述: " << m_description << "\n";
            }
            else if (line.compare(0, 8, "Source: ") == 0) {
                foundSource = true;
            }
        }
    }

    // 验证必要字段
    bool isValid = true;
    if (m_packageName.empty()) {
        std::cerr << "错误: 未找到包名\n";
        isValid = false;
    }
    if (m_version.empty()) {
        std::cerr << "警告: 未找到版本号，将使用默认版本 0.1.0\n";
        m_version = "0.1.0";
    } else {
        std::cout << "使用版本号: " << m_version << "\n";
    }
    if (m_architecture.empty()) {
        if (foundSource) {
            m_architecture = "all";
            std::cout << "源码包使用默认架构: all\n";
        } else {
            std::cerr << "错误: 未找到架构\n";
            isValid = false;
        }
    }

    if (!isValid) {
        std::cerr << "control 文件验证失败\n";
        return false;
    }

    std::cout << "control 文件解析成功\n";
    return true;
}

bool LingmoPkgBuilder::copyDebianFiles(const std::filesystem::path& debianDir) {
    try {
        // 复制整个 debian 目录到临时目录
        std::filesystem::copy(debianDir, m_tempDir / "DEBIAN", 
            std::filesystem::copy_options::recursive);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "复制文件失败: " << e.what() << "\n";
        return false;
    }
}

bool LingmoPkgBuilder::buildFromDirectory(const std::filesystem::path& sourceDir, 
                                  const std::string& outputPath) {
    try {
        // 直接使用源目录构造 LingmoPkgBuilder
        LingmoPkgBuilder builder(sourceDir);
        return builder.build(sourceDir);
    } catch (const std::exception& e) {
        std::cerr << "构建失败: " << e.what() << "\n";
        return false;
    }
}

bool LingmoPkgBuilder::copyArtifacts(const std::string& packageName) const {
    try {
        std::filesystem::create_directories(s_outputDir);
        
        // 复制所有非目录文件
        for (const auto& entry : std::filesystem::directory_iterator(m_tempDir.parent_path())) {
            if (!entry.is_directory()) {  // 只复制文件，不复制目录
                std::filesystem::copy(entry.path(), s_outputDir / entry.path().filename(),
                    std::filesystem::copy_options::update_existing);
            }
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "复制产物失败: " << e.what() << "\n";
        return false;
    }
}

bool LingmoPkgBuilder::runCommand(const std::string& cmd) {
    int result = std::system(cmd.c_str());
    return result == 0;
}

bool LingmoPkgBuilder::checkBuildDependencies(const std::filesystem::path& sourceDir) {
#ifdef HAVE_UNISTD_H
    // 检查是否以root权限运行
    if (geteuid() != 0) {
        std::cerr << "错误: 检查构建依赖需要 root 权限\n"
                  << "请使用 sudo 运行此命令\n";
        return false;
    }
#endif

    std::cout << "正在检查构建依赖...\n";
    
    // 先更新包列表
    if (!runCommand("apt-get update")) {
        std::cerr << "错误: 更新包列表失败\n";
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(sourceDir)) {
        if (!entry.is_directory()) continue;
        
        auto debianDir = entry.path() / "debian";
        if (!std::filesystem::exists(debianDir)) continue;

        std::cout << "检查 " << entry.path().filename() << " 的构建依赖...\n";
        
        // 使用 apt build-dep 安装依赖，添加 "./" 前缀
        std::string cmd = "apt build-dep -y ./" + entry.path().string();
        if (!runCommand(cmd)) {
            std::cerr << "错误: " << entry.path().filename() << " 的构建依赖安装失败\n";
            return false;
        }
    }

    std::cout << "所有构建依赖检查完成\n";
    return true;
} 