#Requires AutoHotkey v2.0

; Alt + J → Home
!j::Send "{Home}"

; Alt + L → End
!l::Send "{End}"

; Alt + Shift + J → Shift + Home
+!j::Send "+{Home}"

; Alt + Shift + L → Shift + End
+!l::Send "+{End}"

; Ctrl + Alt + Shift + J → Ctrl + Shift + Home
^+!j::Send "^+{Home}"

; Ctrl + Alt + Shift + L → Ctrl + Shift + End
^+!l::Send "^+{End}"

; Ctrl + Alt
^!j::Send "^{Home}"
^!l::Send "^{End}"

; Alt + Q → Alt + F4
!q::Send "!{F4}"

; Alt + S → F11
!s::Send "{F11}"

; F12 = hide/unhide desktop icons
F12::
{
    HWND := 0

    try HWND := ControlGetHwnd("SysListView321", "ahk_class Progman")

    if !HWND
        try HWND := ControlGetHwnd("SysListView321", "ahk_class WorkerW")

    if !HWND
    {
        MsgBox "Desktop icon control not found."
        return
    }

    if DllCall("IsWindowVisible", "Ptr", HWND)
        WinHide("ahk_id " HWND)
    else
        WinShow("ahk_id " HWND)
}

; ctrl+ctrl = Run cursor-spotkight exe
~Ctrl Up::
{
    static lastPress := 0

    now := A_TickCount

    if (now - lastPress < 300)
        Run(".\find-my-mouse.exe")

    lastPress := now
}

; win+ctrl+t = Run pin to top exe
#^t::Run(".\pin-to-top.exe --border-width 2")

saveScreenshotPath := EnvGet("USERPROFILE") "\Pictures\Screenshots"

if !DirExist(saveScreenshotPath)
    DirCreate(saveScreenshotPath)

; print-screen = run capture --fullscreen --save pictures/snip folder
PrintScreen::Run(Format('.\capture.exe --fullscreen --draw-toolbar --save "{}"', saveScreenshotPath))

; alt+print-screen = run capture --window --save pictures/snip folder
!PrintScreen::Run(Format('.\capture.exe --window --draw-toolbar --save "{}"', saveScreenshotPath))

; ctrl+print-screen = run capture --snip --save pictures/snip folder
^PrintScreen::Run(Format('.\capture.exe --snip --draw-toolbar --save "{}"', saveScreenshotPath))

; Win+Ctrl+C = color picker
#^c::Run("./color-picker.exe")

; Alt+i = open ip-send
!i::Run(".\ip-send.exe")

OCRSnip() {
    global saveScreenshotPath

    before := Map()

    Loop Files, saveScreenshotPath "\*.*", "F"
        before[A_LoopFileFullPath] := true

    Run(Format('.\capture.exe --snip --save "{}"', saveScreenshotPath))

    imagePath := ""

    Loop 100 {
        Sleep(100)

        Loop Files, saveScreenshotPath "\*.*", "F" {
            if !before.Has(A_LoopFileFullPath) {
                imagePath := A_LoopFileFullPath
                break
            }
        }

        if imagePath
            break
    }

    if !imagePath
        return

    outputFile := A_Temp "\ocr-output.txt"

    if FileExist(outputFile)
        FileDelete(outputFile)

    ocrExe := A_ScriptDir "\ocr.exe"

    cmd := Format(
        'cmd.exe /c ""{}" --image "{}" > "{}""',
        ocrExe,
        imagePath,
        outputFile
    )

    shell := ComObject("WScript.Shell")
    exitCode := shell.Run(cmd, 0, true)

    if exitCode != 0
        return

    if !FileExist(outputFile)
        return

    text := FileRead(outputFile, "UTF-8")
    FileDelete(outputFile)

    A_Clipboard := Trim(text)
}
; Win + Ctrl + O = snip → OCR → clipboard
#^O::OCRSnip()

!Tab::Run(".\window-switcher.exe -l center -s recent")