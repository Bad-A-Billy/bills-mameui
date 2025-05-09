// For licensing and usage information, read docs/release/winui_license.txt
// MASTER
//****************************************************************************

 /***************************************************************************

  mui_audit.c

  Audit dialog

***************************************************************************/

// standard windows headers
#include <windows.h>
#include <windowsx.h>

// standard C headers
#include <tchar.h>

// MAME/MAMEUI headers
#include "winui.h"
#include "winutf8.h"
#include "audit.h"
#include "resource.h"
#include "emu_opts.h"
#include "mui_opts.h"
#include "mui_util.h"
#include "properties.h"
#include <richedit.h>


#ifdef _MSC_VER
#define vsnprintf _vsnprintf
#endif

/***************************************************************************
    function prototypes
 ***************************************************************************/

static DWORD WINAPI AuditThreadProc(LPVOID hDlg);
static INT_PTR CALLBACK AuditWindowProc(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam);
static void ProcessNextRom(void);
static void ProcessNextSample(void);
static void CLIB_DECL DetailsPrintf(int box, const char *fmt, ...) ATTR_PRINTF(2,3);
static const char * StatusString(int iStatus);

/***************************************************************************
    Internal variables
 ***************************************************************************/

#define MAX_AUDITBOX_TEXT   0x7FFFFFFE

static volatile HWND hAudit;
static volatile int rom_index = 0;
static volatile int roms_correct = 0;
static volatile int roms_incorrect = 0;
static volatile int sample_index = 0;
static volatile int samples_correct = 0;
static volatile int samples_incorrect = 0;
static volatile BOOL bPaused = FALSE;
static volatile BOOL bCancel = FALSE;
static int m_choice = 0;

/***************************************************************************
    External functions
 ***************************************************************************/

static int strcatvprintf(std::string &str, const char *format, va_list args)
{
	char tempbuf[4096];
	int result = vsprintf(tempbuf, format, args);
	str.append(tempbuf);
	return result;
}

static int strcatprintf(std::string &str, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int retVal = strcatvprintf(str, format, ap);
	va_end(ap);
	return retVal;
}

void AuditDialog(HWND hParent, int choice)
{
	rom_index         = 0;
	roms_correct      = -1; // ___empty must not be counted
	roms_incorrect    = 0;
	sample_index      = 0;
	samples_correct   = -1; // ___empty must not be counted
	samples_incorrect = 0;
	m_choice = choice;

	//RS use Riched32.dll
	HMODULE hModule = LoadLibrary(TEXT("Riched32.dll"));
	if( hModule )
	{
		DialogBox(GetModuleHandle(NULL),MAKEINTRESOURCE(IDD_AUDIT),hParent,AuditWindowProc);
		FreeLibrary( hModule );
		hModule = NULL;
	}
	else
		MessageBox(GetMainWindow(),TEXT("Unable to Load Riched32.dll"),TEXT("Error"), MB_OK | MB_ICONERROR);
}

void InitGameAudit(int drvindex)
{
	rom_index = drvindex;
}

const char * GetAuditString(int audit_result)
{
	switch (audit_result)
	{
	case media_auditor::CORRECT :
	case media_auditor::BEST_AVAILABLE :
	case media_auditor::NONE_NEEDED :
		return "Yes";

	case media_auditor::NOTFOUND :
	case media_auditor::INCORRECT :
		return "No";

	default:
		if (audit_result == -1)
			printf("GetAuditString: Audit value -1, try doing a full F5 audit\n");
		else
			printf("GetAuditString: Unknown audit value %i\n",audit_result);
		fflush(stdout);
	}

	return "?";
}

BOOL IsAuditResultKnown(int audit_result)
{
	return TRUE;
}

BOOL IsAuditResultYes(int audit_result)
{
	return audit_result == media_auditor::CORRECT
		|| audit_result == media_auditor::BEST_AVAILABLE
		|| audit_result == media_auditor::NONE_NEEDED;
}

BOOL IsAuditResultNo(int audit_result)
{
	return audit_result == media_auditor::NOTFOUND
		|| audit_result == media_auditor::INCORRECT;
}


