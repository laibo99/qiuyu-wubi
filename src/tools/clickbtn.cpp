// 测试辅助：按窗口类名找到顶层窗口，点击其中指定 ID 的按钮（BM_CLICK）
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: clickbtn <windowClass> <buttonId> [windowTitle]\n");
        return 1;
    }
    wchar_t cls[128] = {0};
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, cls, 128);
    int id = atoi(argv[2]);
    HWND top = nullptr;
    for (int i = 0; i < 40 && !top; ++i) {
        top = FindWindowW(cls, nullptr);
        if (!top) Sleep(100);
    }
    if (!top) {
        printf("window not found: %ls\n", cls);
        return 2;
    }
    HWND btn = GetDlgItem(top, id);
    if (!btn) {
        printf("control %d not found\n", id);
        return 3;
    }
    // 用 PostMessage：被点击的按钮可能弹出模态窗口，同步 SendMessage 会一直阻塞
    BOOL ok = PostMessageW(btn, BM_CLICK, 0, 0);
    printf("clicked id=%d hwnd=%p ok=%d\n", id, (void*)btn, (int)ok);
    return ok ? 0 : 4;
}
