#include <iostream>
#include <filesystem>

#include "olcNoiseMaker.hpp"

#ifndef NDEBUG
#include <crtdbg.h>
#endif
#include "Resource.h"

#include <windowsx.h>
#include "Shlobj.h"

const int RESP_PREC = 10;
const int LPF_STRENGTH = 140;

const int audio_sample_rate = 48000;

std::vector<std::string> sounds;
std::vector<double> audio_durations;
std::vector<int> audio_samples;

int level = 0;
int response = 0;
int flastlevel = 0;
int lpflevel = 0;
bool lastbit = false;

int counter = 0;
int current_song = 0;
int last_song = 0;

std::atomic_bool play = false;
std::atomic_bool next = false;
std::atomic_bool prev = false;
std::atomic_bool loop = false;
std::atomic_bool random = false;
std::atomic_int32_t requested_track = -1;

void ctx_update(bool curbit)
{
	int target = (curbit ? 127 : -128);
	int nlevel = (level + ((response*(target - level) + (1<<(RESP_PREC-1)))>>RESP_PREC));
	if((nlevel == level) && (level != target))
		nlevel += (curbit ? 1 : -1);

	int rtarget, rdelta;
	if(curbit == lastbit)
	{
		rtarget = (1<<RESP_PREC)-1;
		rdelta = 1;
	}
	else
	{
		rtarget = 0;
		rdelta = 1;
	}

	int nresponse = response;
	if (response != rtarget)
		nresponse += (curbit == lastbit ? 1 : -1);


	if (nresponse < (2<<(RESP_PREC-8)))
		nresponse = (2<<(RESP_PREC-8));

	response = nresponse;
	lastbit = curbit;
	level = nlevel;
}

short decode_next_sample()
{
    int r = requested_track.load();
    if (r >= 0)
    {
        counter = 0;
        play.store(true);
        next.store(false);
        prev.store(false);
        current_song = r;
        requested_track.store(-1);
    }

    if (!play.load())
    {
        next.store(false);
        prev.store(false);
        return 0;
    }

	if (counter < 0 || counter >= audio_samples[current_song] || next.load())
	{
        if (!loop.load())
        {
            if (random.load())
            {
                int new_song;
                if (sounds.size() <= 2)
                {
                    new_song = sounds.size() == 1 ? current_song : ((current_song + 1) & 0x1);
                }
                else
                {
                    do
                    {
                        new_song = (int)(rand() * sounds.size() / RAND_MAX);
                    } while (new_song == current_song || new_song == last_song);
                }
                last_song = current_song;
                current_song = new_song;
            }
            else
            {
                last_song = current_song;
                current_song = (current_song == sounds.size() - 1 ? 0 : current_song + 1);
            }
        }

        counter = 0;
        level = 0;
        response = 0;
        flastlevel = 0;
        lpflevel = 0;
        lastbit = false;
        next.store(false);
	}

    if (prev.load())
    {
        if (counter < 3 * audio_sample_rate)
        {
            if (last_song != current_song)
            {
                current_song = last_song;
            }
            else
            {
                last_song = current_song;
                current_song = (current_song == 0 ? (int)(sounds.size() - 1) : current_song - 1);
            }
        }

        counter = 0;
        level = 0;
        response = 0;
        flastlevel = 0;
        lpflevel = 0;
        lastbit = false;
        prev.store(false);
    }
	
	int byte = counter >> 3;
	unsigned int data = (unsigned char)(sounds[current_song][byte]);
	data = data >> (counter & 0x07);
	
	// apply context
	bool curbit = ((data & 1u) != 0u);
	bool lastbit2 = lastbit;
	ctx_update(curbit);

	// apply noise shaping
	int blevel = (curbit == lastbit2 ? level : ((flastlevel + level + 1)>>1));
	//if (blevel > 127) blevel = 127;
	//else if (blevel < -128) blevel = -128;
	blevel = blevel & 0xff;
	if (blevel > 127) blevel -= 256;
	flastlevel = level;

	// apply low-pass filter
	lpflevel += ((LPF_STRENGTH * (blevel - lpflevel) + 0x80)>>8);

	lpflevel = lpflevel & 0xffu;
	if (lpflevel > 127) lpflevel -= 256;
	//return clamp(float(lpflevel)/256.0,-1.,1.);
	counter++;
	return lpflevel << 6;
}

