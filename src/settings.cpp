#include "settings.h"

#include <windowsx.h>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <vector>

static const wchar_t* kKey = L"Software\\Storm";

static DWORD ReadDword(HKEY key, const wchar_t* name, DWORD fallback)
{
    DWORD value = 0, size = sizeof(value), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, (BYTE*)&value, &size) == ERROR_SUCCESS
        && type == REG_DWORD)
        return value;
    return fallback;
}

Settings Settings::load()
{
    Settings s;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return s;

    s.quality        = (int)ReadDword(key, L"Quality", (DWORD)s.quality);
    s.frameCap       = (int)ReadDword(key, L"FrameCap", (DWORD)s.frameCap);
    s.cycleStorms    = ReadDword(key, L"CycleStorms", s.cycleStorms ? 1 : 0) != 0;
    s.idleMinutes    = (int)ReadDword(key, L"IdleMinutes", (DWORD)s.idleMinutes);
    s.respectBattery = ReadDword(key, L"RespectBattery", s.respectBattery ? 1 : 0) != 0;
    RegCloseKey(key);

    // Anything out of range is treated as absent rather than obeyed: the
    // registry is user-writable and a bad value should not brick the saver.
    s.quality  = std::min(std::max(s.quality, 0), 3);
    s.frameCap = (s.frameCap == 60) ? 60 : 30;
    s.idleMinutes = std::min(std::max(s.idleMinutes, 0), 240);
    return s;
}

void Settings::save() const
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_WRITE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return;

    auto write = [&](const wchar_t* name, DWORD value)
    {
        RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    };
    write(L"Quality", (DWORD)quality);
    write(L"FrameCap", (DWORD)frameCap);
    write(L"CycleStorms", cycleStorms ? 1u : 0u);
    write(L"IdleMinutes", (DWORD)idleMinutes);
    write(L"RespectBattery", respectBattery ? 1u : 0u);
    RegCloseKey(key);
}

// Tier 0 is what Phases 01 to 04 measured; the others give the adaptive path
// somewhere to go. The march is the expensive pass and the step count is the
// one knob that changes its cost without reallocating anything, which matters
// because the alternative - changing the resolution divisor - means recreating
// the history buffers and throwing away the temporal accumulation with them.
int Settings::marchSteps(int tier)
{
    switch (tier)
    {
        case 1:  return 192;
        case 2:  return 144;
        default: return 256;
    }
}

bool Settings::lightVolumeEveryStep(int tier)
{
    return tier < 2;
}

// ------------------------------------------------------------------- dialog
//
// Built in memory rather than as an .rc template. The controls are few and the
// layout is trivial, and an in-memory template keeps the whole dialog - text,
// sizes, behaviour - in one file next to the settings it edits.

namespace
{

enum : WORD
{
    kIdQuality = 200, kIdFrameCap, kIdCycle, kIdBattery, kIdIdle, kIdIdleLabel,
};

struct DialogState { Settings* settings; };

INT_PTR CALLBACK SettingsProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    DialogState* state = (DialogState*)GetWindowLongPtrW(dialog, GWLP_USERDATA);

    switch (message)
    {
        case WM_INITDIALOG:
        {
            SetWindowLongPtrW(dialog, GWLP_USERDATA, (LONG_PTR)lp);
            state = (DialogState*)lp;
            const Settings& s = *state->settings;

            HWND quality = GetDlgItem(dialog, kIdQuality);
            for (const wchar_t* name : { L"Automatic", L"High", L"Medium", L"Low" })
                ComboBox_AddString(quality, name);
            ComboBox_SetCurSel(quality, s.quality);

            HWND cap = GetDlgItem(dialog, kIdFrameCap);
            ComboBox_AddString(cap, L"30 frames a second");
            ComboBox_AddString(cap, L"60 frames a second");
            ComboBox_SetCurSel(cap, s.frameCap == 60 ? 1 : 0);

            HWND idle = GetDlgItem(dialog, kIdIdle);
            for (const wchar_t* name : { L"Never", L"After 15 minutes",
                                         L"After 30 minutes", L"After 60 minutes" })
                ComboBox_AddString(idle, name);
            const int idleIndex = (s.idleMinutes >= 60) ? 3
                                : (s.idleMinutes >= 30) ? 2
                                : (s.idleMinutes >= 15) ? 1 : 0;
            ComboBox_SetCurSel(idle, idleIndex);

            CheckDlgButton(dialog, kIdCycle,   s.cycleStorms    ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dialog, kIdBattery, s.respectBattery ? BST_CHECKED : BST_UNCHECKED);
            return TRUE;
        }

        case WM_COMMAND:
            if (LOWORD(wp) == IDOK && state)
            {
                Settings& s = *state->settings;
                s.quality  = ComboBox_GetCurSel(GetDlgItem(dialog, kIdQuality));
                s.frameCap = ComboBox_GetCurSel(GetDlgItem(dialog, kIdFrameCap)) == 1 ? 60 : 30;
                static const int kIdle[] = { 0, 15, 30, 60 };
                s.idleMinutes = kIdle[std::min(std::max(
                    ComboBox_GetCurSel(GetDlgItem(dialog, kIdIdle)), 0), 3)];
                s.cycleStorms    = IsDlgButtonChecked(dialog, kIdCycle) == BST_CHECKED;
                s.respectBattery = IsDlgButtonChecked(dialog, kIdBattery) == BST_CHECKED;
                s.save();
                EndDialog(dialog, 1);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) { EndDialog(dialog, 0); return TRUE; }
            return FALSE;

        case WM_CLOSE:
            EndDialog(dialog, 0);
            return TRUE;
    }
    return FALSE;
}