/***************************************************************************
    Internal functions
 ***************************************************************************/
// Verifies the ROM set while calling SetRomAuditResults
int MameUIVerifyRomSet(int game, bool choice)
{
	driver_enumerator enumerator(MameUIGlobal(), driver_list::driver(game));
	enumerator.next();
	media_auditor auditor(enumerator);
	media_auditor::summary summary = auditor.audit_media(AUDIT_VALIDATE_FAST);

	std::string summary_string;

	if (summary == media_auditor::NOTFOUND)
	{
		if (m_choice < 2)
			strcatprintf(summary_string, "%s: Romset NOT FOUND\n", driver_list::driver(game).name);
	}
	else
	if (choice)
	{
		auditor.winui_summarize(driver_list::driver(game).name, &summary_string); // audit all games
	}
	else
	{
		std::ostringstream whatever;
		auditor.summarize(driver_list::driver(game).name, &whatever); // audit one game
		summary_string = whatever.str();
	}

	// output the summary of the audit
	DetailsPrintf(0, "%s", summary_string.c_str());

	SetRomAuditResults(game, summary);
	return summary;
}

// Verifies the Sample set while calling SetSampleAuditResults
int MameUIVerifySampleSet(int game)
{
	driver_enumerator enumerator(MameUIGlobal(), driver_list::driver(game));
	enumerator.next();
	media_auditor auditor(enumerator);
	media_auditor::summary summary = auditor.audit_samples();

	std::string summary_string;

	if (summary == media_auditor::NOTFOUND)
		strcatprintf(summary_string, "%s: Sampleset NOT FOUND\n", driver_list::driver(game).name);
	else
	{
		std::ostringstream whatever;
		auditor.summarize(driver_list::driver(game).name, &whatever);
		summary_string = whatever.str();
	}

	// output the summary of the audit
	DetailsPrintf(1, "%s", summary_string.c_str());

	SetSampleAuditResults(game, summary);
	return summary;
}

static DWORD WINAPI AuditThreadProc(LPVOID hDlg)
{
	char buffer[200];

	while (!bCancel)
	{
		if (!bPaused)
		{
			if (rom_index != -1)
			{
				sprintf(buffer, "Checking Set %s - %s", driver_list::driver(rom_index).name, driver_list::driver(rom_index).type.fullname());
				win_set_window_text_utf8((HWND)hDlg, buffer);
				ProcessNextRom();
			}
			else
			if (sample_index != -1)
			{
				sprintf(buffer, "Checking Set %s - %s", driver_list::driver(sample_index).name, driver_list::driver(sample_index).type.fullname());
				win_set_window_text_utf8((HWND)hDlg, buffer);
				ProcessNextSample();
			}
			else
			{
				win_set_window_text_utf8((HWND)hDlg, "File Audit");
				EnableWindow(GetDlgItem((HWND)hDlg, IDPAUSE), FALSE);
				ExitThread(1);
			}
		}
	}
	return 0;
}

static INT_PTR CALLBACK AuditWindowProc(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	static HANDLE hThread;
	static DWORD dwThreadID = 0;
	HWND hEdit;

	switch (Msg)
	{
	case WM_INITDIALOG:
		hAudit = hDlg;
		//RS 20030613 Set Bkg of RichEdit Ctrl
		hEdit = GetDlgItem(hAudit, IDC_AUDIT_DETAILS);
		if (hEdit)
		{
			SendMessage( hEdit, EM_SETBKGNDCOLOR, FALSE, GetSysColor(COLOR_BTNFACE) );
			// MSH - Set to max
			SendMessage( hEdit, EM_SETLIMITTEXT, MAX_AUDITBOX_TEXT, 0 );
		}

		SendDlgItemMessage(hDlg, IDC_ROMS_PROGRESS,    PBM_SETRANGE, 0, MAKELPARAM(0, driver_list::total()));
		SendDlgItemMessage(hDlg, IDC_SAMPLES_PROGRESS, PBM_SETRANGE, 0, MAKELPARAM(0, driver_list::total()));
		bPaused = false;
		bCancel = false;
		rom_index = 0;
		hThread = CreateThread(NULL, 0, AuditThreadProc, hDlg, 0, &dwThreadID);
		return 1;
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDCANCEL:
			bPaused = false;
			if (hThread)
			{
				bCancel = true;
				DWORD dwExitCode = 0;
				if (GetExitCodeThread(hThread, &dwExitCode) && (dwExitCode == STILL_ACTIVE))
				{
					PostMessage(hDlg, WM_COMMAND, wParam, lParam);
					return 1;
				}
				CloseHandle(hThread);
			}
			EndDialog(hDlg,0);
			m_choice = 0;
			break;

		case IDPAUSE:
			if (bPaused)
			{
				SendDlgItemMessage(hAudit, IDPAUSE, WM_SETTEXT, 0, (LPARAM)TEXT("Pause"));
				bPaused = false;
			}
			else
			{
				SendDlgItemMessage(hAudit, IDPAUSE, WM_SETTEXT, 0, (LPARAM)TEXT("Continue"));
				bPaused = true;
			}
			break;
		}
		return 1;
	}
	return 0;
}