void FillBlock(short *buffer, int sampleCount, double startTime)
{
	for (int i = 0; i < sampleCount; i++)
	{
		buffer[i] = decode_next_sample();
		//buffer[i] = MakeSin(i, startTime + i * (1.0/audio_sample_rate));
	}
}

std::string loadFile(const std::filesystem::path& path)
{
	std::ifstream in;
	in.open(path, std::ios::binary | std::ios::in | std::ios::ate);
	if (!in.is_open())
	{
		return std::string();
	}
	uint64_t length = in.tellg();
	in.seekg(0, in.beg);

	std::string res;
	res.resize(length);
	in.read(res.data(), length);
	in.close();
	return res;
}

WCHAR szClassName[] = L"MainClass";
WCHAR* szTitle = NULL;
HBITMAP frameBuffer = NULL;
std::atomic_bool shouldExit = false;
std::thread redrawThread;
std::vector<std::wstring> trackNames;

int resX = 0, resY = 0;
int mouseX = 0, mouseY = 0;
bool click = false, down = false;

LRESULT CALLBACK WndProc(_In_ HWND hWnd, _In_ UINT message, _In_ WPARAM wParam, _In_ LPARAM lParam);
void SetKeyState(HWND window, WPARAM wParam, bool pressed);
void SetMenuCheckState(HWND window, unsigned int id, bool checked);
void RedrawThread(HWND window, std::atomic_bool* exit);
std::wstring FormatTrackName(const std::filesystem::path& file, size_t currentTrack);
void ProcessDroppedFiles(HWND window, WPARAM wParam);
void LoadFiles(const std::vector<std::filesystem::path> files);
bool DrawButton(HDC hdc, int x, int y, int type);
void OpenFileDialog(HWND window);
void PaintWindow(HWND window);
void TryPlay(HWND window);
void TryStop(HWND window);
void TryLoop(HWND window);
void TryRandom(HWND window);

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR pCmdLine, _In_ int nCmdShow)
{
#ifdef _DEBUG
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    //_CrtSetBreakAlloc(163);
#endif
    {
        auto m_singleInstanceMutex = CreateMutexW(NULL, TRUE, L"DFPWM Reader very unique mutex variable");
        int errResult = GetLastError();
        int argc = 0;
        LPWSTR* args = CommandLineToArgvW(pCmdLine, &argc);
        if (!args)
        {
            return 1;
        }
        std::vector<std::filesystem::path> files;
        if (argc <= 0)
        {
            files.push_back(L"sounds/DebugSoundFull.dfpwm");
        }
        else
        {
            for (int i = 0; i < argc; i++)
            {
                std::filesystem::path p = args[i];
                std::string tmp = p.extension().string();
                const std::string tmp2 = std::string(".dfpwm");
                if (tmp.compare(0, tmp2.size(), tmp2) != 0)
                    continue;
                files.push_back(p);
            }
        }

        int result = LoadStringW(hInstance, IDS_APP_TITLE, (LPWSTR)&szTitle, 0);

        if (errResult == ERROR_ALREADY_EXISTS)
        {
            if (files.size() == 0)
                return 0;

            HWND existingApp = FindWindowW(0, szTitle);
            if (existingApp)
            {
                SetForegroundWindow(existingApp);

                POINT point;
                point.x = 0;
                point.y = 0;

                size_t size = 0;
                for (size_t i = 0; i < files.size(); i++)
                {
                    size += (files[i].wstring().length() + 1) * 2;
                }
                size = size + 2;

                HGLOBAL hMem = GlobalAlloc(GHND, sizeof(DROPFILES) + size);

                if (!hMem)
                {
                    GlobalFree(hMem);
                    return 0;
                }
                DROPFILES *dfiles = (DROPFILES*) GlobalLock(hMem);
                if (!dfiles)
                {
                    GlobalFree(hMem);
                    return 0;
                }

                dfiles->pFiles = sizeof(DROPFILES);
                dfiles->pt = point;
                dfiles->fNC = TRUE;
                dfiles->fWide = TRUE;
                wchar_t* ptr = reinterpret_cast<wchar_t*>(&dfiles[1]);
                for (size_t i = 0; i < files.size(); i++)
                {
                    const std::wstring& str = files[i].wstring();
                    memcpy(ptr, str.data(), str.length() * 2);
                    ptr[str.length()] = 0;
                    ptr += str.length() + 1;
                }
                *ptr = 0;
                GlobalUnlock(hMem);

                if (!PostMessageW(existingApp, WM_DROPFILES, (WPARAM)hMem, 0))
                {
                    GlobalFree(hMem);
                }

                return 0;
            }
        }

        LoadFiles(files);

        std::vector<std::wstring> devices = olcNoiseMaker::Enumerate();
        if (devices.empty())
        {
            MessageBox(NULL, L"No sound device detected?", szTitle, NULL);
            return 1;
        }
	    olcNoiseMaker sound(devices[0], audio_sample_rate, 1, 8, 512);
        if (!sound.GetState())
        {
            MessageBox(NULL, L"Could not create sound player", szTitle, NULL);
            return 1;
        }
	    // Link noise function with sound machine
	    sound.SetUserFunction(FillBlock);

        WNDCLASSEX wcex = {};
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = WndProc;
        wcex.cbClsExtra = 0;
        wcex.cbWndExtra = 0;
        wcex.hInstance = hInstance;
        wcex.hIcon = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));
        wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcex.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wcex.lpszMenuName = MAKEINTRESOURCE(IDR_MENU1);
        wcex.lpszClassName = szClassName;
        wcex.hIconSm = LoadIcon(wcex.hInstance, IDI_APPLICATION);

        if (!RegisterClassEx(&wcex))
        {
            MessageBox(NULL, L"Call to RegisterClassExW failed!", szTitle, NULL);
            return 1;
        }

        if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))
        {
            MessageBox(NULL, L"Could not set window dpi awareness !", szTitle, NULL);
        }
        RECT r;
        r.left = 0;
        r.right = 960;
        r.top = 0;
        r.bottom = 576;
        AdjustWindowRect(&r, WS_EX_OVERLAPPEDWINDOW | WS_EX_ACCEPTFILES, true);
        HWND hWnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW | WS_EX_ACCEPTFILES, szClassName, szTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, hInstance, NULL);

        ShowWindow(hWnd, nCmdShow);
        UpdateWindow(hWnd);

        //th.Init();
        redrawThread = std::thread(RedrawThread, hWnd, &shouldExit);
        LONG_PTR lExStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
        lExStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);
        //SetWindowLongPtr(hWnd, GWL_EXSTYLE, lExStyle);
        //SetWindowPos(hWnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        if (!hWnd)
        {
            MessageBox(NULL, L"Call to CreateWindow failed!", szTitle, NULL);
            return 1;
        }

        if (!sounds.empty())
        {
            play.store(true);
            SetMenuCheckState(hWnd, ID_ACTION_PLAY_PAUSE, true);
        }

        // Main message loop:
        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) && !shouldExit.load())
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        shouldExit.store(true);
        play.store(false);
        redrawThread.join();
        sound.Stop();
        return (int)msg.wParam;
    }
}

