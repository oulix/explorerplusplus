// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#include "stdafx.h"
#include "Explorer++.h"
#include "AboutDialog.h"
#include "Config.h"
#include "CustomizeColorsDialog.h"
#include "DestroyFilesDialog.h"
#include "DisplayColoursDialog.h"
#include "FileProgressSink.h"
#include "FilterDialog.h"
#include "HelpFileMissingDialog.h"
#include "IModelessDialogNotification.h"
#include "MergeFilesDialog.h"
#include "ModelessDialogs.h"
#include "OptionsDialog.h"
#include "ScriptingDialog.h"
#include "SearchDialog.h"
#include "SplitFileDialog.h"
#include "UpdateCheckDialog.h"
#include "WildcardSelectDialog.h"
#include "MainResource.h"
#include "../Helper/ListViewHelper.h"
#include "../Helper/ProcessHelper.h"
#include "../Helper/ShellHelper.h"
#include <boost/scope_exit.hpp>
#include <wil/com.h>

#pragma warning(disable:4459) // declaration of 'boost_scope_exit_aux_args' hides global declaration

void Explorerplusplus::OnChangeDisplayColors()
{
	CDisplayColoursDialog DisplayColoursDialog(m_hLanguageModule, IDD_DISPLAYCOLOURS, m_hContainer,
		m_hDisplayWindow, m_config->displayWindowCentreColor.ToCOLORREF(),
		m_config->displayWindowSurroundColor.ToCOLORREF());
	DisplayColoursDialog.ShowModalDialog();
}

void Explorerplusplus::OnFilterResults()
{
	CFilterDialog FilterDialog(m_hLanguageModule, IDD_FILTER, m_hContainer, this);
	FilterDialog.ShowModalDialog();
}

void Explorerplusplus::OnMergeFiles()
{
	std::wstring currentDirectory = m_pActiveShellBrowser->GetDirectory();

	std::list<std::wstring>	FullFilenameList;
	int iItem = -1;

	while((iItem = ListView_GetNextItem(m_hActiveListView, iItem, LVNI_SELECTED)) != -1)
	{
		TCHAR szFullFilename[MAX_PATH];
		m_pActiveShellBrowser->GetItemFullName(iItem, szFullFilename, SIZEOF_ARRAY(szFullFilename));
		FullFilenameList.push_back(szFullFilename);
	}

	CMergeFilesDialog CMergeFilesDialog(m_hLanguageModule, IDD_MERGEFILES, m_hContainer,
		this, currentDirectory, FullFilenameList, m_config->globalFolderSettings.showFriendlyDates);
	CMergeFilesDialog.ShowModalDialog();
}

void Explorerplusplus::OnSplitFile()
{
	int iSelected = ListView_GetNextItem(m_hActiveListView, -1, LVNI_SELECTED);

	if(iSelected != -1)
	{
		TCHAR szFullFilename[MAX_PATH];
		m_pActiveShellBrowser->GetItemFullName(iSelected, szFullFilename, SIZEOF_ARRAY(szFullFilename));

		CSplitFileDialog SplitFileDialog(m_hLanguageModule, IDD_SPLITFILE, m_hContainer, this, szFullFilename);
		SplitFileDialog.ShowModalDialog();
	}
}

void Explorerplusplus::OnDestroyFiles()
{
	std::list<std::wstring>	FullFilenameList;
	int iItem = -1;

	while((iItem = ListView_GetNextItem(m_hActiveListView, iItem, LVNI_SELECTED)) != -1)
	{
		TCHAR szFullFilename[MAX_PATH];
		m_pActiveShellBrowser->GetItemFullName(iItem, szFullFilename, SIZEOF_ARRAY(szFullFilename));
		FullFilenameList.push_back(szFullFilename);
	}

	CDestroyFilesDialog CDestroyFilesDialog(m_hLanguageModule, IDD_DESTROYFILES,
		m_hContainer, FullFilenameList, m_config->globalFolderSettings.showFriendlyDates);
	CDestroyFilesDialog.ShowModalDialog();
}

void Explorerplusplus::OnWildcardSelect(BOOL bSelect)
{
	CWildcardSelectDialog WilcardSelectDialog(m_hLanguageModule, IDD_WILDCARDSELECT, m_hContainer, bSelect, this);
	WilcardSelectDialog.ShowModalDialog();
}

