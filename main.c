#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PY_console_private.h"

// ========== 数据结构定义 ==========
#define MAX_SCRIPT_NAME 256
#define MAX_SCRIPT_PATH 512
#define MAX_OUTPUT_SIZE 1 * 1024 * 1024  // 4MB 输出缓冲区
#define MAX_SCRIPTS 100
#define BUFFER_CLEAR_RATIO 2  // 满时保留 1/2，删除 1/2

// 脚本信息结构
typedef struct {
    char name[MAX_SCRIPT_NAME];
    char path[MAX_SCRIPT_PATH];
    BOOL isRunning;
    HANDLE hProcess;
    HANDLE hStdoutRd;
    HANDLE hStdoutWr;
    HANDLE hReadThread;
    HWND hEdit;
    char* outputBuffer;
    int bufferSize;
    DWORD processId;
    BOOL needUpdate;
    BOOL needScroll;
    BOOL isFromConfig;  // TRUE: 从配置文件加载, FALSE: 本地扫描
    int displayedSize;  // 已显示到 EDIT 的字节数
    DWORD lastUpdateTime;  // 上次 UI 更新时间
    int accumulatedBytes;  // 累积的新数据字节数
    CRITICAL_SECTION cs;
} ScriptInfo;

// 全局变量
static ScriptInfo g_scripts[MAX_SCRIPTS];
static int g_scriptCount = 0;
static HWND g_hMainWnd = NULL;
static HWND g_hListBox = NULL;
static HFONT g_hListFont = NULL;
static HFONT g_hButtonFont = NULL;
static HFONT g_hEditFont = NULL;
static int g_currentTab = -1;
static HWND g_currentEdit = NULL;

// 定时器ID
#define TIMER_UPDATE 1
#define UPDATE_INTERVAL 50

// ========== 流式插入数据结构 ==========
typedef struct {
    const char* text;
    int position;
    int length;
} StreamData;

// 流式插入回调函数
DWORD CALLBACK StreamInCallback(DWORD_PTR dwCookie, LPBYTE pbBuff, LONG cb, LONG* pcb) {
    StreamData* data = (StreamData*)dwCookie;
    int remaining = data->length - data->position;
    int toCopy = (cb < remaining) ? cb : remaining;

    if (toCopy > 0) {
        memcpy(pbBuff, data->text + data->position, toCopy);
        data->position += toCopy;
        *pcb = toCopy;
    }
    else {
        *pcb = 0;
    }

    return 0;
}

// ========== 函数声明 ==========
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void LoadConfig(const char* filename);
void SaveConfig(const char* filename);
void AddScript(const char* path, BOOL fromConfig);
void ScanLocalScripts();
void StartScript(int index);
void StopScript(int index);
void AppendToScriptOutput(int index, const char* text);
void SwitchToTab(int index);
void UpdateListDisplay();
void RefreshCurrentOutput();
void ScrollToBottom(HWND hEdit);
DWORD WINAPI ReadPipeThread(LPVOID lpParam);

// ========== 滚动到底部 ==========
void ScrollToBottom(HWND hEdit) {
    if (hEdit && IsWindow(hEdit)) {
        int len = GetWindowTextLengthA(hEdit);
        SendMessageA(hEdit, EM_SETSEL, len, len);
        SendMessageA(hEdit, EM_SCROLLCARET, 0, 0);
        SendMessage(hEdit, WM_VSCROLL, SB_BOTTOM, 0);
    }
}

// ========== 刷新当前显示 ==========
void RefreshCurrentOutput() {
    // 由于 AppendToScriptOutput 已经实时增量更新 EDIT 控件
    // 这里只需要处理滚动即可
    if (g_currentTab >= 0 && g_currentTab < g_scriptCount) {
        ScriptInfo* script = &g_scripts[g_currentTab];
        if (script->hEdit && IsWindow(script->hEdit) && script->needScroll) {
            ScrollToBottom(script->hEdit);
            script->needScroll = FALSE;
        }
    }
}