LRESULT CALLBACK WndProc(_In_ HWND hWnd, _In_ UINT message, _In_ WPARAM wParam, _In_ LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
        PaintWindow(hWnd);
        break;
    case WM_CLEAR:
        break;
    case WM_ERASEBKGND:
        return (LRESULT)1;
    case WM_DESTROY:
        if (frameBuffer)
        {
            DeleteObject(frameBuffer);
        }
        PostQuitMessage(0);
        break;
    case WM_DROPFILES :
        ProcessDroppedFiles(hWnd, wParam);
        break;
    case WM_SIZE:
    {
        int prevX = resX;
        int prevY = resY;
        resX = LOWORD(lParam);
        resY = HIWORD(lParam);
        if (resX != prevX || resY != prevY)
        {
            HBITMAP newBuf = CreateCompatibleBitmap(GetDC(hWnd), resX, resY);
            if (frameBuffer)
            {
                DeleteObject(frameBuffer);
            }
            frameBuffer = newBuf;
        }
        break;
    }
    case WM_MOUSEMOVE:
        mouseX = GET_X_LPARAM(lParam);
        mouseY = GET_Y_LPARAM(lParam);
        down = wParam & MK_LBUTTON;
        break;
    case WM_LBUTTONDOWN:
        click = true;
        down = true;
        break;
    case WM_LBUTTONUP:
        down = false;
        break;
    case WM_GETMINMAXINFO:
    {
        LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;
        lpMMI->ptMinTrackSize.x = 300;
        lpMMI->ptMinTrackSize.y = 300;
        break;
    }
    case WM_KEYDOWN:
        SetKeyState(hWnd, wParam, true);
        break;
    case WM_SYSKEYDOWN:
        return DefWindowProc(hWnd, message, wParam, lParam);
    case WM_COMMAND: 
 
        // Test for the identifier of a command item. 
        
        switch(LOWORD(wParam)) 
        { 
            case ID_FILE_OPEN: 
                OpenFileDialog(hWnd);
                break;
        
            case ID_FILE_EXIT: 
                DestroyWindow(hWnd);
                break;
        
            case ID_ACTION_PLAY_PAUSE:
                TryPlay(hWnd);
                break;

            case ID_ACTION_NEXT:
                next.store(true);
                break;

            case ID_ACTION_PREVIOUS:
                prev.store(true);
                break;

            case ID_ACTION_STOP:
                TryStop(hWnd);
                break;

            case ID_ACTION_LOOP:
                TryLoop(hWnd);
                break;

            case ID_ACTION_RANDOM:
                TryRandom(hWnd);
                break;

            default: 
                break;
        
        } 
        return 0; 
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

void PaintWindow(HWND window)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(window, &ps);
    HBRUSH white = GetSysColorBrush(COLOR_WINDOW);

    RECT rc;
    HDC hdcMem;
    HBITMAP hbmOld;
    GetClientRect(window, &rc);
    hdcMem = CreateCompatibleDC(hdc);
    hbmOld = (HBITMAP)SelectObject(hdcMem, frameBuffer);
    FillRect(hdcMem, &rc, white);

    HGDIOBJ original = NULL;
    original = SelectObject(hdcMem,GetStockObject(DC_PEN));

    int centerX = resX / 2;

    SelectObject(hdcMem, GetStockObject(DC_PEN));
    SelectObject(hdcMem, GetStockObject(DC_BRUSH));

    SetDCBrushColor(hdcMem, RGB(200,200,200));
    SetDCPenColor(hdcMem, RGB(0,0,0));
    int tmpMouseY = mouseY;
    if (mouseY >= resY - 90)
    {
        mouseY = 0;
    }

    for (int i = 0; i < trackNames.size(); i++)
    {
        RECT r = {55, 22 + i * 40, resX-50, 45 + i * 40};
        DRAWTEXTPARAMS p = {};
        p.cbSize = 20;

        SetDCBrushColor(hdcMem, current_song == i ? RGB(220,220,255) : RGB(220, 220, 220));
        RoundRect(hdcMem, 15, 12 + i * 40, resX - 20, 48 + i * 40, 5, 5);
        SetBkMode(hdcMem, TRANSPARENT);
        DrawTextExW(hdcMem, trackNames[i].data(), -1, &r, DT_LEFT, &p);

        bool current = (current_song == i) && play.load();
        if (DrawButton(hdcMem, 35, 30 + i * 40, current ? 1 : 0))
        {
            if (current)
            {
                TryPlay(window);
            }
            else
            {
                requested_track.store(i);
            }
        }
    }

    HBRUSH gray = CreateSolidBrush(RGB(80,80,80));
    mouseY = tmpMouseY;
    RECT r = {0, resY-90, resX, resY};
    FillRect(hdcMem, &r, gray);

    SetDCBrushColor(hdcMem, RGB(200, 200, 200));
    RoundRect(hdcMem, 20, resY-80, resX - 20, resY-50, 10, 10);

    SetDCBrushColor(hdcMem, RGB(100, 100, 100));
    Rectangle(hdcMem, 35, resY-67, resX - 35, resY-63);

    SetDCBrushColor(hdcMem, RGB(255, 255, 255));
    if (sounds.empty())
    {
        Ellipse(hdcMem, 35, resY-70, 45, resY-60);
    }
    else
    {
        double width = resX - 80;
        double pos = counter * 1.0f / audio_samples[current_song] * width + 40;
        Ellipse(hdcMem, (int)(pos - 5), resY-70, (int)(pos + 5), resY-60);
    }

    if (DrawButton(hdcMem, centerX - 20, resY - 25, play.load() ? 1 : 0))
    {
        TryPlay(window);
    }
    if (DrawButton(hdcMem, centerX + 20, resY - 25, 4))
    {
        TryStop(window);
    }
    if (DrawButton(hdcMem, centerX - 60, resY - 25, 3))
    {
        prev.store(true);
    }
    if (DrawButton(hdcMem, centerX + 60, resY - 25, 2))
    {
        next.store(true);
    }

    BitBlt(hdc, rc.left, rc.top, rc.right-rc.left, rc.bottom-rc.top, hdcMem, 0, 0, SRCCOPY);

    DeleteObject(gray);
    SelectObject(hdc,original);
    SelectObject(hdcMem, hbmOld);
    DeleteDC(hdcMem);
    EndPaint(window, &ps);
    click = false;
}