void Explorerplusplus::OnSearch()
{
	if(g_hwndSearch == NULL)
	{
		Tab &selectedTab = m_tabContainer->GetSelectedTab();
		std::wstring currentDirectory = selectedTab.GetShellBrowser()->GetDirectory();

		CSearchDialog *SearchDialog = new CSearchDialog(m_hLanguageModule, IDD_SEARCH, m_hContainer,
			currentDirectory, this, m_tabContainer);
		g_hwndSearch = SearchDialog->ShowModelessDialog(new CModelessDialogNotification());
	}
	else
	{
		SetFocus(g_hwndSearch);
	}
}

void Explorerplusplus::OnCustomizeColors()
{
	CCustomizeColorsDialog CustomizeColorsDialog(m_hLanguageModule, IDD_CUSTOMIZECOLORS, m_hContainer, this, &m_ColorRules);
	CustomizeColorsDialog.ShowModalDialog();

	/* Causes the active listview to redraw (therefore
	applying any updated color schemes). */
	InvalidateRect(m_hActiveListView, NULL, FALSE);
}

void Explorerplusplus::OnRunScript()
{
	if (g_hwndRunScript == NULL)
	{
		ScriptingDialog *scriptingDialog = new ScriptingDialog(m_hLanguageModule, IDD_SCRIPTING, m_hContainer, this);
		g_hwndRunScript = scriptingDialog->ShowModelessDialog(new CModelessDialogNotification());
	}
	else
	{
		SetFocus(g_hwndRunScript);
	}
}

void Explorerplusplus::OnShowOptions()
{
	if(g_hwndOptions == NULL)
	{
		OptionsDialog *optionsDialog = OptionsDialog::Create(m_config, m_hLanguageModule, this, m_tabContainer);
		g_hwndOptions = optionsDialog->Show(m_hContainer);
	}
	else
	{
		SetFocus(g_hwndOptions);
	}
}

void Explorerplusplus::OnShowHelp()
{
	TCHAR szHelpFile[MAX_PATH];
	GetProcessImageName(GetCurrentProcessId(), szHelpFile, SIZEOF_ARRAY(szHelpFile));
	PathRemoveFileSpec(szHelpFile);
	PathAppend(szHelpFile, NExplorerplusplus::HELP_FILE_NAME);

	unique_pidl_absolute pidl;
	HRESULT hr = SHParseDisplayName(szHelpFile, nullptr, wil::out_param(pidl), 0, nullptr);

	bool bOpenedHelpFile = false;

	if(SUCCEEDED(hr))
	{
		BOOL bRes = ExecuteFileAction(m_hContainer, NULL, NULL, NULL, pidl.get());

		if(bRes)
		{
			bOpenedHelpFile = true;
		}
	}

	if(!bOpenedHelpFile)
	{
		CHelpFileMissingDialog HelpFileMissingDialog(m_hLanguageModule, IDD_HELPFILEMISSING, m_hContainer);
		HelpFileMissingDialog.ShowModalDialog();
	}
}

void Explorerplusplus::OnCheckForUpdates()
{
	CUpdateCheckDialog UpdateCheckDialog(m_hLanguageModule, IDD_UPDATECHECK, m_hContainer);
	UpdateCheckDialog.ShowModalDialog();
}

void Explorerplusplus::OnAbout()
{
	CAboutDialog AboutDialog(m_hLanguageModule, IDD_ABOUT, m_hContainer);
	AboutDialog.ShowModalDialog();
}

void Explorerplusplus::OnSaveDirectoryListing() const
{
	TCHAR FileName[MAX_PATH];
	LoadString(m_hLanguageModule, IDS_GENERAL_DIRECTORY_LISTING_FILENAME, FileName, SIZEOF_ARRAY(FileName));
	StringCchCat(FileName, SIZEOF_ARRAY(FileName), _T(".txt"));
	BOOL bSaveNameRetrieved = GetFileNameFromUser(m_hContainer, FileName, SIZEOF_ARRAY(FileName), m_CurrentDirectory.c_str());

	if(bSaveNameRetrieved)
	{
		NFileOperations::SaveDirectoryListing(m_CurrentDirectory, FileName);
	}
}