// ========== 更新列表显示 ==========
void UpdateListDisplay() {
    int currentSel = SendMessageA(g_hListBox, LB_GETCURSEL, 0, 0);
    SendMessageA(g_hListBox, LB_RESETCONTENT, 0, 0);

    for (int i = 0; i < g_scriptCount; i++) {
        char displayName[MAX_SCRIPT_NAME + 50];
        // 根据来源添加不同标记
        const char* sourceMark = g_scripts[i].isFromConfig ? "[配置]" : "[本地]";

        if (g_scripts[i].isRunning) {
            snprintf(displayName, sizeof(displayName), "%s [●] %s", sourceMark, g_scripts[i].name);
        }
        else {
            snprintf(displayName, sizeof(displayName), "%s [○] %s", sourceMark, g_scripts[i].name);
        }
        SendMessageA(g_hListBox, LB_ADDSTRING, 0, (LPARAM)displayName);
    }

    if (currentSel >= 0 && currentSel < g_scriptCount) {
        SendMessageA(g_hListBox, LB_SETCURSEL, currentSel, 0);
    }
}

// ========== 配置文件读取保存 ==========
void LoadConfig(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) return;

    char line[MAX_SCRIPT_PATH];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) > 0) {
            AddScript(line, TRUE);  // 从配置文件加载
        }
    }
    fclose(f);
}

void SaveConfig(const char* filename) {
    FILE* f = fopen(filename, "w");
    if (!f) return;

    // 只保存来自配置文件的脚本
    for (int i = 0; i < g_scriptCount; i++) {
        if (g_scripts[i].isFromConfig) {
            fprintf(f, "%s\n", g_scripts[i].path);
        }
    }
    fclose(f);
}

// ========== 添加脚本 ==========
void AddScript(const char* path, BOOL fromConfig) {
    if (g_scriptCount >= MAX_SCRIPTS) return;

    // 检查是否已存在相同路径的脚本
    for (int i = 0; i < g_scriptCount; i++) {
        if (strcmp(g_scripts[i].path, path) == 0) {
            return;  // 已存在，不重复添加
        }
    }

    ScriptInfo* script = &g_scripts[g_scriptCount];
    strcpy(script->path, path);

    const char* lastSlash = strrchr(path, '\\');
    if (!lastSlash) lastSlash = strrchr(path, '/');
    if (lastSlash) {
        strcpy(script->name, lastSlash + 1);
    }
    else {
        strcpy(script->name, path);
    }

    script->isRunning = FALSE;
    script->hProcess = NULL;
    script->hStdoutRd = NULL;
    script->hStdoutWr = NULL;
    script->hReadThread = NULL;
    script->outputBuffer = (char*)malloc(MAX_OUTPUT_SIZE);
    script->bufferSize = 0;
    script->outputBuffer[0] = '\0';
    script->processId = 0;
    script->hEdit = NULL;
    script->needUpdate = FALSE;
    script->needScroll = TRUE;
    script->isFromConfig = fromConfig;  // 设置来源标记
    script->displayedSize = 0;  // 初始化已显示大小为0
    script->lastUpdateTime = 0;  // 初始化 UI 更新时间
    script->accumulatedBytes = 0;  // 初始化累积字节数

    InitializeCriticalSection(&script->cs);

    g_scriptCount++;
}

// ========== 扫描本地脚本 ==========
void ScanLocalScripts() {
    char exePath[MAX_PATH];
    char searchPath[MAX_PATH];
    WIN32_FIND_DATAA findData;
    HANDLE hFind;

    // 获取程序所在目录
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
    }

    // 构建搜索路径 *.py
    snprintf(searchPath, sizeof(searchPath), "%s*.py", exePath);

    // 开始查找
    hFind = FindFirstFileA(searchPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;  // 没有找到任何 .py 文件
    }

    do {
        // 跳过隐藏文件和系统文件
        if (findData.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
            continue;
        }

        // 构建完整路径
        char fullPath[MAX_PATH];
        snprintf(fullPath, sizeof(fullPath), "%s%s", exePath, findData.cFileName);

        // 添加脚本（标记为本地扫描）
        AddScript(fullPath, FALSE);

    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);
}