void SetKeyState(HWND window, WPARAM wParam, bool pressed)
{
    switch (wParam)
    {
    case VK_MEDIA_NEXT_TRACK:
        next.store(true);
        break;
    case VK_MEDIA_PREV_TRACK:
        prev.store(true);
        break;
    case VK_MEDIA_STOP:
        TryStop(window);
        break;
    case VK_MEDIA_PLAY_PAUSE:
        TryPlay(window);
        break;
    default:
        break;
    }
}

/*
 0 is play, 1 i pause, 2 is next, 3 is back, 4 is stop
 Returns true if mouse is over button
*/
bool DrawButton(HDC hdc, int x, int y, int type)
{
    float d = (float)(mouseX - x) * (mouseX - x);
    d += (mouseY - y) * (mouseY - y);
    bool hover = d <= 15*15;
    if (hover)
    {
        if (down)
        {
            SetDCBrushColor(hdc, RGB(200,200,255));
        }
        else
        {
            SetDCBrushColor(hdc, RGB(230,230,230));
        }
    }
    else
    {
        SetDCBrushColor(hdc, RGB(200, 200, 200));
    }

    Ellipse(hdc, x-15, y-15, x+15, y+15);
    SetDCBrushColor(hdc, RGB(0, 0, 0));
    switch (type)
    {
    case 0:
    {
        POINT points[] = 
        {
            POINT{x-7, y-7},
            POINT{x+8, y},
            POINT{x-7, y+7}
        };
        Polygon(hdc, points, 3);
        break;
    }
    case 1:
    {
        Rectangle(hdc, x-8, y-9, x-2, y+9);
        Rectangle(hdc, x+2, y-9, x+8, y+9);
        break;
    }
    case 2:
    {
        POINT points[] = 
        {
            POINT{x-7, y-7},
            POINT{x+1, y},
            POINT{x-7, y+7},
            POINT{x, y-7},
            POINT{x+8, y},
            POINT{x, y+7}
        };
        Polygon(hdc, points, 3);
        Polygon(hdc, points + 3, 3);
        break;
    }
    case 3:
    {
        POINT points[] = 
        {
            POINT{x+6, y-7},
            POINT{x-2, y},
            POINT{x+6, y+7},
            POINT{x-1, y-7},
            POINT{x-9, y},
            POINT{x-1, y+7}
        };
        Polygon(hdc, points, 3);
        Polygon(hdc, points + 3, 3);
        break;
    }
    case 4:
    {
        Rectangle(hdc, x-8, y-8, x+8, y+8);
        break;
    }
    default:
        break;
    }
    return hover && click;
}

