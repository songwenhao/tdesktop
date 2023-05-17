#ifndef LOGTRACE_H
#define LOGTRACE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN             // 从 Windows 头中排除极少使用的资料
#endif

#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace LogTrace {
    inline char* GetLogStrA(const char* format, va_list args) {
        // log
        size_t len = _vscprintf(format, args)
            + 1 // _vscprintf doesn't count terminating '\0'
            + 32 // time
            + 2 // \r\n
            ;

        char* buffer = (char*)calloc(len, sizeof(char));
        if (buffer) {
            // time
            SYSTEMTIME sysTime;
            memset(&sysTime, 0, sizeof(SYSTEMTIME));
            GetSystemTime(&sysTime);

            size_t offset = 0;

            _snprintf_s(buffer + offset, len - offset, _TRUNCATE, "[%04u/%02u/%02u %02u:%02u:%02u] ", sysTime.wYear,
                sysTime.wMonth, sysTime.wDay, sysTime.wHour + 8, sysTime.wMinute, sysTime.wSecond);
            offset = strlen(buffer);

            _vsnprintf_s(buffer + offset, len - offset, _TRUNCATE, format, args);
            offset = strlen(buffer);

            strncat_s(buffer + offset, len - offset, "\r\n", _TRUNCATE);
        }

        return buffer;
    }

    inline void LogA(const char* format, ...) {
#ifndef _DEBUG
        return;
#endif

        // log
        va_list args;
        va_start(args, format);

        char* buffer = GetLogStrA(format, args);
        if (buffer) {

#ifdef _CONSOLE
            printf(buffer);
            OutputDebugStringA(buffer);
#else
            OutputDebugStringA(buffer);
#endif
            free(buffer);
        }

        va_end(args);
    }

    // release模式也会打印日志
    inline void ForceLogA(const char* format, ...) {
        // log
        va_list args;
        va_start(args, format);

        char* buffer = GetLogStrA(format, args);
        if (buffer) {

#ifdef _CONSOLE
            printf(buffer);
            OutputDebugStringA(buffer);
#else
            OutputDebugStringA(buffer);
#endif
            free(buffer);
        }

        va_end(args);
    }

    inline wchar_t* GetLogStrW(const wchar_t* format, va_list args) {
        // log
        size_t len = _vscwprintf(format, args)
            + 1 // _vscprintf doesn't count terminating '\0'
            + 32 // time
            + 2 // \r\n
            ;

        wchar_t* buffer = (wchar_t*)calloc(len, sizeof(wchar_t));
        if (buffer) {
            // time
            SYSTEMTIME sysTime;
            memset(&sysTime, 0, sizeof(SYSTEMTIME));
            GetSystemTime(&sysTime);

            size_t offset = 0;

            _snwprintf_s(buffer + offset, len - offset, _TRUNCATE, L"[%04u/%02u/%02u %02u:%02u:%02u] ", sysTime.wYear,
                sysTime.wMonth, sysTime.wDay, sysTime.wHour + 8, sysTime.wMinute, sysTime.wSecond);
            offset = wcslen(buffer);

            _vsnwprintf_s(buffer + offset, len - offset, _TRUNCATE, format, args);
            offset = wcslen(buffer);

            wcsncat_s(buffer + offset, len - offset, L"\r\n", _TRUNCATE);
        }

        return buffer;
    }

    inline void LogW(const wchar_t* format, ...) {
#ifndef _DEBUG
        return;
#endif
        va_list args;
        va_start(args, format);

        wchar_t* buffer = GetLogStrW(format, args);
        if (buffer) {
#ifdef _CONSOLE
            wprintf(buffer);
            OutputDebugStringW(buffer);
#else
            OutputDebugStringW(buffer);
#endif
            free(buffer);
        }

        va_end(args);
    }

    // release模式也会打印日志
    inline void ForceLogW(const wchar_t* format, ...) {
        va_list args;
        va_start(args, format);

        wchar_t* buffer = GetLogStrW(format, args);
        if (buffer) {
#ifdef _CONSOLE
            wprintf(buffer);
            OutputDebugStringW(buffer);
#else
            OutputDebugStringW(buffer);
#endif
            free(buffer);
        }

        va_end(args);
    }

    inline void LogToFileA(const char* logFile, const char* format, ...) {
        DWORD writeBytes = 0;
        HANDLE fileHandle = CreateFileA(logFile, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fileHandle == INVALID_HANDLE_VALUE) {
            return;
        }

        SetFilePointer(fileHandle, 0, 0, FILE_END);

        va_list args;
        va_start(args, format);

        char* buffer = GetLogStrA(format, args);
        if (buffer) {
            WriteFile(fileHandle, buffer, (DWORD)strlen(buffer), &writeBytes, NULL);

            free(buffer);
        }

        CloseHandle(fileHandle);
        va_end(args);
    }

    inline void LogToFileW(const wchar_t* logFile, const wchar_t* format, ...) {
        const unsigned char head[] = { 0xFF, 0xFE };
        DWORD writeBytes = 0;
        HANDLE fileHandle = CreateFileW(logFile, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fileHandle != INVALID_HANDLE_VALUE) {
            // 首次创建文件写入UTF16 BOM头
            if (0 == GetLastError()) {
                WriteFile(fileHandle, head, (DWORD)sizeof(head), &writeBytes, NULL);
            }
        } else {
            return;
        }

        SetFilePointer(fileHandle, 0, 0, FILE_END);

        va_list args;
        va_start(args, format);

        wchar_t* buffer = GetLogStrW(format, args);
        if (buffer) {
            WriteFile(fileHandle, buffer, (DWORD)(wcslen(buffer) * sizeof(buffer[0])), &writeBytes, NULL);

            free(buffer);
        }

        CloseHandle(fileHandle);
        va_end(args);
    }
}

#endif //LOGTRACE_H