// ========== 启动脚本 ==========
void StartScript(int index) {
    if (index < 0 || index >= g_scriptCount) return;
    ScriptInfo* script = &g_scripts[index];
    if (script->isRunning) return;

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    char cmdLine[MAX_SCRIPT_PATH + 100];

    if (!CreatePipe(&script->hStdoutRd, &script->hStdoutWr, &sa, 0)) {
        MessageBoxA(g_hMainWnd, "创建管道失败", "错误", MB_OK);
        return;
    }

    SetHandleInformation(script->hStdoutRd, HANDLE_FLAG_INHERIT, 0);

    si.dwFlags = STARTF_USESTDHANDLES;
    // 不设置 SW_HIDE，让 GUI 程序自行控制窗口显示
    si.hStdOutput = script->hStdoutWr;
    si.hStdError = script->hStdoutWr;
    // 不重定向 stdin，避免影响 GUI 程序启动
    si.hStdInput = NULL;

    // 使用 CREATE_NO_WINDOW 隐藏控制台窗口，但不影响 GUI 程序的界面显示
    snprintf(cmdLine, sizeof(cmdLine), "python.exe -u \"%s\"", script->path);

    if (!CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE,
        CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS,
        NULL, NULL, &si, &pi)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "启动失败: %s\n请确认 python.exe 在 PATH 中", script->path);
        MessageBoxA(g_hMainWnd, msg, "错误", MB_OK);
        CloseHandle(script->hStdoutRd);
        CloseHandle(script->hStdoutWr);
        script->hStdoutRd = script->hStdoutWr = NULL;
        return;
    }

    script->hProcess = pi.hProcess;
    script->processId = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(script->hStdoutWr);
    script->hStdoutWr = NULL;
    script->isRunning = TRUE;

    EnterCriticalSection(&script->cs);
    script->bufferSize = 0;
    script->outputBuffer[0] = '\0';
    script->needScroll = TRUE;
    script->displayedSize = 0;  // 重置已显示大小
    LeaveCriticalSection(&script->cs);

    int* pIndex = (int*)malloc(sizeof(int));
    *pIndex = index;
    script->hReadThread = CreateThread(NULL, 0, ReadPipeThread, pIndex, 0, NULL);

    char msg[256];
    snprintf(msg, sizeof(msg), "\r\n[%s] 脚本已启动 (PID: %d)\r\n",
        script->name, script->processId);
    AppendToScriptOutput(index, msg);

    UpdateListDisplay();

    script->needUpdate = TRUE;
    if (index == g_currentTab) {
        RefreshCurrentOutput();
    }
}

// ========== 停止脚本 ==========
void StopScript(int index) {
    if (index < 0 || index >= g_scriptCount) return;
    ScriptInfo* script = &g_scripts[index];
    if (!script->isRunning) return;

    if (script->hProcess) {
        TerminateProcess(script->hProcess, 0);
        CloseHandle(script->hProcess);
        script->hProcess = NULL;
    }

    if (script->hReadThread) {
        WaitForSingleObject(script->hReadThread, 1000);
        CloseHandle(script->hReadThread);
        script->hReadThread = NULL;
    }

    if (script->hStdoutRd) {
        CloseHandle(script->hStdoutRd);
        script->hStdoutRd = NULL;
    }

    script->isRunning = FALSE;

    char msg[256];
    snprintf(msg, sizeof(msg), "\r\n[%s] 脚本已停止\r\n", script->name);
    AppendToScriptOutput(index, msg);

    UpdateListDisplay();

    if (index == g_currentTab) {
        RefreshCurrentOutput();
    }
}