void SetMenuCheckState(HWND window, unsigned int id, bool checked)
{
    auto menu = GetMenu(window);
    MENUITEMINFOW menuInfo;
    menuInfo.cbSize = sizeof(MENUITEMINFOW);
    menuInfo.fMask = MIIM_STATE;
    
    bool resultTTT = GetMenuItemInfoW(menu, id, FALSE, &menuInfo);
    menuInfo.fState = checked ? MFS_CHECKED : MFS_UNCHECKED;
    SetMenuItemInfoW(menu, id, FALSE, &menuInfo);
}

std::vector<std::wstring> parseString(std::wstring in)
{
    std::vector<std::wstring> result;
    bool wasNull = false;
    size_t start = 0;
    for (size_t i = 0; i < in.size(); i++)
    {
        wchar_t c = in[i];
        if (c == 0)
        {
            if (wasNull)
                break;
            wasNull = true;
            if (!i)
                continue;
            result.push_back(in.substr(start, i - start));
            start = i + 1;
        }
        else
        {
            wasNull = false;
        }
    }
    return result;
}

void OpenFileDialog(HWND window)
{
    size_t prevSoundCount = sounds.size();

    OPENFILENAMEW ofn;
    std::wstring filePath;
    filePath.resize(2048, 0);
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = window;
	ofn.lpstrFile = filePath.data();
	ofn.nMaxFile = (int)filePath.size();
	ofn.lpstrFilter = L"DFPWM file (*.dfpwm)\0*.dfpwm\0";
	ofn.nFilterIndex = 1;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
	if (GetOpenFileNameW(&ofn) == TRUE)
	{
        std::vector<std::filesystem::path> files;
        std::vector<std::wstring> strings = parseString(filePath);
        if (strings.size() == 0)
            return;
        if (strings.size() == 1)
        {
            files.push_back(ofn.lpstrFile);
        }
        else
        {
            std::filesystem::path base = strings[0];
            for (size_t i = 1; i < strings.size(); i++)
            {
                files.push_back(base / strings[i]);
            }
        }
        LoadFiles(files);
        if (prevSoundCount == 0 && !sounds.empty())
        {
            play.store(true);
            SetMenuCheckState(window, ID_ACTION_PLAY_PAUSE, true);
        }
	}
}

