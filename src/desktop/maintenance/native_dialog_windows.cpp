#include "native_dialog.hpp"
#include "storage_inventory.hpp"
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <stdexcept>

namespace marksix::maintenance {
namespace {
struct Dialog { HWND label{}, text{}, yes{}, no{}; bool approved = false; bool done = false; };
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM first, LPARAM second) {
    auto* dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        dialog = static_cast<Dialog*>(reinterpret_cast<CREATESTRUCTW*>(second)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
    }
    if (dialog && message == WM_SIZE) {
        const int width = LOWORD(second), height = HIWORD(second);
        MoveWindow(dialog->label, 16, 16, std::max(100, width - 32), 24, TRUE);
        MoveWindow(dialog->text, 16, 44, std::max(100, width - 32), std::max(100, height - 110), TRUE);
        MoveWindow(dialog->no, std::max(16, width - 142), height - 54, 126, 36, TRUE);
        if (dialog->yes) MoveWindow(dialog->yes, std::max(16, width - 360), height - 54, 206, 36, TRUE);
        return 0;
    }
    if (message == DM_GETDEFID) return MAKELONG(IDCANCEL, DC_HASDEFID);
    if (dialog && (message == WM_CLOSE || (message == WM_COMMAND && (LOWORD(first) == IDOK || LOWORD(first) == IDCANCEL)))) {
        dialog->approved = message == WM_COMMAND && LOWORD(first) == IDOK;
        dialog->done = true; DestroyWindow(window); return 0;
    }
    return DefWindowProcW(window, message, first, second);
}
std::wstring wide(const std::string& text) {
    try { return pathFromUtf8(text).native(); }
    catch (const std::exception&) { return pathFromUtf8(quotedReportField(text)).native(); }
}
}
void nativeStartupBlocked(const std::string& message) {
    MessageBoxW(nullptr, wide(message).c_str(), L"Mark Six Studio cannot start", MB_OK | MB_ICONINFORMATION);
}
bool nativeReview(const std::string& title, const std::string& report, const std::string& approve, const std::string& cancel) {
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{}; type.lpfnWndProc = windowProc; type.hInstance = instance;
    type.lpszClassName = L"MarkSixMaintenanceReview1"; type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&type);
    Dialog dialog;
    const auto window = CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, wide(title).c_str(),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 840, 580, nullptr, nullptr, instance, &dialog);
    if (!window) return false;
    dialog.label = CreateWindowExW(0, L"STATIC", L"Maintenance details and preserved legacy files", WS_CHILD | WS_VISIBLE,
        16, 16, 790, 24, window, nullptr, instance, nullptr);
    dialog.text = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wide(report).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        16, 44, 790, 412, window, nullptr, instance, nullptr);
    SendMessageW(dialog.text, EM_SETLIMITTEXT, 16 * 1024 * 1024, 0);
    if (!approve.empty()) dialog.yes = CreateWindowExW(0, L"BUTTON", wide(approve).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        454, 470, 206, 36, window, reinterpret_cast<HMENU>(INT_PTR(IDOK)), instance, nullptr);
    dialog.no = CreateWindowExW(0, L"BUTTON", wide(cancel).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        672, 470, 126, 36, window, reinterpret_cast<HMENU>(INT_PTR(IDCANCEL)), instance, nullptr);
    const auto font = GetStockObject(DEFAULT_GUI_FONT);
    for (const auto control : {dialog.label, dialog.text, dialog.yes, dialog.no}) if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    ShowWindow(window, SW_SHOW); SetForegroundWindow(window); SetFocus(dialog.no);
    MSG message{};
    while (!dialog.done && GetMessageW(&message, nullptr, 0, 0) > 0) if (!IsDialogMessageW(window, &message)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return dialog.approved;
}
struct NativeProgress::Impl { HWND window = nullptr, label = nullptr; };
NativeProgress::NativeProgress() : impl_(std::make_unique<Impl>()) {
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{}; type.lpfnWndProc = [](HWND window, UINT message, WPARAM first, LPARAM second) -> LRESULT {
        if (message == WM_CLOSE) return 0; // Destructive commit has already been approved.
        return DefWindowProcW(window, message, first, second);
    };
    type.hInstance = instance; type.lpszClassName = L"MarkSixMaintenanceProgress1";
    type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&type);
    impl_->window = CreateWindowExW(0, type.lpszClassName, L"Mark Six Studio cleanup in progress", WS_OVERLAPPED | WS_CAPTION,
        CW_USEDEFAULT, CW_USEDEFAULT, 660, 220, nullptr, nullptr, instance, nullptr);
    if (!impl_->window) throw std::runtime_error("Cannot display native cleanup progress");
    impl_->label = CreateWindowExW(0, L"STATIC", L"Cleanup progress", WS_CHILD | WS_VISIBLE,
        24, 24, 600, 150, impl_->window, nullptr, instance, nullptr);
    if (!impl_->label) { DestroyWindow(impl_->window); throw std::runtime_error("Cannot display native cleanup progress details"); }
    SendMessageW(impl_->label, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    ShowWindow(impl_->window, SW_SHOW); SetForegroundWindow(impl_->window);
}
NativeProgress::~NativeProgress() { if (impl_->window) DestroyWindow(impl_->window); }
void NativeProgress::update(const std::string& message, std::uint64_t completed, std::uint64_t total) {
    SetWindowTextW(impl_->label, wide("Cleanup progress\r\n" + message + "\r\n" + std::to_string(completed) + " / " + std::to_string(total) +
        "\r\nThe final result will list preserved legacy files and remaining work.").c_str());
    UpdateWindow(impl_->window);
    MSG event{};
    for (unsigned i = 0; i < 64 && PeekMessageW(&event, impl_->window, 0, 0, PM_REMOVE); ++i) {
        TranslateMessage(&event); DispatchMessageW(&event);
    }
}

}
