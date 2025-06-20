#define UNICODE
#define _UNICODE

#pragma once
#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <locale.h>
#include <filesystem>

#pragma comment(lib,"shell32.lib")

namespace fs = std::filesystem;
inline std::string to_string(std::wstring const &wstr)
{
    std::string str;
    int const length = WideCharToMultiByte(
        CP_UTF8, 0,
        wstr.c_str(), static_cast<int>(wstr.length()),
        nullptr, 0,
        nullptr, nullptr);
    if (length <= 0)
        return str;
    str.resize(static_cast<size_t>(length));
    int const result = WideCharToMultiByte(
        CP_UTF8, 0,
        wstr.c_str(), static_cast<int>(wstr.length()),
        const_cast<char *>(str.data()), length,
        nullptr, nullptr);
    if (result != length)
        str.clear();
    return str;
}
inline std::wstring to_wstring(std::string const &str)
{
    std::wstring ustr;
    int const length = MultiByteToWideChar(
        CP_UTF8, 0,
        str.c_str(), static_cast<int>(str.length()),
        nullptr, 0);
    if (length <= 0)
        return ustr;
    ustr.resize(static_cast<size_t>(length));
    int const result = MultiByteToWideChar(
        CP_UTF8, 0,
        str.c_str(), static_cast<int>(str.length()),
        const_cast<wchar_t *>(ustr.data()), length);
    if (result != length)
        ustr.clear();
    return ustr;
}
std::vector<std::string> GetHardLinkList(const std::wstring &filePath)
{
    std::vector<std::string> result;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
    {
        std::cerr << "Failed to create pipe." << std::endl;
        return result;
    }

    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = NULL;

    std::wstring cmdLine = L"cmd.exe /c fsutil hardlink list \"" + filePath + L"\"";

    if (!CreateProcess(
            NULL,
            const_cast<LPWSTR>(cmdLine.c_str()),
            NULL, NULL, TRUE,
            CREATE_NO_WINDOW,
            NULL, NULL,
            &si, &pi))
    {
        std::cerr << "Failed to execute command: " << to_string(cmdLine) << std::endl;
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return result;
    }

    CloseHandle(hWrite);

    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
    {
        buffer[bytesRead] = '\0';
        std::string line(buffer);

        line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](char c)
                                              { return !std::isspace(c); }));
        line.erase(std::find_if(line.rbegin(), line.rend(), [](char c)
                                { return !std::isspace(c); })
                       .base(),
                   line.end());

        if (!line.empty())
        {
            result.push_back(line);
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);

    return result;
}
int main()
{
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    std::locale::global(std::locale(".UTF-8"));
    std::cout << "HardLinkRemover v1.0" << std::endl;

    LPWSTR *szArgList;
    int nArgs;

    szArgList = CommandLineToArgvW(GetCommandLine(), &nArgs);

    if (nArgs < 2)
    {
        std::cerr << "Drag files that need to have those hardlinks removed into this exe.";
        Sleep(3000);
        return 1;
    }

    std::vector<std::string> allHardLinks;

    for (int i = 1; i < nArgs; ++i)
    {
        std::wstring file = szArgList[i];

        std::vector<std::string> links = GetHardLinkList(file);

        if (!links.empty())
        {
            std::cout << "Hard links for: " << to_string(file) << std::endl;
            for (const auto &path : links)
            {
                std::cout << path << std::endl;
                allHardLinks.push_back(path);
            }
            std::cout << "Remove these hardlinks?(Y/N)" << std::endl;
            std::string choice;
            std::cin >> choice;
            if (choice == "Y" || choice == "y")
            {
                for (const auto &path : allHardLinks)
                {
                    if (fs::exists(path))
                    {
                        if (fs::remove(path))
                        {
                            std::cout << "Removed: " << path << std::endl;
                        }
                    }
                    else
                    {
                        std::cerr << "Failed to remove: " << path << std::endl;
                    }
                }
            }
            else
            {
                continue;
            }
        }
        else
        {
            std::cerr << "No hard links found or command failed for: " << to_string(file) << std::endl;
            Sleep(3000);
        }
    }
       return 0;
}