// A dialog template assembled at runtime. Everything is in dialog units.
struct TemplateWriter
{
    std::vector<BYTE> bytes;

    void align(size_t to) { while (bytes.size() % to) bytes.push_back(0); }
    void raw(const void* data, size_t size)
    {
        const BYTE* p = (const BYTE*)data;
        bytes.insert(bytes.end(), p, p + size);
    }
    void word(WORD v)  { raw(&v, sizeof(v)); }
    void dword(DWORD v) { raw(&v, sizeof(v)); }
    void text(const wchar_t* s)
    {
        raw(s, (std::wcslen(s) + 1) * sizeof(wchar_t));
    }

    // DLGITEMTEMPLATEEX, and the field order is not the one DLGITEMTEMPLATE
    // uses: help id first, then extended style, then style. Writing style
    // where help id belongs produces a template Windows rejects outright -
    // DialogBoxIndirectParam returns -1 and sets no error code, which is a
    // singularly unhelpful way to be told.
    void control(DWORD style, short x, short y, short cx, short cy,
                 WORD id, WORD cls, const wchar_t* caption)
    {
        align(4);
        dword(0);                      // help id
        dword(0);                      // extended style
        dword(style | WS_CHILD | WS_VISIBLE);
        word((WORD)x); word((WORD)y); word((WORD)cx); word((WORD)cy);
        word(id); word(0);             // id, as a DWORD
        word(0xFFFF); word(cls);       // predefined window class
        text(caption);
        word(0);                       // no creation data
    }
};

} // namespace

bool ShowSettingsDialog(HINSTANCE instance, HWND parent, Settings& settings)
{
    const short width = 244, height = 168;
    TemplateWriter t;

    // DLGTEMPLATEEX header.
    t.word(1); t.word(0xFFFF);                 // version, signature
    t.dword(0);                                // help id
    t.dword(0);                                // extended style
    t.dword(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    t.word(10);                                // control count - must match exactly
    t.word(0); t.word(0); t.word(width); t.word(height);
    t.word(0);                                 // no menu
    t.word(0);                                 // default class
    t.text(L"Storm");
    t.word(9);                                 // font size
    t.word(FW_NORMAL); t.bytes.push_back(0); t.bytes.push_back(DEFAULT_CHARSET);
    t.text(L"Segoe UI");

    const WORD kStatic = 0x0082, kButton = 0x0080, kCombo = 0x0085;
    const DWORD combo = CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL;

    t.control(SS_LEFT, 12, 14, 74, 10, (WORD)-1, kStatic, L"Quality");
    t.control(combo,   92, 12, 140, 90, kIdQuality, kCombo, L"");

    t.control(SS_LEFT, 12, 36, 74, 10, (WORD)-1, kStatic, L"Frame rate");
    t.control(combo,   92, 34, 140, 90, kIdFrameCap, kCombo, L"");

    t.control(SS_LEFT, 12, 58, 74, 10, kIdIdleLabel, kStatic, L"Stop drawing");
    t.control(combo,   92, 56, 140, 90, kIdIdle, kCombo, L"");

    t.control(BS_AUTOCHECKBOX | WS_TABSTOP, 12, 84, 220, 12, kIdCycle, kButton,
              L"A different storm every couple of minutes");
    t.control(BS_AUTOCHECKBOX | WS_TABSTOP, 12, 100, 220, 12, kIdBattery, kButton,
              L"Reduce quality and frame rate on battery");

    t.control(BS_DEFPUSHBUTTON | WS_TABSTOP, 128, 138, 50, 16, IDOK, kButton, L"OK");
    t.control(BS_PUSHBUTTON | WS_TABSTOP,    182, 138, 50, 16, IDCANCEL, kButton, L"Cancel");

    DialogState state{ &settings };
    const INT_PTR result = DialogBoxIndirectParamW(
        instance, (LPCDLGTEMPLATEW)t.bytes.data(), parent, SettingsProc, (LPARAM)&state);
    if (result == -1)
    {
        char message[160];
        std::snprintf(message, sizeof(message),
                      "The settings dialog could not be created (error %lu).",
                      (unsigned long)GetLastError());
        FailHard(message);
    }
    return result == 1;
}