/* Callback for the Audit property sheet */
INT_PTR CALLBACK GameAuditDialogProc(HWND hDlg,UINT Msg,WPARAM wParam,LPARAM lParam)
{
	switch (Msg)
	{
	case WM_INITDIALOG:
		FlushFileCaches();
		hAudit = hDlg;
		win_set_window_text_utf8(GetDlgItem(hDlg, IDC_PROP_TITLE), GameInfoTitle(OPTIONS_GAME, rom_index));
		SetTimer(hDlg, 0, 1, NULL);
		return 1;

	case WM_TIMER:
		KillTimer(hDlg, 0);
		{
			int iStatus;
			LPCSTR lpStatus;

			iStatus = MameUIVerifyRomSet(rom_index, 0);
			lpStatus = DriverUsesRoms(rom_index) ? StatusString(iStatus) : "None required";
			win_set_window_text_utf8(GetDlgItem(hDlg, IDC_PROP_ROMS), lpStatus);

			if (DriverUsesSamples(rom_index))
			{
				iStatus = MameUIVerifySampleSet(rom_index);
				lpStatus = StatusString(iStatus);
			}
			else
			{
				lpStatus = "None Required";
			}

			win_set_window_text_utf8(GetDlgItem(hDlg, IDC_PROP_SAMPLES), lpStatus);
		}
		ShowWindow(hDlg, SW_SHOW);
		break;
	}
	return 0;
}

static void ProcessNextRom()
{
	int retval = 0;
	TCHAR buffer[200];

	retval = MameUIVerifyRomSet(rom_index, 1);
	switch (retval)
	{
	case media_auditor::BEST_AVAILABLE: /* correct, incorrect or separate count? */
	case media_auditor::CORRECT:
	case media_auditor::NONE_NEEDED:
		roms_correct++;
		_stprintf(buffer, TEXT("%i"), roms_correct);
		SendDlgItemMessage(hAudit, IDC_ROMS_CORRECT, WM_SETTEXT, 0, (LPARAM)buffer);
		_stprintf(buffer, TEXT("%i"), roms_correct + roms_incorrect);
		SendDlgItemMessage(hAudit, IDC_ROMS_TOTAL, WM_SETTEXT, 0, (LPARAM)buffer);
		break;

	case media_auditor::NOTFOUND:
	case media_auditor::INCORRECT:
		roms_incorrect++;
		_stprintf(buffer, TEXT("%i"), roms_incorrect);
		SendDlgItemMessage(hAudit, IDC_ROMS_INCORRECT, WM_SETTEXT, 0, (LPARAM)buffer);
		_stprintf(buffer, TEXT("%i"), roms_correct + roms_incorrect);
		SendDlgItemMessage(hAudit, IDC_ROMS_TOTAL, WM_SETTEXT, 0, (LPARAM)buffer);
		break;
	}

	rom_index++;
	SendDlgItemMessage(hAudit, IDC_ROMS_PROGRESS, PBM_SETPOS, rom_index, 0);

	if (rom_index == driver_list::total())
	{
		sample_index = 0;
		rom_index = -1;
	}
}