void TryPlay(HWND window)
{
    if (sounds.empty())
    {
        //MessageBoxW(window, L"No sound to play!", szTitle, MB_ICONEXCLAMATION);
        return;
    }
    play.store(!play.load());
    SetMenuCheckState(window, ID_ACTION_PLAY_PAUSE, play.load());
}

void TryStop(HWND window)
{
    play.store(false);
    counter = 0;
    last_song = current_song;
    current_song = 0;
}

void TryLoop(HWND window)
{
    loop.store(!loop.load());
    SetMenuCheckState(window, ID_ACTION_LOOP, loop.load());
}

void TryRandom(HWND window)
{
    random.store(!random.load());
    SetMenuCheckState(window, ID_ACTION_RANDOM, random.load());
}

void ProcessDroppedFiles(HWND window, WPARAM wParam)
{
    if (!wParam)
        return;
    size_t prevSoundCount = sounds.size();
    HDROP dr = (HDROP)(wParam);
    int count = DragQueryFileW(dr, 0xffffffff, NULL, 0);
    if (count <= 0)
    {
        DragFinish(dr);
        return;
    }
    std::vector<std::filesystem::path> files;
    files.reserve(count);
    for (int i = 0; i < count; i++)
    {
        int strLen = DragQueryFileW(dr, i, NULL, 0);
        if (strLen <= 0)
        {
            DragFinish(dr);
            return;
        }
        std::wstring file;
        file.resize(strLen + 1, '0');
        int res = DragQueryFileW(dr, i, file.data(), (unsigned int)(file.size()));
        std::filesystem::path p = file;
        std::string tmp = p.extension().string();
        const std::string tmp2 = std::string(".dfpwm");
        if (tmp.compare(0, tmp2.size(), tmp2) != 0)
            continue;
        files.push_back(p);
    }
    DragFinish(dr);
    LoadFiles(files);
    if (prevSoundCount == 0 && !sounds.empty())
    {
        play.store(true);
        SetMenuCheckState(window, ID_ACTION_PLAY_PAUSE, true);
    }
}