// ========== 追加输出到脚本缓冲区 ==========
void AppendToScriptOutput(int index, const char* text) {
    if (index < 0 || index >= g_scriptCount) return;
    ScriptInfo* script = &g_scripts[index];

    int textLen = strlen(text);
    if (textLen == 0) return;

    EnterCriticalSection(&script->cs);

    int remaining = MAX_OUTPUT_SIZE - script->bufferSize - 1;
    BOOL bufferCleared = FALSE;  // 标记是否清理了缓冲区

    if (textLen >= remaining) {
        int removeSize = script->bufferSize / BUFFER_CLEAR_RATIO;
        if (removeSize > 0 && removeSize < script->bufferSize) {
            memmove(script->outputBuffer, script->outputBuffer + removeSize,
                script->bufferSize - removeSize);
            script->bufferSize -= removeSize;
            script->outputBuffer[script->bufferSize] = '\0';

            bufferCleared = TRUE;  // 标记已清理

            char warning[128];
            snprintf(warning, sizeof(warning),
                "\r\n[警告] 缓冲区已满，已删除 %d 字节旧数据 (保留最新一半)\r\n", removeSize);
            int warnLen = strlen(warning);

            if (script->bufferSize + warnLen < MAX_OUTPUT_SIZE) {
                strcat(script->outputBuffer, warning);
                script->bufferSize += warnLen;
            }
        }

        remaining = MAX_OUTPUT_SIZE - script->bufferSize - 1;
        if (textLen > remaining) {
            textLen = remaining;
            if (textLen <= 0) {
                LeaveCriticalSection(&script->cs);
                return;
            }
        }
    }

    if (script->bufferSize + textLen < MAX_OUTPUT_SIZE) {
        strncat(script->outputBuffer, text, textLen);
        script->bufferSize += textLen;
    }

    script->needUpdate = TRUE;
    script->needScroll = TRUE;

    LeaveCriticalSection(&script->cs);

    // 如果该脚本的 EDIT 控件已创建，智能节流更新显示
    if (script->hEdit && IsWindow(script->hEdit)) {
        DWORD now = GetTickCount();
        script->accumulatedBytes += textLen;

        // 智能节流：只在以下情况刷新
        // 1. 缓冲区被清理过（必须刷新）
        // 2. 距离上次刷新超过100ms
        // 3. 累积的新数据超过4KB
        BOOL shouldUpdate = bufferCleared ||
            (now - script->lastUpdateTime > 100) ||
            (script->accumulatedBytes > 4096);

        if (shouldUpdate) {
            // 重新设置整个内容（保证同步）
            SetWindowTextA(script->hEdit, script->outputBuffer);
            script->displayedSize = script->bufferSize;

            script->lastUpdateTime = now;
            script->accumulatedBytes = 0;

            // 如果是当前显示的脚本，滚动到底部
            if (index == g_currentTab) {
                ScrollToBottom(script->hEdit);
            }
        }
    }

    // 如果是当前显示的脚本，额外通知主窗口（用于滚动等）
    if (index == g_currentTab) {
        PostMessage(g_hMainWnd, WM_USER + 201, index, 0);
    }
}

// ========== 切换标签页 ==========
void SwitchToTab(int index) {
    if (index < 0 || index >= g_scriptCount) return;

    // 隐藏当前编辑框
    if (g_currentEdit && IsWindow(g_currentEdit)) {
        ShowWindow(g_currentEdit, SW_HIDE);
    }

    g_currentTab = index;
    ScriptInfo* script = &g_scripts[index];

    // 如果还没创建 EDIT 控件，创建并填充内容
    if (!script->hEdit || !IsWindow(script->hEdit)) {
        RECT rc;
        GetClientRect(g_hMainWnd, &rc);

        script->hEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            240, 10, rc.right - 250, rc.bottom - 30,
            g_hMainWnd, (HMENU)(100 + index),
            GetModuleHandle(NULL), NULL);

        if (g_hEditFont) {
            SendMessage(script->hEdit, WM_SETFONT, (WPARAM)g_hEditFont, TRUE);
        }

        // 首次填充内容（只执行一次）
        EnterCriticalSection(&script->cs);
        if (script->bufferSize > 0) {
            SetWindowTextA(script->hEdit, script->outputBuffer);
            script->displayedSize = script->bufferSize;  // 记录已显示的大小
        }
        LeaveCriticalSection(&script->cs);

        ScrollToBottom(script->hEdit);
    }

    // 显示目标编辑框（超快！~2ms）
    ShowWindow(script->hEdit, SW_SHOW);
    g_currentEdit = script->hEdit;

    // 确保滚动到底部
    if (script->needScroll) {
        ScrollToBottom(script->hEdit);
        script->needScroll = FALSE;
    }
}

