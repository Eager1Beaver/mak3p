#include "gui/input_picker.hpp"
#include "gui/presentation.hpp"
#include "app/import.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <FL/platform.H>
#include <FL/fl_utf8.h>

#include <memory>
#include <sstream>
#include <string_view>
#include <system_error>
#else
#include <FL/Fl_Native_File_Chooser.H>
#endif

#include <stdexcept>

namespace audio_converter::gui {
namespace {

std::string input_file_filter() {
    return text("gui_files_filter", {{"extensions", audio_file_glob()}});
}

#ifdef _WIN32

struct ReleaseInterface {
    void operator()(IUnknown* interface_pointer) const noexcept { interface_pointer->Release(); }
};

void check_dialog(HRESULT result) {
    if (FAILED(result)) {
        throw std::system_error(static_cast<int>(result), std::system_category(), "Windows Open dialog");
    }
}

std::wstring wide_text(std::string_view value) {
    std::wstring result(value.size() + 1, L'\0');
    result.resize(fl_utf8towc(value.data(), static_cast<unsigned>(value.size()), result.data(),
                             static_cast<unsigned>(result.size())));
    return result;
}

std::wstring windows_pattern(std::wstring_view pattern) {
    if (pattern == L"*") return L"*.*";
    if (!pattern.starts_with(L"*.{") || !pattern.ends_with(L'}')) return std::wstring(pattern);

    pattern.remove_prefix(3);
    pattern.remove_suffix(1);
    std::wstring expanded = L"*.";
    for (const auto character : pattern) {
        if (character == L',') expanded += L";*.";
        else expanded += character;
    }
    return expanded;
}

void set_file_filters(IFileOpenDialog& dialog) {
    std::wistringstream input(wide_text(input_file_filter()));
    std::vector<std::pair<std::wstring, std::wstring>> filters;
    for (std::wstring line; std::getline(input, line);) {
        const auto tab = line.find(L'\t');
        if (tab == line.npos) throw std::runtime_error("Invalid file filter");
        filters.emplace_back(line.substr(0, tab), windows_pattern(std::wstring_view(line).substr(tab + 1)));
    }
    std::vector<COMDLG_FILTERSPEC> specs;
    specs.reserve(filters.size());
    for (const auto& [label, pattern] : filters) specs.push_back({label.c_str(), pattern.c_str()});
    check_dialog(dialog.SetFileTypes(static_cast<UINT>(specs.size()), specs.data()));
    check_dialog(dialog.SetFileTypeIndex(1));
}

#endif
} // namespace

std::vector<fs::path> select_input_paths([[maybe_unused]] Fl_Window& owner, bool folders) {
    const auto title = text(folders ? "gui_select_folders" : "gui_select_files");
    std::vector<fs::path> paths;
#ifdef _WIN32
    IFileOpenDialog* instance = nullptr;
    check_dialog(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&instance)));
    std::unique_ptr<IFileOpenDialog, ReleaseInterface> dialog(instance);

    FILEOPENDIALOGOPTIONS options{};
    check_dialog(dialog->GetOptions(&options));
    options |= FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT | FOS_NOCHANGEDIR;
    if (folders) options |= FOS_PICKFOLDERS;
    check_dialog(dialog->SetOptions(options));
    check_dialog(dialog->SetTitle(wide_text(title).c_str()));
    if (!folders) set_file_filters(*dialog);

    const auto status = dialog->Show(fl_xid(&owner));
    if (status == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    check_dialog(status);

    IShellItemArray* selection = nullptr;
    check_dialog(dialog->GetResults(&selection));
    std::unique_ptr<IShellItemArray, ReleaseInterface> items(selection);
    DWORD count = 0;
    check_dialog(items->GetCount(&count));
    paths.reserve(count);
    for (DWORD index = 0; index < count; ++index) {
        IShellItem* selected = nullptr;
        check_dialog(items->GetItemAt(index, &selected));
        std::unique_ptr<IShellItem, ReleaseInterface> item(selected);
        PWSTR native = nullptr;
        check_dialog(item->GetDisplayName(SIGDN_FILESYSPATH, &native));
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(native, CoTaskMemFree);
        paths.emplace_back(path.get());
    }
#else
    Fl_Native_File_Chooser picker(folders ? Fl_Native_File_Chooser::BROWSE_MULTI_DIRECTORY
                                         : Fl_Native_File_Chooser::BROWSE_MULTI_FILE);
    picker.title(title.c_str());
    if (!folders) picker.filter(input_file_filter().c_str());
    const int status = picker.show();
    if (status < 0) throw std::runtime_error(picker.errmsg());
    if (status > 0) return {};
    for (int index = 0; index < picker.count(); ++index) {
        paths.push_back(path_from_utf8(picker.filename(index)));
    }
#endif
    return paths;
}

} // namespace audio_converter::gui