void Explorerplusplus::OnCreateNewFolder()
{
	auto pidlDirectory = m_pActiveShellBrowser->GetDirectoryIdl();

	wil::com_ptr<IShellItem> directoryShellItem;
	HRESULT hr = SHCreateItemFromIDList(pidlDirectory.get(), IID_PPV_ARGS(&directoryShellItem));

	if (FAILED(hr))
	{
		return;
	}

	FileProgressSink *sink = FileProgressSink::CreateNew();
	sink->SetPostNewItemObserver([this] (PIDLIST_ABSOLUTE pidl) {
		m_bCountingDown = TRUE;
		NListView::ListView_SelectAllItems(m_hActiveListView, FALSE);
		SetFocus(m_hActiveListView);

		m_pActiveShellBrowser->QueueRename(pidl);
	});

	TCHAR newFolderName[128];
	LoadString(m_hLanguageModule, IDS_NEW_FOLDER_NAME, newFolderName, SIZEOF_ARRAY(newFolderName));
	hr = NFileOperations::CreateNewFolder(directoryShellItem.get(), newFolderName, sink);
	sink->Release();

	if(FAILED(hr))
	{
		TCHAR szTemp[512];

		LoadString(m_hLanguageModule, IDS_NEWFOLDERERROR, szTemp,
			SIZEOF_ARRAY(szTemp));

		MessageBox(m_hContainer, szTemp, NExplorerplusplus::APP_NAME,
			MB_ICONERROR | MB_OK);
	}
}

void Explorerplusplus::OnResolveLink()
{
	TCHAR	ShortcutFileName[MAX_PATH];
	TCHAR	szFullFileName[MAX_PATH];
	TCHAR	szPath[MAX_PATH];
	HRESULT	hr;
	int		iItem;

	iItem = ListView_GetNextItem(m_hActiveListView, -1, LVNI_FOCUSED);

	if(iItem != -1)
	{
		m_pActiveShellBrowser->GetItemFullName(iItem, ShortcutFileName, SIZEOF_ARRAY(ShortcutFileName));

		hr = NFileOperations::ResolveLink(m_hContainer, 0, ShortcutFileName, szFullFileName, SIZEOF_ARRAY(szFullFileName));

		if(hr == S_OK)
		{
			/* Strip the filename, just leaving the path component. */
			StringCchCopy(szPath, SIZEOF_ARRAY(szPath), szFullFileName);
			PathRemoveFileSpec(szPath);

			hr = m_tabContainer->CreateNewTab(szPath, TabSettings(_selected = true));

			if(SUCCEEDED(hr))
			{
				/* Strip off the path, and select the shortcut target
				in the listview. */
				PathStripPath(szFullFileName);
				m_pActiveShellBrowser->SelectFiles(szFullFileName);

				SetFocus(m_hActiveListView);
			}
		}
	}
}

HRESULT Explorerplusplus::OnGoBack()
{
	Tab &selectedTab = m_tabContainer->GetSelectedTab();
	return selectedTab.GetNavigationController()->GoBack();
}

HRESULT Explorerplusplus::OnGoForward()
{
	Tab &selectedTab = m_tabContainer->GetSelectedTab();
	return selectedTab.GetNavigationController()->GoForward();
}

HRESULT Explorerplusplus::OnGoToOffset(int offset)
{
	Tab &selectedTab = m_tabContainer->GetSelectedTab();
	return selectedTab.GetNavigationController()->GoToOffset(offset);
}

HRESULT Explorerplusplus::OnGoToKnownFolder(REFKNOWNFOLDERID knownFolderId)
{
	unique_pidl_absolute pidl;
	HRESULT hr = SHGetKnownFolderIDList(knownFolderId, KF_FLAG_DEFAULT, nullptr, wil::out_param(pidl));

	if (FAILED(hr))
	{
		return hr;
	}

	Tab &selectedTab = m_tabContainer->GetSelectedTab();
	return selectedTab.GetNavigationController()->BrowseFolder(pidl.get());
}

HRESULT Explorerplusplus::OnGoHome()
{
	Tab &selectedTab = m_tabContainer->GetSelectedTab();
	HRESULT hr = selectedTab.GetNavigationController()->BrowseFolder(m_config->defaultTabDirectory);

	if (FAILED(hr))
	{
		hr = selectedTab.GetNavigationController()->BrowseFolder(m_config->defaultTabDirectoryStatic);
	}

	return hr;
}