void LoadFiles(const std::vector<std::filesystem::path> files)
{
    bool wasPlay = play.load();
    play.store(false); // make sure we are not reading playing any sound right now, adding sounds to list might cause vector to be reallocated
    for (int i = 0; i < files.size(); i++)
    {
        std::string tmp = loadFile(files[i]);
        if (tmp.empty())
        {
            std::wstring mess = L"Could not load file ";
            mess += files[i].wstring();
            MessageBox(NULL, mess.c_str(), szTitle, MB_ICONWARNING);
            continue;
        }
        sounds.push_back(tmp);
        trackNames.push_back(FormatTrackName(files[i], sounds.size()));
    
        audio_samples.push_back((int)tmp.size() * 8);
        audio_durations.push_back(audio_samples.back() * 1.0 / audio_sample_rate);
    }
    play.store(wasPlay);
}

int getCharKind(wchar_t c)
{
    if (c >= 'A' && c <= 'Z') return 0;
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= '0' && c <= '9') return 2;
    return 3;
}

std::wstring FormatTrackName(const std::filesystem::path & file, size_t currentTrack)
{
    std::wstring tmp = file.stem().wstring();
    
    if (tmp.empty())
        return std::wstring(L"track_" + std::to_wstring(currentTrack));

    std::vector<std::wstring> words;
    int lastKind = getCharKind(tmp[0]);
    for (size_t i = 1; i < tmp.length(); i++)
    {
        wchar_t c = tmp[i];
        int kind = getCharKind(c);
        if (lastKind == 0 && kind == 1)
            lastKind = kind;
        if (kind != lastKind)
        {
            words.push_back(tmp.substr(0, i));
            tmp.erase(0, i);
            lastKind = kind;
            i = 0;
        }
    }
    words.push_back(tmp);
    std::wstring result;
    for (size_t i = 0; i < words.size(); i++)
    {
        std::wstring& str = words[i];
        if (getCharKind(str[0]) != 3)
        {
            if (!result.empty())
                result.push_back(L' ');
            
            if (getCharKind(str[0] < 3))
            {
                str[0] = std::toupper(str[0]);
                for (size_t i = 1; i < str.size(); i++)
                {
                    str[i] = std::tolower(str[i]);
                }
            }
            result.append(str);
        }
    }
    return result;
}

void RedrawThread(HWND window, std::atomic_bool* exitVal)
{
    while (!exitVal->load())
    {
        InvalidateRect(window, NULL, false);
        RedrawWindow(window, NULL, NULL, RDW_INTERNALPAINT);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}