// ========== 读取管道线程 ==========
DWORD WINAPI ReadPipeThread(LPVOID lpParam) {
    int index = *(int*)lpParam;
    free(lpParam);

    ScriptInfo* script = &g_scripts[index];
    CHAR buffer[4096];
    DWORD bytesRead;
    DWORD totalBytesAvail;

    while (script->isRunning && script->hStdoutRd) {
        if (PeekNamedPipe(script->hStdoutRd, NULL, 0, NULL, &totalBytesAvail, NULL)) {
            if (totalBytesAvail > 0) {
                DWORD toRead = min(totalBytesAvail, sizeof(buffer) - 1);
                if (ReadFile(script->hStdoutRd, buffer, toRead, &bytesRead, NULL) && bytesRead > 0) {
                    buffer[bytesRead] = '\0';
                    AppendToScriptOutput(index, buffer);
                }
            }
        }

        if (script->hProcess) {
            DWORD exitCode;
            if (GetExitCodeProcess(script->hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                while (PeekNamedPipe(script->hStdoutRd, NULL, 0, NULL, &totalBytesAvail, NULL) && totalBytesAvail > 0) {
                    DWORD toRead = min(totalBytesAvail, sizeof(buffer) - 1);
                    if (ReadFile(script->hStdoutRd, buffer, toRead, &bytesRead, NULL) && bytesRead > 0) {
                        buffer[bytesRead] = '\0';
                        AppendToScriptOutput(index, buffer);
                    }
                }

                char msg[256];
                snprintf(msg, sizeof(msg), "\r\n[%s] 脚本已自动结束 (退出码: %d)\r\n",
                    script->name, exitCode);
                AppendToScriptOutput(index, msg);

                script->isRunning = FALSE;
                if (script->hProcess) {
                    CloseHandle(script->hProcess);
                    script->hProcess = NULL;
                }
                if (script->hStdoutRd) {
                    CloseHandle(script->hStdoutRd);
                    script->hStdoutRd = NULL;
                }

                PostMessageA(g_hMainWnd, WM_USER + 200, index, 0);
                break;
            }
        }

        Sleep(10);
    }

    return 0;
}

// ========== 窗口过程 ==========
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hMainWnd = hwnd;

        // 初始化 Common Controls（为 Rich Edit 做准备）
        INITCOMMONCONTROLSEX icex;
        icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
        icex.dwICC = ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icex);

        // ========== 创建字体 ==========
        LOGFONTA lfList = { 0 };
        lfList.lfHeight = 16;
        lfList.lfWidth = 0;
        lfList.lfWeight = FW_NORMAL;
        lfList.lfCharSet = DEFAULT_CHARSET;
        lfList.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
        strcpy(lfList.lfFaceName, "宋体");
        g_hListFont = CreateFontIndirectA(&lfList);

        LOGFONTA lfButton = { 0 };
        lfButton.lfHeight = 18;
        lfButton.lfWidth = 0;
        lfButton.lfWeight = FW_NORMAL;
        lfButton.lfCharSet = DEFAULT_CHARSET;
        lfButton.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
        strcpy(lfButton.lfFaceName, "Microsoft YaHei");
        g_hButtonFont = CreateFontIndirectA(&lfButton);

        LOGFONTA lfEdit = { 0 };
        lfEdit.lfHeight = 14;
        lfEdit.lfWidth = 0;
        lfEdit.lfWeight = FW_NORMAL;
        lfEdit.lfCharSet = DEFAULT_CHARSET;
        lfEdit.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        strcpy(lfEdit.lfFaceName, "宋体");
        g_hEditFont = CreateFontIndirectA(&lfEdit);

        // 创建列表
        g_hListBox = CreateWindowExA(0, "LISTBOX", "",
            WS_CHILD | WS_VISIBLE | WS_BORDER |
            WS_VSCROLL | LBS_NOTIFY,
            10, 10, 220, 500,
            hwnd, (HMENU)1, GetModuleHandle(NULL), NULL);
        SendMessage(g_hListBox, WM_SETFONT, (WPARAM)g_hListFont, TRUE);

        // 创建按钮
        HWND hBtnStart = CreateWindowExA(0, "BUTTON", "启动选中脚本",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 520, 100, 30, hwnd, (HMENU)2,
            GetModuleHandle(NULL), NULL);
        SendMessage(hBtnStart, WM_SETFONT, (WPARAM)g_hButtonFont, TRUE);

        HWND hBtnStop = CreateWindowExA(0, "BUTTON", "停止选中脚本",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            120, 520, 100, 30, hwnd, (HMENU)3,
            GetModuleHandle(NULL), NULL);
        SendMessage(hBtnStop, WM_SETFONT, (WPARAM)g_hButtonFont, TRUE);

        HWND hBtnAdd = CreateWindowExA(0, "BUTTON", "添加脚本",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 560, 100, 30, hwnd, (HMENU)4,
            GetModuleHandle(NULL), NULL);
        SendMessage(hBtnAdd, WM_SETFONT, (WPARAM)g_hButtonFont, TRUE);

        HWND hBtnDelete = CreateWindowExA(0, "BUTTON", "删除脚本",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            120, 560, 100, 30, hwnd, (HMENU)5,
            GetModuleHandle(NULL), NULL);
        SendMessage(hBtnDelete, WM_SETFONT, (WPARAM)g_hButtonFont, TRUE);

        SetTimer(hwnd, TIMER_UPDATE, UPDATE_INTERVAL, NULL);
        LoadConfig("scripts.ini");  // 先加载配置文件
        ScanLocalScripts();         // 再扫描本地脚本
        UpdateListDisplay();

        SetWindowTextA(hwnd, "Python 脚本管理器 - 单击选择查看输出，双击启动/停止");
        break;
    }

    case WM_TIMER: {
        if (wParam == TIMER_UPDATE) {
            // 定时器不再需要刷新 EDIT，因为已经实时增量更新了
            // 只需要更新列表显示（运行状态等）
            UpdateListDisplay();
        }
        break;
    }

    case WM_USER + 201: {
        int index = (int)wParam;
        if (index == g_currentTab && index >= 0 && index < g_scriptCount) {
            ScriptInfo* script = &g_scripts[index];
            // 只需要处理滚动
            if (script->needScroll && script->hEdit && IsWindow(script->hEdit)) {
                ScrollToBottom(script->hEdit);
                script->needScroll = FALSE;
            }
        }
        break;
    }

    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);

        MoveWindow(g_hListBox, 10, 10, 220, rc.bottom - 90, TRUE);

        HWND hBtn = GetDlgItem(hwnd, 2);
        if (hBtn) MoveWindow(hBtn, 10, rc.bottom - 70, 100, 30, TRUE);
        hBtn = GetDlgItem(hwnd, 3);
        if (hBtn) MoveWindow(hBtn, 120, rc.bottom - 70, 100, 30, TRUE);
        hBtn = GetDlgItem(hwnd, 4);
        if (hBtn) MoveWindow(hBtn, 10, rc.bottom - 35, 100, 30, TRUE);
        hBtn = GetDlgItem(hwnd, 5);
        if (hBtn) MoveWindow(hBtn, 120, rc.bottom - 35, 100, 30, TRUE);

        if (g_currentEdit && IsWindow(g_currentEdit)) {
            MoveWindow(g_currentEdit, 240, 10, rc.right - 250, rc.bottom - 30, TRUE);
        }
        break;
    }

    case WM_COMMAND: {
        int selection = SendMessageA(g_hListBox, LB_GETCURSEL, 0, 0);

        switch (LOWORD(wParam)) {
        case 1: {
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                if (selection >= 0 && selection < g_scriptCount) {
                    SwitchToTab(selection);
                }
            }
            else if (HIWORD(wParam) == LBN_DBLCLK) {
                if (selection >= 0 && selection < g_scriptCount) {
                    if (g_scripts[selection].isRunning) {
                        StopScript(selection);
                    }
                    else {
                        StartScript(selection);
                    }
                    if (selection == g_currentTab) {
                        RefreshCurrentOutput();
                    }
                }
            }
            break;
        }
        case 2: {
            if (selection >= 0 && selection < g_scriptCount) {
                StartScript(selection);
                SwitchToTab(selection);
            }
            break;
        }
        case 3: {
            if (selection >= 0 && selection < g_scriptCount) {
                StopScript(selection);
            }
            break;
        }
        case 4: {
            OPENFILENAMEA ofn = { 0 };
            char file[MAX_PATH] = { 0 };
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = "Python Scripts\0*.py\0All Files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

            if (GetOpenFileNameA(&ofn)) {
                AddScript(file, TRUE);  // 手动添加的脚本视为配置脚本
                UpdateListDisplay();
                SaveConfig("scripts.ini");
                SendMessageA(g_hListBox, LB_SETCURSEL, g_scriptCount - 1, 0);
                SwitchToTab(g_scriptCount - 1);
            }
            break;
        }
        case 5: {
            if (selection >= 0 && selection < g_scriptCount) {
                if (g_scripts[selection].isRunning) {
                    StopScript(selection);
                }
                if (g_scripts[selection].outputBuffer) {
                    DeleteCriticalSection(&g_scripts[selection].cs);
                    free(g_scripts[selection].outputBuffer);
                }
                if (g_scripts[selection].hEdit && IsWindow(g_scripts[selection].hEdit)) {
                    DestroyWindow(g_scripts[selection].hEdit);
                }
                for (int i = selection; i < g_scriptCount - 1; i++) {
                    g_scripts[i] = g_scripts[i + 1];
                }
                g_scriptCount--;

                UpdateListDisplay();
                SaveConfig("scripts.ini");

                if (g_scriptCount == 0) {
                    if (g_currentEdit && IsWindow(g_currentEdit)) {
                        ShowWindow(g_currentEdit, SW_HIDE);
                        g_currentEdit = NULL;
                    }
                    g_currentTab = -1;
                }
                else if (selection >= g_scriptCount) {
                    SendMessageA(g_hListBox, LB_SETCURSEL, g_scriptCount - 1, 0);
                    SwitchToTab(g_scriptCount - 1);
                }
                else {
                    SendMessageA(g_hListBox, LB_SETCURSEL, selection, 0);
                    SwitchToTab(selection);
                }
            }
            break;
        }
        }
        break;
    }

    case WM_USER + 200: {
        int index = (int)wParam;
        if (index >= 0 && index < g_scriptCount) {
            UpdateListDisplay();
            if (index == g_currentTab) {
                RefreshCurrentOutput();
            }
        }
        break;
    }

    case WM_CLOSE: {
        for (int i = 0; i < g_scriptCount; i++) {
            if (g_scripts[i].isRunning) {
                StopScript(i);
            }
            DeleteCriticalSection(&g_scripts[i].cs);
        }
        KillTimer(hwnd, TIMER_UPDATE);

        if (g_hListFont) DeleteObject(g_hListFont);
        if (g_hButtonFont) DeleteObject(g_hButtonFont);
        if (g_hEditFont) DeleteObject(g_hEditFont);

        DestroyWindow(hwnd);
        break;
    }

    case WM_DESTROY: {
        PostQuitMessage(0);
        break;
    }

    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ========== 程序入口 ==========
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "PythonScriptManager";
    wc.hIcon = LoadIcon(hInstance, "A");

    if (!RegisterClassA(&wc)) {
        MessageBoxA(NULL, "窗口类注册失败", "错误", MB_OK);
        return 1;
    }

    HWND hwnd = CreateWindowExA(0, "PythonScriptManager", "Python 脚本管理器",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 650,
        NULL, NULL, hInstance, NULL);

    if (!hwnd) {
        MessageBoxA(NULL, "窗口创建失败", "错误", MB_OK);
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