static void ProcessNextSample()
{
	int retval = 0;
	TCHAR buffer[200];

	retval = MameUIVerifySampleSet(sample_index);

	switch (retval)
	{
	case media_auditor::NOTFOUND:
	case media_auditor::INCORRECT:
		if (DriverUsesSamples(sample_index))
		{
			samples_incorrect++;
			_stprintf(buffer, TEXT("%i"), samples_incorrect);
			SendDlgItemMessage(hAudit, IDC_SAMPLES_INCORRECT, WM_SETTEXT, 0, (LPARAM)buffer);
			_stprintf(buffer, TEXT("%i"), samples_correct + samples_incorrect);
			SendDlgItemMessage(hAudit, IDC_SAMPLES_TOTAL, WM_SETTEXT, 0, (LPARAM)buffer);
			break;
		}
		[[fallthrough]];
	default:
		if ((DriverUsesSamples(sample_index)) || (m_choice == 1))
		{
			samples_correct++;
			_stprintf(buffer, TEXT("%i"), samples_correct);
			SendDlgItemMessage(hAudit, IDC_SAMPLES_CORRECT, WM_SETTEXT, 0, (LPARAM)buffer);
			_stprintf(buffer, TEXT("%i"), samples_correct + samples_incorrect);
			SendDlgItemMessage(hAudit, IDC_SAMPLES_TOTAL, WM_SETTEXT, 0, (LPARAM)buffer);
			break;
		}
	}

	sample_index++;
	SendDlgItemMessage(hAudit, IDC_SAMPLES_PROGRESS, PBM_SETPOS, sample_index, 0);

	if (sample_index == driver_list::total())
	{
		DetailsPrintf(1, "Audit complete.\n");
		SendDlgItemMessage(hAudit, IDCANCEL, WM_SETTEXT, 0, (LPARAM)TEXT("Close"));
		sample_index = -1;
	}
}

static void CLIB_DECL DetailsPrintf(int box, const char *fmt, ...)
{
	//RS 20030613 Different Ids for Property Page and Dialog
	// so see which one's currently instantiated
	HWND hEdit = 0;
	if (box == 0)
	{
		hEdit = GetDlgItem(hAudit, IDC_AUDIT_DETAILS);
		if (!hEdit)
			hEdit = GetDlgItem(hAudit, IDC_AUDIT_DETAILS_PROP0);
	}
	else
	if (box == 1)
	{
		hEdit = GetDlgItem(hAudit, IDC_AUDIT_DETAILS);
		if (!hEdit)
			hEdit = GetDlgItem(hAudit, IDC_AUDIT_DETAILS_PROP1);
	}

	if (!hEdit)
	{
		// Auditing via F5 - no window to display the results
		//printf("audit detailsprintf() can't find any audit control\n");
		return;
	}

	va_list marker;
	va_start(marker, fmt);
	char buffer[8000];
	vsprintf(buffer, fmt, marker);
	va_end(marker);

	TCHAR* t_s = ui_wstring_from_utf8(ConvertToWindowsNewlines(buffer));
	if( !t_s || _tcscmp(TEXT(""), t_s) == 0)
		return;

	int textLength = Edit_GetTextLength(hEdit);
	Edit_SetSel(hEdit, textLength, textLength);
	SendMessage( hEdit, EM_REPLACESEL, false, (WPARAM)(LPCTSTR)win_tstring_strdup(t_s) );

	free(t_s);
}

static const char * StatusString(int iStatus)
{
	static const char *ptr = "Unknown";

	switch (iStatus)
	{
	case media_auditor::CORRECT:
		ptr = "Passed";
		break;

	case media_auditor::BEST_AVAILABLE:
		ptr = "Best available";
		break;

	case media_auditor::NONE_NEEDED:
		ptr = "None Required";
		break;

	case media_auditor::NOTFOUND:
		ptr = "Not found";
		break;

	case media_auditor::INCORRECT:
		ptr = "Failed";
		break;
	}

	return ptr;
}

