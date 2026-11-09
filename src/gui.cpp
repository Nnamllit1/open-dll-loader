#include "loader.h"
#include "managed_host.h"
#include "ui_style.h"
#include <commdlg.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

namespace {
constexpr int REFRESH = 101, ADD = 102, REMOVE = 103, LOAD = 104, CHECK = 105, INSPECT = 106, COPY = 107;
constexpr int RELOAD = 108, UNLOAD = 109;
constexpr int SEARCH = 201, COMPATIBLE = 202, PROCESSES = 203, QUEUE = 204;
constexpr int EMPTY_TITLE = 206, EMPTY_HINT = 207;
constexpr UINT EVENT = WM_APP + 1;
constexpr UINT SNAPSHOT = WM_APP + 2;
constexpr UINT_PTR REFRESH_TIMER = 10;
HWND window{}, search{}, compatible{}, processList{}, queueList{}, logBox{};
HIMAGELIST rowImages{};
std::vector<odl::Process> allProcesses, visibleProcesses;
struct Library { odl::Dll dll; std::wstring status = L"Ready"; odl::State state = odl::State::already_loaded; };
std::vector<Library> libraries;
odl::Process target;
std::thread worker;
std::thread refreshWorker;
bool busy{}, rebuilding{}, testMode{}, refreshing{}, modal{}, cooperative{}, hostActive{};
std::wstring statusText = L"Ready", activity;
COLORREF statusColor = ui::green;
int clientWidth = 1220, clientHeight = 820, queueBottom = 534;
struct UiEvent { int row = -1; std::wstring message; std::wstring label; odl::State state = odl::State::already_loaded; bool done{}, success{}; int managedActive = -1; };
struct Snapshot { std::vector<odl::Process> processes; odl::Process selected; odl::HostReply host; bool manual{}; std::wstring error; };
void request_refresh(bool manual = false);

std::wstring value(HWND h) {
    const auto n = GetWindowTextLengthW(h); std::wstring out(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, out.data(), n + 1); out.resize(n); return out;
}
std::wstring lower(std::wstring s) { std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); }); return s; }
std::wstring leaf(const std::wstring& path) { return std::filesystem::path(path).filename().wstring(); }
int selected_library() { return ListView_GetNextItem(queueList, -1, LVNI_SELECTED); }
size_t checked_count() {
    size_t n{}; for (int i = 0; i < static_cast<int>(libraries.size()); ++i) if (ListView_GetCheckState(queueList, i)) ++n;
    return n;
}
void update_actions() {
    ShowWindow(queueList, libraries.empty() ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(window, EMPTY_TITLE), libraries.empty() ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(window, EMPTY_HINT), libraries.empty() ? SW_SHOW : SW_HIDE);
    const bool chosen = target.pid && target.machine == odl::own_machine();
    const bool queueReady = checked_count() > 0;
    for (int id : {ADD, REFRESH, REMOVE, INSPECT, CHECK, LOAD, RELOAD, UNLOAD}) {
        bool enabled = !busy;
        if (id == REMOVE || id == INSPECT) enabled = enabled && selected_library() >= 0;
        if (id == CHECK || id == LOAD) enabled = enabled && chosen && queueReady;
        if (id == RELOAD || id == UNLOAD) enabled = enabled && cooperative && target.pid && (id == RELOAD || hostActive);
        EnableWindow(GetDlgItem(window, id), enabled);
    }
    EnableWindow(search, !busy); EnableWindow(compatible, !busy); EnableWindow(processList, !busy); EnableWindow(queueList, !busy);
    DragAcceptFiles(window, !busy);
    InvalidateRect(window, nullptr, FALSE);
}
void set_status(std::wstring text, COLORREF color = ui::green) { statusText = std::move(text); statusColor = color; InvalidateRect(window, nullptr, FALSE); }
void log(const std::wstring& text) {
    SYSTEMTIME now{}; GetLocalTime(&now); wchar_t stamp[24]{};
    swprintf_s(stamp, L"%02u:%02u:%02u  ", now.wHour, now.wMinute, now.wSecond);
    activity += stamp + text + L"\r\n\r\n";
    if (activity.size() > 500000) activity.erase(0, activity.find(L"\r\n", activity.size() - 400000) + 2);
    SetWindowTextW(logBox, activity.c_str());
    SendMessageW(logBox, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1)); SendMessageW(logBox, EM_SCROLLCARET, 0, 0);
}
void report_error(const std::wstring& text) { log(text); set_status(L"Needs attention", ui::red); }
void copy_log() {
    if (!OpenClipboard(window)) throw std::wstring(L"Could not open the clipboard.");
    struct Clipboard { ~Clipboard() { CloseClipboard(); } } close;
    const SIZE_T bytes = (activity.size() + 1) * sizeof(wchar_t);
    HANDLE memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) throw std::wstring(L"Could not allocate clipboard text.");
    auto p = GlobalLock(memory);
    if (!p) { GlobalFree(memory); throw std::wstring(L"Could not prepare clipboard text."); }
    memcpy(p, activity.c_str(), bytes); GlobalUnlock(memory);
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory)) { GlobalFree(memory); throw std::wstring(L"Could not copy the log."); }
    set_status(L"Log copied");
}
void column(HWND list, int index, const wchar_t* label, int width) {
    LVCOLUMNW col{}; col.mask = LVCF_TEXT | LVCF_WIDTH; col.cx = ui::px(width); col.pszText = const_cast<LPWSTR>(label);
    ListView_InsertColumn(list, index, &col);
}
void cell(HWND list, int row, int col, const std::wstring& text) { ListView_SetItemText(list, row, col, const_cast<LPWSTR>(text.c_str())); }
void choose_process() {
    if (rebuilding) return;
    const auto i = ListView_GetNextItem(processList, -1, LVNI_SELECTED);
    target = i >= 0 && static_cast<size_t>(i) < visibleProcesses.size() ? visibleProcesses[i] : odl::Process{};
    cooperative = hostActive = false;
    set_status(target.pid ? L"Process selected" : L"Ready"); update_actions();
    if (!testMode && target.pid) request_refresh();
}
void filter_processes(bool preserveScroll = false) {
    const auto previous = target;
    const int top = preserveScroll ? ListView_GetTopIndex(processList) : 0;
    const auto query = lower(value(search));
    const bool same = SendMessageW(compatible, BM_GETCHECK, 0, 0) == BST_CHECKED;
    rebuilding = true; SendMessageW(processList, WM_SETREDRAW, FALSE, 0); ListView_DeleteAllItems(processList); visibleProcesses.clear();
    target = {};
    for (const auto& p : allProcesses) {
        if (same && p.machine != odl::own_machine()) continue;
        if (!query.empty() && lower(p.name + L" " + std::to_wstring(p.pid) + L" " + p.path).find(query) == std::wstring::npos) continue;
        const int i = static_cast<int>(visibleProcesses.size());
        visibleProcesses.push_back(p);
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = i; item.pszText = const_cast<LPWSTR>(p.name.c_str()); ListView_InsertItem(processList, &item);
        cell(processList, i, 1, std::to_wstring(p.pid)); cell(processList, i, 2, odl::machine_name(p.machine));
        if (p.pid == previous.pid && p.created == previous.created) {
            ListView_SetItemState(processList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); target = p;
        }
    }
    SendMessageW(processList, WM_SETREDRAW, TRUE, 0); InvalidateRect(processList, nullptr, TRUE); rebuilding = false;
    if (previous.pid && !target.pid) { cooperative = hostActive = false; set_status(L"Select a process", ui::accent); }
    if (top > 0 && !visibleProcesses.empty()) ListView_Scroll(processList, 0, ui::px(30) * std::min(top, static_cast<int>(visibleProcesses.size()) - 1));
    update_actions();
}
void refresh() {
    if (!testMode) allProcesses = odl::processes();
    filter_processes(); set_status(L"Process list refreshed");
}
void receive_snapshot(Snapshot snapshot) {
    const auto same = [](const odl::Process& a, const odl::Process& b) { return a.pid == b.pid && a.created == b.created && a.machine == b.machine && a.name == b.name && a.path == b.path; };
    const bool changed = allProcesses.size() != snapshot.processes.size() || !std::equal(allProcesses.begin(), allProcesses.end(), snapshot.processes.begin(), snapshot.processes.end(), same);
    if (snapshot.error.empty() && changed) { allProcesses = std::move(snapshot.processes); filter_processes(true); }
    if (target.pid && target.pid == snapshot.selected.pid && target.created == snapshot.selected.created) {
        cooperative = snapshot.host.available; hostActive = snapshot.host.active;
    }
    if (snapshot.manual) {
        if (!snapshot.error.empty()) report_error(snapshot.error);
        else if (!busy) set_status(L"Process list refreshed");
    }
    update_actions();
}
void request_refresh(bool manual) {
    if (busy || refreshing || modal || (!manual && IsIconic(window))) return;
    if (refreshWorker.joinable()) refreshWorker.join();
    const auto selected = target; const bool fixtureMode = testMode;
    const auto fixtures = fixtureMode ? allProcesses : std::vector<odl::Process>{};
    refreshing = true;
    try {
        refreshWorker = std::thread([selected, fixtureMode, fixtures, manual] {
            auto snapshot = std::make_unique<Snapshot>(); snapshot->selected = selected; snapshot->manual = manual;
            try {
                snapshot->processes = fixtureMode ? fixtures : odl::processes();
                if (!fixtureMode && selected.pid) snapshot->host = odl::host_command(selected, odl::HostCommand::status, 120);
            } catch (const std::wstring& error) { snapshot->error = error; }
              catch (...) { snapshot->error = L"Could not refresh the process list."; }
            auto raw = snapshot.release(); if (!PostMessageW(window, SNAPSHOT, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
        });
    } catch (...) { refreshing = false; throw; }
}
void append_library(odl::Dll dll) {
    if (std::any_of(libraries.begin(), libraries.end(), [&](const auto& d) { return _wcsicmp(d.dll.path.c_str(), dll.path.c_str()) == 0; })) {
        set_status(L"DLL already in queue"); return;
    }
    const int index = static_cast<int>(libraries.size());
    libraries.push_back({std::move(dll)});
    const auto& d = libraries.back().dll; auto name = leaf(d.path);
    LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = index; item.pszText = name.data(); ListView_InsertItem(queueList, &item);
    cell(queueList, index, 1, odl::machine_name(d.machine)); cell(queueList, index, 2, L"Ready"); ListView_SetCheckState(queueList, index, TRUE);
    ListView_SetItemState(queueList, -1, 0, LVIS_SELECTED); ListView_SetItemState(queueList, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    log(L"Added " + name + L"  (" + odl::machine_name(d.machine) + L")"); set_status(L"DLL added"); update_actions();
}
void add(const std::wstring& path) {
    auto dll = odl::inspect(path);
    if (dll.machine != odl::own_machine()) throw std::wstring(L"This DLL is ") + odl::machine_name(dll.machine) + L". Open the matching loader build to use it.";
    append_library(std::move(dll));
}
void browse() {
    std::vector<wchar_t> names(65536);
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = window; ofn.lpstrTitle = L"Add native DLLs";
    ofn.lpstrFilter = L"Native DLL files (*.dll)\0*.dll\0All files\0*.*\0"; ofn.lpstrFile = names.data(); ofn.nMaxFile = static_cast<DWORD>(names.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR;
    modal = true;
    const bool accepted = GetOpenFileNameW(&ofn) != 0;
    modal = false;
    if (!accepted) return;
    const std::wstring first(names.data()); const wchar_t* next = names.data() + first.size() + 1;
    std::vector<std::wstring> paths;
    if (!*next) paths.push_back(first);
    else while (*next) { paths.push_back(first + L"\\" + next); next += wcslen(next) + 1; }
    for (const auto& path : paths) { try { add(path); } catch (const std::wstring& s) { report_error(leaf(path) + L": " + s); } }
}
void remove_selected() {
    const int i = selected_library(); if (i < 0) return;
    const auto name = leaf(libraries[i].dll.path); ListView_DeleteItem(queueList, i); libraries.erase(libraries.begin() + i);
    if (!libraries.empty()) { const int next = std::min(i, static_cast<int>(libraries.size()) - 1); ListView_SetItemState(queueList, next, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); }
    log(L"Removed " + name + L" from the queue."); set_status(L"Queue updated"); update_actions();
}
void post(UiEvent event) {
    auto p = std::make_unique<UiEvent>(std::move(event)); auto raw = p.release();
    if (!PostMessageW(window, EVENT, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
}
void start(bool dryRun, bool inspectOnly = false) {
    if (testMode) throw std::wstring(L"UI tests cannot load code or inspect live processes.");
    if (busy) return;
    const auto selectedTarget = target;
    std::vector<std::pair<int, std::wstring>> paths;
    if (inspectOnly) {
        const auto index = selected_library(); if (index < 0) return; paths.emplace_back(index, libraries[index].dll.path);
    } else {
        if (!target.pid || target.machine != odl::own_machine()) throw std::wstring(L"Select a process with the same architecture as this loader.");
        for (int n = 0; n < static_cast<int>(libraries.size()); ++n) if (ListView_GetCheckState(queueList, n)) paths.emplace_back(n, libraries[n].dll.path);
        if (paths.empty()) throw std::wstring(L"Check at least one DLL in the queue.");
    }
    if (worker.joinable()) worker.join();
    busy = true; update_actions(); set_status(inspectOnly ? L"Inspecting" : dryRun ? L"Checking" : L"Loading", ui::accent);
    log(inspectOnly ? L"Inspecting the selected DLL without executing it." : (dryRun ? L"Checking for " : L"Loading into ") + selectedTarget.path + L" [PID " + std::to_wstring(selectedTarget.pid) + L"]");
    try {
        worker = std::thread([selectedTarget, paths = std::move(paths), dryRun, inspectOnly] {
            bool success = true;
            try {
                for (const auto& [index, path] : paths) {
                    if (inspectOnly) { post({index, odl::diagnose(selectedTarget, path), L"Inspected"}); continue; }
                    post({index, {}, dryRun ? L"Checking..." : L"Loading..."});
                    auto result = odl::load(selectedTarget, path, dryRun);
                    const auto label = result.state == odl::State::loaded ? (dryRun ? L"Checked" : L"Loaded") : result.state == odl::State::already_loaded ? L"Already loaded" : result.state == odl::State::pending ? L"Pending" : L"Failed";
                    post({index, result.message, label, result.state});
                    if (!result.ok()) { success = false; break; }
                }
            } catch (const std::wstring& s) { post({-1, s, {}, odl::State::failed}); success = false; }
              catch (...) { post({-1, L"Unexpected error. The operation stopped.", {}, odl::State::failed}); success = false; }
            post({-1, success ? L"Operation finished." : L"Stopped. Later DLLs were not attempted. Inspect the selected file for more information.", {}, odl::State::already_loaded, true, success});
        });
    } catch (...) { busy = false; update_actions(); throw; }
}
void start_host_command(odl::HostCommand command) {
    if (testMode) throw std::wstring(L"UI tests do not send commands to live hosts.");
    if (busy || !cooperative || !target.pid) return;
    if (worker.joinable()) worker.join();
    const auto selected = target;
    busy = true; update_actions(); set_status(command == odl::HostCommand::reload ? L"Reloading plugin" : L"Unloading plugin", ui::accent);
    log(L"Asking the cooperating host to " + std::wstring(command == odl::HostCommand::reload ? L"reload" : L"stop and unload") + L" its own plugin.");
    try {
        worker = std::thread([selected, command] {
            UiEvent event; event.done = true;
            try {
                const auto result = odl::host_command(selected, command, 5000);
                event.message = result.message; event.success = result.available && result.ok;
                event.managedActive = result.available ? (result.active ? 1 : 0) : -2;
            } catch (...) { event.message = L"The cooperative host request failed."; event.managedActive = -2; }
            post(std::move(event));
        });
    } catch (...) { busy = false; update_actions(); throw; }
}
HWND make(const wchar_t* klass, const wchar_t* label, DWORD style, int id) {
    auto h = CreateWindowExW(0, klass, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (!h) throw std::wstring(L"Could not create a window control.");
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ui::body), TRUE); return h;
}
void make_button(int id, const wchar_t* label) {
    auto h = make(L"BUTTON", label, BS_OWNERDRAW | WS_TABSTOP, id); SetWindowSubclass(h, ui::button_events, 1, 0);
}
void apply_fonts() {
    for (auto h : {search, compatible, processList, queueList, logBox}) if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(h == logBox ? ui::mono : ui::body), TRUE);
    SendMessageW(GetDlgItem(window, EMPTY_TITLE), WM_SETFONT, reinterpret_cast<WPARAM>(ui::strong), TRUE);
    SendMessageW(GetDlgItem(window, EMPTY_HINT), WM_SETFONT, reinterpret_cast<WPARAM>(ui::caption), TRUE);
    if (rowImages) { ListView_SetImageList(processList, nullptr, LVSIL_SMALL); ListView_SetImageList(queueList, nullptr, LVSIL_SMALL); ImageList_Destroy(rowImages); }
    rowImages = ImageList_Create(1, ui::px(30), ILC_COLOR32, 1, 1);
    ListView_SetImageList(processList, rowImages, LVSIL_SMALL); ListView_SetImageList(queueList, rowImages, LVSIL_SMALL);
}
void layout(int width, int height) {
    clientWidth = MulDiv(width, 96, static_cast<int>(ui::dpi)); clientHeight = MulDiv(height, 96, static_cast<int>(ui::dpi));
    const int x = 356, right = clientWidth - 32, full = right - x;
    queueBottom = clientHeight - 286;
    ui::move(search, 24, 182, 194, 20); ui::move(GetDlgItem(window, REFRESH), 228, 172, 72, 36);
    ui::move(compatible, 24, 222, 276, 26); ui::move(processList, 16, 286, 292, clientHeight - 346);
    ListView_SetColumnWidth(processList, 0, ui::px(172)); ListView_SetColumnWidth(processList, 1, ui::px(66)); ListView_SetColumnWidth(processList, 2, ui::px(46));
    ui::move(GetDlgItem(window, ADD), right - 132, 236, 110, 32);
    ui::move(GetDlgItem(window, RELOAD), right - 206, 140, 88, 32);
    ui::move(GetDlgItem(window, UNLOAD), right - 108, 140, 88, 32);
    ui::move(queueList, x + 16, 282, full - 32, queueBottom - 372);
    const int centerY = 282 + (queueBottom - 372) / 2;
    ui::move(GetDlgItem(window, EMPTY_TITLE), x + 28, centerY - 20, full - 56, 24);
    ui::move(GetDlgItem(window, EMPTY_HINT), x + 28, centerY + 12, full - 56, 22);
    ListView_SetColumnWidth(queueList, 0, ui::px(full - 264)); ListView_SetColumnWidth(queueList, 1, ui::px(80)); ListView_SetColumnWidth(queueList, 2, ui::px(126));
    const int actions = queueBottom + 16;
    ui::move(GetDlgItem(window, REMOVE), x, actions, 92, 40); ui::move(GetDlgItem(window, INSPECT), x + 104, actions, 108, 40);
    ui::move(GetDlgItem(window, CHECK), right - 266, actions, 126, 40); ui::move(GetDlgItem(window, LOAD), right - 128, actions, 128, 40);
    const int activityY = queueBottom + 78;
    ui::move(GetDlgItem(window, COPY), right - 108, activityY + 10, 88, 28);
    ui::move(logBox, x + 20, activityY + 48, full - 40, clientHeight - activityY - 92);
    InvalidateRect(window, nullptr, TRUE);
}
void paint(HDC dc) {
    const int right = clientWidth - 32, full = right - 356;
    ui::fill(dc, ui::rect(0, 0, clientWidth, clientHeight), ui::background); ui::fill(dc, ui::rect(0, 0, 324, clientHeight), ui::sidebar);
    ui::card(dc, ui::rect(24, 30, 36, 36), ui::accent, ui::accent, 9);
    ui::text(dc, L"DLL", ui::rect(24, 30, 36, 36), ui::caption, ui::paper, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    ui::text(dc, L"Open DLL Loader", ui::rect(72, 30, 230, 36), ui::brand, ui::paper);
    ui::text(dc, L"Native libraries. Clear controls.", ui::rect(24, 80, 276, 24), ui::caption, RGB(147, 167, 193));
    ui::text(dc, L"PROCESSES", ui::rect(24, 126, 200, 20), ui::strong, RGB(211, 224, 241));
    ui::card(dc, ui::rect(23, 171, 196, 38), ui::field, GetFocus() == search ? ui::accent : ui::field, 8);
    ui::text(dc, std::to_wstring(visibleProcesses.size()) + L" processes shown", ui::rect(24, 256, 276, 20), ui::caption, RGB(147, 167, 193));
    if (visibleProcesses.empty()) ui::text(dc, L"No matching processes", ui::rect(24, 302, 276, 40), ui::body, RGB(147, 167, 193));
    ui::text(dc, L"v0.3.0 / " + odl::machine_name(odl::own_machine()) + L"   ·   Live refresh: 2s   ·   F5", ui::rect(24, clientHeight - 42, 280, 24), ui::caption, RGB(147, 167, 193));
    ui::text(dc, L"Load libraries", ui::rect(356, 30, full - 160, 42), ui::title, ui::ink);
    ui::text(dc, L"Choose a process. Review your DLLs. Load when ready.", ui::rect(356, 78, full, 24), ui::body, ui::muted);
    const auto pillBg = statusColor == ui::red ? RGB(255, 235, 238) : statusColor == ui::accent ? RGB(229, 240, 255) : RGB(225, 244, 237);
    ui::card(dc, ui::rect(right - 154, 35, 154, 30), pillBg, pillBg, 20);
    ui::text(dc, statusText, ui::rect(right - 148, 35, 142, 30), ui::caption, statusColor, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    ui::card(dc, ui::rect(356, 120, full, 84));
    ui::text(dc, cooperative ? L"TARGET PROCESS  /  COOPERATIVE HOST" : L"TARGET PROCESS", ui::rect(378, 130, full - 260, 18), ui::caption, ui::muted);
    ui::text(dc, target.pid ? target.name + L"  ·  PID " + std::to_wstring(target.pid) + L"  ·  " + odl::machine_name(target.machine) : L"Select a process from the sidebar", ui::rect(378, 151, full - 260, 24), ui::strong, ui::ink);
    ui::text(dc, target.pid ? target.path : L"Search by application name, PID, or executable path.", ui::rect(378, 177, full - 44, 18), ui::caption, ui::muted, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS);
    ui::card(dc, ui::rect(356, 220, full, queueBottom - 220));
    ui::text(dc, L"Libraries", ui::rect(378, 238, 130, 30), ui::strong, ui::ink);
    ui::text(dc, libraries.empty() ? L"No libraries added" : std::to_wstring(checked_count()) + L" of " + std::to_wstring(libraries.size()) + L" checked", ui::rect(476, 239, full - 280, 28), ui::caption, ui::muted);
    const int detailY = queueBottom - 74;
    ui::fill(dc, ui::rect(378, detailY - 10, full - 44, 1), ui::line);
    const int selected = selected_library();
    if (selected >= 0 && static_cast<size_t>(selected) < libraries.size()) {
        const auto& dll = libraries[selected].dll;
        ui::text(dc, dll.path, ui::rect(378, detailY, full - 44, 22), ui::body, ui::ink, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS);
        ui::text(dc, L"SHA256  " + dll.sha256, ui::rect(378, detailY + 28, full - 44, 22), ui::mono, ui::muted);
    } else {
        ui::text(dc, L"Select a library to see its path and checksum.", ui::rect(378, detailY + 4, full - 44, 44), ui::body, ui::muted);
    }
    const int activityY = queueBottom + 78;
    ui::card(dc, ui::rect(356, activityY, full, clientHeight - activityY - 28));
    ui::text(dc, L"Activity", ui::rect(378, activityY + 10, 180, 28), ui::strong, ui::ink);
    ui::text(dc, L"Ctrl+O  Add DLLs       Ctrl+F  Find a process", ui::rect(356, clientHeight - 23, full, 20), ui::caption, ui::muted);
}
void initialize() {
    ui::create_fonts(GetDpiForWindow(window));
    search = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, SEARCH); SetWindowTheme(search, L"", L"");
    SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Find a process...")); SendMessageW(search, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(ui::px(10), ui::px(8)));
    compatible = make(L"BUTTON", (L"Show " + odl::machine_name(odl::own_machine()) + L" processes only").c_str(), BS_AUTOCHECKBOX | WS_TABSTOP, COMPATIBLE);
    SendMessageW(compatible, BM_SETCHECK, BST_CHECKED, 0); SetWindowTheme(compatible, L"", L"");
    processList = make(WC_LISTVIEWW, L"Processes", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER | LVS_SHAREIMAGELISTS | WS_TABSTOP, PROCESSES);
    ListView_SetExtendedListViewStyle(processList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
    SetWindowTheme(processList, L"", L""); ListView_SetBkColor(processList, ui::sidebar); ListView_SetTextBkColor(processList, ui::sidebar); ListView_SetTextColor(processList, RGB(221, 232, 246));
    column(processList, 0, L"Application", 172); column(processList, 1, L"PID", 66); column(processList, 2, L"Type", 46);
    queueList = make(WC_LISTVIEWW, L"DLL queue", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS | WS_TABSTOP, QUEUE);
    ListView_SetExtendedListViewStyle(queueList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
    SetWindowTheme(queueList, L"Explorer", nullptr); ListView_SetBkColor(queueList, ui::paper); ListView_SetTextBkColor(queueList, ui::paper); ListView_SetTextColor(queueList, ui::ink);
    column(queueList, 0, L"Library", 320); column(queueList, 1, L"Type", 80); column(queueList, 2, L"Status", 126);
    auto emptyTitle = make(L"STATIC", L"Drop DLL files here", SS_CENTER, EMPTY_TITLE); SendMessageW(emptyTitle, WM_SETFONT, reinterpret_cast<WPARAM>(ui::strong), TRUE);
    auto emptyHint = make(L"STATIC", L"Or choose Add DLLs to build your queue.", SS_CENTER, EMPTY_HINT); SendMessageW(emptyHint, WM_SETFONT, reinterpret_cast<WPARAM>(ui::caption), TRUE);
    for (const auto& pair : {std::pair{REFRESH, L"Refresh"}, {ADD, L"+ Add DLLs"}, {REMOVE, L"Remove"}, {INSPECT, L"Inspect"}, {CHECK, L"Check only"}, {LOAD, L"Load checked"}, {COPY, L"Copy log"}, {RELOAD, L"Reload"}, {UNLOAD, L"Unload"}}) make_button(pair.first, pair.second);
    auto tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    for (int id : {RELOAD, UNLOAD}) {
        TOOLINFOW tip{}; tip.cbSize = sizeof(tip); tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS; tip.hwnd = window; tip.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(window, id));
        tip.lpszText = const_cast<LPWSTR>(L"Available for cooperating plugin hosts. Ordinary injected DLLs cannot be forcibly unloaded."); SendMessageW(tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    logBox = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 205); SendMessageW(logBox, EM_SETLIMITTEXT, 1000000, 0);
    apply_fonts();
    if (testMode) {
        const auto arch = odl::own_machine();
        allProcesses = {{4242, 1, arch, L"SandboxHost.exe", L"C:\\Examples\\SandboxHost.exe"}, {9016, 2, arch, L"PluginWorkbench.exe", L"C:\\Examples\\PluginWorkbench.exe"}, {1234, 3, static_cast<USHORT>(arch == IMAGE_FILE_MACHINE_AMD64 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64), L"OtherArchitecture.exe", L"C:\\Examples\\OtherArchitecture.exe"}};
    }
    refresh(); log(L"Ready. Choose a process and add the libraries you want to load."); set_status(L"Ready");
    if (!testMode) SetTimer(window, REFRESH_TIMER, 2000, nullptr);
}
LRESULT notify(NMHDR* hdr) {
    if (hdr->idFrom == PROCESSES && hdr->code == LVN_ITEMCHANGED) {
        auto changed = reinterpret_cast<NMLISTVIEW*>(hdr);
        if ((changed->uChanged & LVIF_STATE) && ((changed->uOldState ^ changed->uNewState) & LVIS_SELECTED)) choose_process();
        return 0;
    }
    if (hdr->idFrom == QUEUE && hdr->code == LVN_ITEMCHANGED) { update_actions(); return 0; }
    if ((hdr->idFrom == QUEUE || hdr->idFrom == PROCESSES) && hdr->code == LVN_GETINFOTIPW) {
        auto info = reinterpret_cast<NMLVGETINFOTIPW*>(hdr);
        std::wstring tip;
        if (info->iItem >= 0 && hdr->idFrom == QUEUE && static_cast<size_t>(info->iItem) < libraries.size()) tip = libraries[info->iItem].dll.path;
        if (info->iItem >= 0 && hdr->idFrom == PROCESSES && static_cast<size_t>(info->iItem) < visibleProcesses.size()) tip = visibleProcesses[info->iItem].path;
        if (!tip.empty()) wcsncpy_s(info->pszText, info->cchTextMax, tip.c_str(), _TRUNCATE);
    }
    if (hdr->idFrom == QUEUE && hdr->code == LVN_KEYDOWN) {
        if (reinterpret_cast<NMLVKEYDOWN*>(hdr)->wVKey == VK_DELETE && !busy) remove_selected();
    }
    if (hdr->idFrom == QUEUE && hdr->code == LVN_GETEMPTYMARKUP) {
        auto empty = reinterpret_cast<NMLVEMPTYMARKUP*>(hdr); empty->dwFlags = EMF_CENTERED;
        wcscpy_s(empty->szMarkup, L"Drop DLL files here\n\nOr choose Add DLLs to build your queue."); return TRUE;
    }
    if (hdr->idFrom == QUEUE && hdr->code == NM_CUSTOMDRAW) {
        auto draw = reinterpret_cast<NMLVCUSTOMDRAW*>(hdr);
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
        if (draw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM) && draw->iSubItem == 2 && draw->nmcd.dwItemSpec < libraries.size()) {
            const auto& row = libraries[draw->nmcd.dwItemSpec];
            draw->clrText = row.state == odl::State::failed ? ui::red : row.status == L"Loaded" || row.status == L"Checked" ? ui::green : ui::muted;
            return CDRF_NEWFONT;
        }
    }
    return 0;
}
LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    try {
        switch (message) {
        case WM_CREATE: window = hwnd; initialize(); return 0;
        case WM_SIZE: if (search) layout(LOWORD(l), HIWORD(l)); return 0;
        case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {ui::px(1080), ui::px(800)}; return 0;
        case WM_DPICHANGED: {
            ui::create_fonts(HIWORD(w)); apply_fonts(); const auto r = reinterpret_cast<RECT*>(l);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT ps{}; auto dc = BeginPaint(hwnd, &ps); paint(dc); EndPaint(hwnd, &ps); return 0; }
        case WM_PRINTCLIENT: paint(reinterpret_cast<HDC>(w)); return 0;
        case WM_DRAWITEM: ui::button(*reinterpret_cast<DRAWITEMSTRUCT*>(l), w == LOAD, w == REFRESH); return TRUE;
        case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
            auto dc = reinterpret_cast<HDC>(w); const auto child = reinterpret_cast<HWND>(l);
            const bool dark = child == search || child == compatible;
            SetTextColor(dc, dark ? RGB(214, 226, 243) : GetDlgCtrlID(child) == EMPTY_HINT ? ui::muted : ui::ink); SetBkColor(dc, child == search ? ui::field : dark ? ui::sidebar : ui::paper);
            return reinterpret_cast<LRESULT>(child == search ? ui::fieldBrush : dark ? ui::sidebarBrush : ui::paperBrush);
        }
        case WM_NOTIFY: return notify(reinterpret_cast<NMHDR*>(l));
        case WM_TIMER: if (w == REFRESH_TIMER) request_refresh(); return 0;
        case SNAPSHOT: {
            std::unique_ptr<Snapshot> snapshot(reinterpret_cast<Snapshot*>(l));
            if (refreshWorker.joinable()) refreshWorker.join(); refreshing = false;
            if (!busy) receive_snapshot(std::move(*snapshot)); return 0;
        }
        case WM_COMMAND:
            if (LOWORD(w) == SEARCH && (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS)) { InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (LOWORD(w) == SEARCH && HIWORD(w) == EN_CHANGE && !busy) { filter_processes(); return 0; }
            if (busy && LOWORD(w) != COPY) return 0;
            switch (LOWORD(w)) {
            case COMPATIBLE: filter_processes(); break;
            case REFRESH: request_refresh(true); break;
            case ADD: browse(); break;
            case REMOVE: remove_selected(); break;
            case INSPECT: start(true, true); break;
            case CHECK: start(true); break;
            case LOAD: start(false); break;
            case COPY: copy_log(); break;
            case RELOAD: start_host_command(odl::HostCommand::reload); break;
            case UNLOAD: start_host_command(odl::HostCommand::unload); break;
            } return 0;
        case WM_DROPFILES: {
            auto drop = reinterpret_cast<HDROP>(w); struct Guard { HDROP h; ~Guard() { DragFinish(h); } } guard{drop};
            if (busy) return 0;
            const auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                std::vector<wchar_t> path(static_cast<size_t>(DragQueryFileW(drop, i, nullptr, 0)) + 1);
                DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size()));
                try { add(path.data()); } catch (const std::wstring& s) { report_error(s); }
            } return 0;
        }
        case EVENT: {
            std::unique_ptr<UiEvent> event(reinterpret_cast<UiEvent*>(l));
            if (event->row >= 0 && static_cast<size_t>(event->row) < libraries.size()) {
                libraries[event->row].status = event->label; libraries[event->row].state = event->state; cell(queueList, event->row, 2, event->label);
            }
            if (!event->message.empty()) log(event->message);
            if (event->managedActive >= 0) hostActive = event->managedActive != 0;
            else if (event->managedActive == -2) cooperative = hostActive = false;
            if (event->done) { if (worker.joinable()) worker.join(); busy = false; update_actions(); set_status(event->success ? L"Completed" : L"Needs attention", event->success ? ui::green : ui::red); if (!testMode) request_refresh(); }
            return 0;
        }
        case WM_CLOSE:
            if (busy) { set_status(L"Operation in progress", ui::accent); return 0; }
            KillTimer(hwnd, REFRESH_TIMER);
            if (refreshWorker.joinable()) refreshWorker.join();
            { MSG pending{}; while (PeekMessageW(&pending, hwnd, SNAPSHOT, SNAPSHOT, PM_REMOVE)) delete reinterpret_cast<Snapshot*>(pending.lParam); }
            DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
    } catch (const std::wstring& error) { if (logBox) report_error(error); else return -1; }
      catch (...) { if (logBox) report_error(L"Unexpected error. The operation could not complete."); else return -1; }
    return DefWindowProcW(hwnd, message, w, l);
}

bool render_png(HWND hwnd, const std::filesystem::path& file) {
    RECT r{}; GetClientRect(hwnd, &r); auto screen = GetDC(hwnd); auto dc = CreateCompatibleDC(screen);
    auto bitmap = CreateCompatibleBitmap(screen, r.right, r.bottom); auto old = SelectObject(dc, bitmap);
    // Render the client and each direct child at client coordinates. Asking a
    // framed top-level window to WM_PRINT its children offsets them by the
    // invisible non-client frame on some Windows versions.
    paint(dc);
    for (auto child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT bounds{}; GetWindowRect(child, &bounds); MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&bounds), 2);
        const int saved = SaveDC(dc); SetViewportOrgEx(dc, bounds.left, bounds.top, nullptr);
        IntersectClipRect(dc, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        RestoreDC(dc, saved);
    }
    SelectObject(dc, old); DeleteDC(dc); ReleaseDC(hwnd, screen);
    bool ok{};
    {
        Gdiplus::Bitmap image(bitmap, nullptr); UINT count{}, bytes{}; Gdiplus::GetImageEncodersSize(&count, &bytes);
        std::vector<unsigned char> memory(bytes); auto encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(memory.data());
        if (Gdiplus::GetImageEncoders(count, bytes, encoders) == Gdiplus::Ok) {
            for (UINT i = 0; i < count; ++i) if (wcscmp(encoders[i].MimeType, L"image/png") == 0) { ok = image.Save(file.c_str(), &encoders[i].Clsid, nullptr) == Gdiplus::Ok; break; }
        }
    }
    DeleteObject(bitmap); return ok;
}
int ui_test(HWND hwnd, const std::filesystem::path& output) {
    unsigned checks{};
    auto require = [&](bool ok, const std::wstring& detail = L"") { if (!ok) throw std::wstring(L"UI test failed at check ") + std::to_wstring(checks + 1) + L" " + detail; ++checks; };
    require(visibleProcesses.size() == 2);
    SetWindowTextW(search, L"4242"); require(visibleProcesses.size() == 1);
    ListView_SetItemState(processList, 0, LVIS_SELECTED, LVIS_SELECTED); require(target.pid == 4242);
    SetWindowTextW(search, L"not-a-process"); require(visibleProcesses.empty() && !target.pid);
    SetWindowTextW(search, L""); require(visibleProcesses.size() == 2);
    SendMessageW(compatible, BM_SETCHECK, BST_UNCHECKED, 0); filter_processes(); require(visibleProcesses.size() == 3);
    SendMessageW(compatible, BM_SETCHECK, BST_CHECKED, 0); filter_processes();
    SetWindowTextW(search, L"sandbox"); ListView_SetItemState(processList, 0, LVIS_SELECTED, LVIS_SELECTED); require(target.pid == 4242);
    SetWindowTextW(search, L""); require(target.pid == 4242);
    require(!IsWindowEnabled(GetDlgItem(window, RELOAD)) && !IsWindowEnabled(GetDlgItem(window, UNLOAD)));
    const auto preserved = target;
    SetWindowTextW(search, L"sandbox");
    request_refresh();
    const auto deadline = GetTickCount64() + 2000;
    while (refreshing && GetTickCount64() < deadline) {
        MSG message{}; while (PeekMessageW(&message, hwnd, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(1);
    }
    require(!refreshing && target.pid == preserved.pid && value(search) == L"sandbox");
    Snapshot changed; changed.processes = allProcesses; changed.selected = target;
    changed.processes.push_back({7777, 4, odl::own_machine(), L"NewSandbox.exe", L"C:\\Examples\\NewSandbox.exe"});
    receive_snapshot(std::move(changed));
    require(target.pid == preserved.pid && visibleProcesses.size() == 2);
    Snapshot disappeared; disappeared.selected = target;
    for (const auto& process : allProcesses) if (process.pid != target.pid) disappeared.processes.push_back(process);
    receive_snapshot(std::move(disappeared));
    require(!target.pid && !IsWindowEnabled(GetDlgItem(window, LOAD)));
    allProcesses.push_back(preserved); filter_processes();
    for (int i = 0; i < static_cast<int>(visibleProcesses.size()); ++i) if (visibleProcesses[i].pid == preserved.pid) ListView_SetItemState(processList, i, LVIS_SELECTED, LVIS_SELECTED);
    SetWindowTextW(search, L"");
    require(!IsWindowEnabled(GetDlgItem(window, LOAD)));
    append_library({L"C:\\Examples\\Plugins\\ExamplePlugin.dll", std::wstring(64, L'a'), odl::own_machine(), {L"KERNEL32.dll"}});
    append_library({L"C:\\Examples\\Plugins\\DiagnosticsOverlay.dll", std::wstring(64, L'b'), odl::own_machine(), {L"USER32.dll"}});
    require(libraries.size() == 2 && checked_count() == 2 && IsWindowEnabled(GetDlgItem(window, LOAD)));
    Snapshot managed; managed.processes = allProcesses; managed.selected = target; managed.host.available = managed.host.ok = managed.host.active = true;
    receive_snapshot(std::move(managed)); require(IsWindowEnabled(GetDlgItem(window, RELOAD)) && IsWindowEnabled(GetDlgItem(window, UNLOAD)));
    ListView_SetCheckState(queueList, 0, FALSE); ListView_SetCheckState(queueList, 1, FALSE); require(!IsWindowEnabled(GetDlgItem(window, LOAD)));
    ListView_SetCheckState(queueList, 0, TRUE); require(IsWindowEnabled(GetDlgItem(window, LOAD)));
    remove_selected(); require(libraries.size() == 1);
    append_library({L"C:\\Examples\\Plugins\\DiagnosticsOverlay.dll", std::wstring(64, L'b'), odl::own_machine(), {L"USER32.dll"}});
    busy = true; update_actions(); require(!IsWindowEnabled(GetDlgItem(window, ADD)) && !IsWindowEnabled(search)); busy = false; update_actions();
    for (auto size : {std::pair{1080, 800}, std::pair{1220, 860}}) {
        SetWindowPos(hwnd, nullptr, 0, 0, ui::px(size.first), ui::px(size.second), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT client{}; GetClientRect(hwnd, &client);
        for (auto h : {search, processList, queueList, logBox, GetDlgItem(window, LOAD)}) {
            RECT child{}; GetWindowRect(h, &child); MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&child), 2);
            require(child.left >= 0 && child.top >= 0 && child.right <= client.right && child.bottom <= client.bottom && child.bottom > child.top,
                L"control=" + std::to_wstring(GetDlgCtrlID(h)) + L" child=" + std::to_wstring(child.left) + L"," + std::to_wstring(child.top) + L"," + std::to_wstring(child.right) + L"," + std::to_wstring(child.bottom) +
                L" client=" + std::to_wstring(client.right) + L"," + std::to_wstring(client.bottom) + L" dpi=" + std::to_wstring(ui::dpi));
        }
        if (size.first == 1080) require(render_png(hwnd, output.wstring() + L".compact.png"));
    }
    libraries[0].status = L"Checked"; cell(queueList, 0, 2, L"Checked"); set_status(L"Ready");
    activity = L"00:00:00  UI preview uses sample data. No processes were modified.\r\n\r\n00:00:01  ExamplePlugin.dll passed file validation.\r\n\r\n00:00:02  Two libraries queued. Ready when you are."; SetWindowTextW(logBox, activity.c_str());
    require(render_png(hwnd, output));
    const auto previewActivity = activity; activity.clear();
    SendMessageW(hwnd, EVENT, 0, reinterpret_cast<LPARAM>(new UiEvent{1, L"Example failure: the expected DLL was not found in the target module list. Inspect can report file architecture, direct imports, and available process policies.", L"Failed", odl::State::failed}));
    SendMessageW(hwnd, EVENT, 0, reinterpret_cast<LPARAM>(new UiEvent{-1, L"UI test result only. No DLL was executed.", {}, odl::State::failed, true, false}));
    require(statusText == L"Needs attention" && libraries[1].status == L"Failed" && IsWindowEnabled(GetDlgItem(window, INSPECT)));
    require(render_png(hwnd, output.wstring() + L".error.png"));
    libraries[1].status = L"Ready"; libraries[1].state = odl::State::already_loaded; cell(queueList, 1, 2, L"Ready"); set_status(L"Ready");
    activity = previewActivity; SetWindowTextW(logBox, activity.c_str());
    const auto selectedDpi = ui::dpi;
    ui::create_fonts(144); apply_fonts();
    SetWindowPos(hwnd, nullptr, 0, 0, ui::px(1220), ui::px(860), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    require(render_png(hwnd, output.wstring() + L".150.png"));
    ui::create_fonts(selectedDpi); apply_fonts();
    SetWindowPos(hwnd, nullptr, 0, 0, ui::px(1220), ui::px(860), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    while (!libraries.empty()) { ListView_SetItemState(queueList, 0, LVIS_SELECTED, LVIS_SELECTED); remove_selected(); }
    target = {}; ListView_SetItemState(processList, -1, 0, LVIS_SELECTED); activity = L"Ready. Choose a process and add your libraries."; SetWindowTextW(logBox, activity.c_str()); set_status(L"Ready");
    require(render_png(hwnd, output.wstring() + L".empty.png"));
    std::ofstream report(output.wstring() + L".json"); report << "{\"checks_passed\":" << checks << ",\"sample_data_only\":true,\"target_memory_modified\":false}";
    return 0;
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc{}; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc); testMode = argc == 3 && std::wstring(argv[1]) == L"--ui-test";
    const std::filesystem::path preview = testMode ? argv[2] : L""; if (argv) LocalFree(argv);
    Gdiplus::GdiplusStartupInput input; ULONG_PTR graphics{}; if (Gdiplus::GdiplusStartup(&graphics, &input, nullptr) != Gdiplus::Ok) return 1;
    INITCOMMONCONTROLSEX init{sizeof(init), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&init);
    WNDCLASSW wc{}; wc.lpfnWndProc = procedure; wc.hInstance = instance; wc.lpszClassName = L"OpenDllLoader.Window";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION); RegisterClassW(&wc);
    ui::dpi = GetDpiForSystem();
    auto hwnd = CreateWindowExW(0, wc.lpszClassName, (L"Open DLL Loader · " + odl::machine_name(odl::own_machine())).c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, ui::px(1220), ui::px(860), nullptr, nullptr, instance, nullptr);
    int result = 1;
    if (hwnd && testMode) {
        try { result = ui_test(hwnd, preview); } catch (const std::wstring& error) {
            std::wofstream(std::filesystem::path(preview.wstring() + L".error.txt")) << error;
            const auto length = WideCharToMultiByte(CP_UTF8, 0, error.data(), static_cast<int>(error.size()), nullptr, 0, nullptr, nullptr);
            std::string message(static_cast<size_t>(length), '\0'); WideCharToMultiByte(CP_UTF8, 0, error.data(), static_cast<int>(error.size()), message.data(), length, nullptr, nullptr); std::cerr << message << '\n';
        }
        DestroyWindow(hwnd);
    } else if (hwnd) {
        ShowWindow(hwnd, show); MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            bool handled{};
            if (msg.message == WM_KEYDOWN && !busy) {
                if ((GetKeyState(VK_CONTROL) & 0x8000) && msg.wParam == 'F') { SetFocus(search); handled = true; }
                else if ((GetKeyState(VK_CONTROL) & 0x8000) && msg.wParam == 'O') { SendMessageW(hwnd, WM_COMMAND, ADD, 0); handled = true; }
                else if (msg.wParam == VK_F5) { SendMessageW(hwnd, WM_COMMAND, REFRESH, 0); handled = true; }
            }
            if (!handled && !IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        result = static_cast<int>(msg.wParam);
    }
    if (worker.joinable()) worker.join();
    if (refreshWorker.joinable()) refreshWorker.join();
    if (rowImages) ImageList_Destroy(rowImages); ui::destroy(); Gdiplus::GdiplusShutdown(graphics); return result;
}
