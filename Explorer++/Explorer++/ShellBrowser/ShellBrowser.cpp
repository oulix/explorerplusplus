// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#include "stdafx.h"
#include "ShellBrowser.h"
#include "Config.h"
#include "ItemData.h"
#include "MainResource.h"
#include "SortModes.h"
#include "ViewModes.h"
#include "../Helper/Controls.h"
#include "../Helper/DriveInfo.h"
#include "../Helper/FileOperations.h"
#include "../Helper/FolderSize.h"
#include "../Helper/Helper.h"
#include "../Helper/ListViewHelper.h"
#include "../Helper/Macros.h"
#include "../Helper/ShellHelper.h"
#include <boost/scope_exit.hpp>
#include <wil/com.h>
#include <list>

#pragma warning(disable:4459) // declaration of 'boost_scope_exit_aux_args' hides global declaration

void CALLBACK TimerProc(HWND hwnd,UINT uMsg,UINT_PTR idEvent,DWORD dwTime);

int CShellBrowser::listViewParentSubclassIdCounter = 0;

/* IUnknown interface members. */
HRESULT __stdcall CShellBrowser::QueryInterface(REFIID iid, void **ppvObject)
{
	*ppvObject = NULL;

	if (iid == IID_IUnknown)
	{
		*ppvObject = this;
	}

	if (*ppvObject)
	{
		AddRef();
		return S_OK;
	}

	return E_NOINTERFACE;
}

ULONG __stdcall CShellBrowser::AddRef(void)
{
	return ++m_iRefCount;
}

ULONG __stdcall CShellBrowser::Release(void)
{
	m_iRefCount--;

	if (m_iRefCount == 0)
	{
		delete this;
		return 0;
	}

	return m_iRefCount;
}

CShellBrowser *CShellBrowser::CreateNew(int id, HINSTANCE resourceInstance, HWND hOwner,
	CachedIcons *cachedIcons, const Config *config, TabNavigationInterface *tabNavigation,
	const FolderSettings &folderSettings, boost::optional<FolderColumns> initialColumns)
{
	return new CShellBrowser(id, resourceInstance, hOwner, cachedIcons, config, tabNavigation,
		folderSettings, initialColumns);
}

CShellBrowser::CShellBrowser(int id, HINSTANCE resourceInstance, HWND hOwner,
	CachedIcons *cachedIcons, const Config *config, TabNavigationInterface *tabNavigation,
	const FolderSettings &folderSettings, boost::optional<FolderColumns> initialColumns) :
	m_ID(id),
	m_hResourceModule(resourceInstance),
	m_hOwner(hOwner),
	m_cachedIcons(cachedIcons),
	m_config(config),
	m_tabNavigation(tabNavigation),
	m_folderSettings(folderSettings),
	m_folderColumns(initialColumns ? *initialColumns : config->globalFolderSettings.folderColumns),
	m_columnThreadPool(1),
	m_columnResultIDCounter(0),
	m_thumbnailThreadPool(1),
	m_thumbnailResultIDCounter(0),
	m_infoTipsThreadPool(1),
	m_infoTipResultIDCounter(0)
{
	m_iRefCount = 1;

	m_hListView = SetUpListView(hOwner);
	m_iconFetcher = std::make_unique<IconFetcher>(m_hListView, cachedIcons);

	InitializeDragDropHelpers();

	m_bFolderVisited = FALSE;

	m_bColumnsPlaced = FALSE;
	m_bOverFolder = FALSE;
	m_bDragging = FALSE;
	m_bVirtualFolder = FALSE;
	m_bThumbnailsSetup = FALSE;
	m_nCurrentColumns = 0;
	m_iDirMonitorId = -1;
	m_pActiveColumns = NULL;
	m_bPerformingDrag = FALSE;
	m_nActiveColumns = 0;
	m_bNewItemCreated = FALSE;
	m_iDropped = -1;
	m_middleButtonItem = -1;

	m_uniqueFolderId = 0;

	m_PreviousSortColumnExists = false;

	InitializeCriticalSection(&m_csDirectoryAltered);

	m_iFolderIcon = GetDefaultFolderIconIndex();
	m_iFileIcon = GetDefaultFileIconIndex();

	m_thumbnailThreadPool.push([] (int id) {
		UNREFERENCED_PARAMETER(id);

		CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	});
}

CShellBrowser::~CShellBrowser()
{
	DestroyWindow(m_hListView);

	m_columnThreadPool.clear_queue();
	m_thumbnailThreadPool.clear_queue();
	m_infoTipsThreadPool.clear_queue();

	m_thumbnailThreadPool.push([] (int id) {
		UNREFERENCED_PARAMETER(id);

		CoUninitialize();
	});

	/* Release the drag and drop helpers. */
	m_pDropTargetHelper->Release();
	m_pDragSourceHelper->Release();

	DeleteCriticalSection(&m_csDirectoryAltered);

	/* TODO: Also destroy the thumbnails imagelist. */
}

HWND CShellBrowser::SetUpListView(HWND parent)
{
	HWND hListView = CreateListView(parent, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
		WS_CLIPCHILDREN | LVS_ICON | LVS_EDITLABELS | LVS_SHOWSELALWAYS |
		LVS_SHAREIMAGELISTS | LVS_AUTOARRANGE | WS_TABSTOP | LVS_ALIGNTOP);

	if (hListView == nullptr)
	{
		return nullptr;
	}

	DWORD dwExtendedStyle = ListView_GetExtendedListViewStyle(hListView);

	if (m_config->useFullRowSelect)
	{
		dwExtendedStyle |= LVS_EX_FULLROWSELECT;
	}

	if (m_config->checkBoxSelection)
	{
		dwExtendedStyle |= LVS_EX_CHECKBOXES;
	}

	ListView_SetExtendedListViewStyle(hListView, dwExtendedStyle);

	NListView::ListView_SetAutoArrange(m_hListView, m_folderSettings.autoArrange);
	NListView::ListView_SetGridlines(m_hListView, m_config->globalFolderSettings.showGridlines);

	if (m_folderSettings.applyFilter)
	{
		NListView::ListView_SetBackgroundImage(m_hListView, IDB_FILTERINGAPPLIED);
	}

	NListView::ListView_ActivateOneClickSelect(m_hListView, m_config->globalFolderSettings.oneClickActivate,
		m_config->globalFolderSettings.oneClickActivateHoverTime);

	SetWindowTheme(hListView, L"Explorer", NULL);

	m_windowSubclasses.push_back(WindowSubclassWrapper(hListView, ListViewProcStub,
		LISTVIEW_SUBCLASS_ID, reinterpret_cast<DWORD_PTR>(this)));

	m_windowSubclasses.push_back(WindowSubclassWrapper(parent, ListViewParentProcStub,
		listViewParentSubclassIdCounter++, reinterpret_cast<DWORD_PTR>(this)));

	return hListView;
}

BOOL CShellBrowser::GetAutoArrange(void) const
{
	return m_folderSettings.autoArrange;
}

void CShellBrowser::SetAutoArrange(BOOL autoArrange)
{
	m_folderSettings.autoArrange = autoArrange;

	NListView::ListView_SetAutoArrange(m_hListView, m_folderSettings.autoArrange);
}

ViewMode CShellBrowser::GetViewMode() const
{
	return m_folderSettings.viewMode;
}

/* This function is only called on 'hard' view changes
(i.e. view changes resulting from user requests). It is
not called when a tab is first set up (in which case
the view mode still needs to be setup), or when entering
a folder. */
void CShellBrowser::SetViewMode(ViewMode viewMode)
{
	if (viewMode == m_folderSettings.viewMode)
	{
		return;
	}

	if (m_folderSettings.viewMode == +ViewMode::Thumbnails && viewMode != +ViewMode::Thumbnails)
		RemoveThumbnailsView();

	SetViewModeInternal(viewMode);

	switch (viewMode)
	{
	case ViewMode::Tiles:
		SetTileViewInfo();
		break;
	}
}

/* Explicitly sets the view mode within in the listview.
This function also initializes any items needed to support
the current view mode. This MUST be done within this
function, as when a tab is first opened, the view settings
will need to be initialized. */
void CShellBrowser::SetViewModeInternal(ViewMode viewMode)
{
	DWORD dwStyle;

	switch (viewMode)
	{
	case ViewMode::ExtraLargeIcons:
	{
		wil::com_ptr<IImageList> pImageList;
		SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(&pImageList));
		ListView_SetImageList(m_hListView, reinterpret_cast<HIMAGELIST>(pImageList.get()), LVSIL_NORMAL);
	}
	break;

	case ViewMode::LargeIcons:
	{
		wil::com_ptr<IImageList> pImageList;
		SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(&pImageList));
		ListView_SetImageList(m_hListView, reinterpret_cast<HIMAGELIST>(pImageList.get()), LVSIL_NORMAL);
	}
	break;

	/* Do nothing. This will setup the listview by itself. */
	case ViewMode::Thumbnails:
		break;

	case ViewMode::Tiles:
	case ViewMode::Icons:
	{
		wil::com_ptr<IImageList> pImageList;
		SHGetImageList(SHIL_LARGE, IID_PPV_ARGS(&pImageList));
		ListView_SetImageList(m_hListView, reinterpret_cast<HIMAGELIST>(pImageList.get()), LVSIL_NORMAL);
	}
	break;

	case ViewMode::SmallIcons:
	case ViewMode::List:
	case ViewMode::Details:
	{
		wil::com_ptr<IImageList> pImageList;
		SHGetImageList(SHIL_SMALL, IID_PPV_ARGS(&pImageList));
		ListView_SetImageList(m_hListView, reinterpret_cast<HIMAGELIST>(pImageList.get()), LVSIL_SMALL);
	}
	break;
	}

	/* Delete all the tile view columns. */
	if (m_folderSettings.viewMode == +ViewMode::Tiles && viewMode != +ViewMode::Tiles)
		DeleteTileViewColumns();

	switch (viewMode)
	{
	case ViewMode::Tiles:
		dwStyle = LV_VIEW_TILE;

		InsertTileViewColumns();
		break;

	case ViewMode::ExtraLargeIcons:
	case ViewMode::LargeIcons:
	case ViewMode::Icons:
		dwStyle = LV_VIEW_ICON;
		break;

	case ViewMode::SmallIcons:
		dwStyle = LV_VIEW_SMALLICON;
		break;

	case ViewMode::List:
		dwStyle = LV_VIEW_LIST;
		break;

	case ViewMode::Details:
		dwStyle = LV_VIEW_DETAILS;

		if (!m_bColumnsPlaced)
		{
			PlaceColumns();
			m_bColumnsPlaced = TRUE;
		}
		break;

	case ViewMode::Thumbnails:
		dwStyle = LV_VIEW_ICON;

		if (!m_bThumbnailsSetup)
			SetupThumbnailsView();
		break;

	default:
		dwStyle = LV_VIEW_ICON;
		viewMode = ViewMode::Icons;
		break;
	}

	m_folderSettings.viewMode = viewMode;

	if (viewMode != +ViewMode::Details)
	{
		m_columnThreadPool.clear_queue();
		m_columnResults.clear();
	}

	SendMessage(m_hListView, LVM_SETVIEW, dwStyle, 0);
}

SortMode CShellBrowser::GetSortMode() const
{
	return m_folderSettings.sortMode;
}

void CShellBrowser::SetSortMode(SortMode sortMode)
{
	m_folderSettings.sortMode = sortMode;
}

HRESULT CShellBrowser::InitializeDragDropHelpers(void)
{
	HRESULT hr;

	/* Initialize the drag source helper, and use it to initialize the drop target helper. */
	hr = CoCreateInstance(CLSID_DragDropHelper, NULL, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&m_pDragSourceHelper));

	if (SUCCEEDED(hr))
	{
		hr = m_pDragSourceHelper->QueryInterface(IID_PPV_ARGS(&m_pDropTargetHelper));

		RegisterDragDrop(m_hListView, this);

		/* RegisterDragDrop calls AddRef on initialization. */
		Release();
	}

	return hr;
}

int CShellBrowser::GetId() const
{
	return m_ID;
}

void CShellBrowser::OnGridlinesSettingChanged()
{
	NListView::ListView_SetGridlines(m_hListView, m_config->globalFolderSettings.showGridlines);
}

void CShellBrowser::UpdateFileSelectionInfo(int iCacheIndex,BOOL Selected)
{
	ULARGE_INTEGER	ulFileSize;
	BOOL			IsFolder;

	IsFolder = (m_itemInfoMap.at(iCacheIndex).wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
	== FILE_ATTRIBUTE_DIRECTORY;

	ulFileSize.LowPart = m_itemInfoMap.at(iCacheIndex).wfd.nFileSizeLow;
	ulFileSize.HighPart = m_itemInfoMap.at(iCacheIndex).wfd.nFileSizeHigh;

	if(Selected)
	{
		if(IsFolder)
			m_NumFoldersSelected++;
		else
			m_NumFilesSelected++;

		m_ulFileSelectionSize.QuadPart += ulFileSize.QuadPart;
	}
	else
	{
		if(IsFolder)
			m_NumFoldersSelected--;
		else
			m_NumFilesSelected--;

		m_ulFileSelectionSize.QuadPart -= ulFileSize.QuadPart;
	}
}

BOOL CShellBrowser::IsFilenameFiltered(const TCHAR *FileName) const
{
	if(CheckWildcardMatch(m_folderSettings.filter.c_str(),FileName,m_folderSettings.filterCaseSensitive))
		return FALSE;

	return TRUE;
}

int CShellBrowser::GetItemDisplayName(int iItem,UINT BufferSize,TCHAR *Buffer) const
{
	int internalIndex = GetItemInternalIndex(iItem);
	StringCchCopy(Buffer,BufferSize,m_itemInfoMap.at(internalIndex).wfd.cFileName);

	return lstrlen(Buffer);
}

HRESULT CShellBrowser::GetItemFullName(int iIndex,TCHAR *FullItemPath,UINT cchMax) const
{
	LVITEM			lvItem;
	BOOL			bRes;

	lvItem.mask		= LVIF_PARAM;
	lvItem.iItem	= iIndex;
	lvItem.iSubItem	= 0;
	bRes = ListView_GetItem(m_hListView,&lvItem);

	if(bRes)
	{
		QueryFullItemNameInternal((int)lvItem.lParam,FullItemPath,cchMax);

		return S_OK;
	}

	return E_FAIL;
}

void CShellBrowser::QueryFullItemNameInternal(int iItemInternal,TCHAR *szFullFileName,UINT cchMax) const
{
	GetDisplayName(m_itemInfoMap.at(iItemInternal).pidlComplete.get(),szFullFileName,cchMax,SHGDN_FORPARSING);
}

std::wstring CShellBrowser::GetDirectory() const
{
	return m_CurDir;
}

unique_pidl_absolute CShellBrowser::GetDirectoryIdl() const
{
	unique_pidl_absolute pidlDirectory(ILCloneFull(m_directoryState.pidlDirectory.get()));
	return pidlDirectory;
}

/* TODO: Convert to using pidl's here, rather than
file names. */
int CShellBrowser::SelectFiles(const TCHAR *FileNamePattern)
{
	int iItem;
	
	iItem = LocateFileItemIndex(FileNamePattern);

	if(iItem != -1)
	{
		NListView::ListView_FocusItem(m_hListView,iItem,TRUE);
		NListView::ListView_SelectItem(m_hListView,iItem,TRUE);
		ListView_EnsureVisible(m_hListView,iItem,FALSE);
		return 1;
	}

	return 0;
}

int CShellBrowser::LocateFileItemIndex(const TCHAR *szFileName) const
{
	LV_FINDINFO	lvFind;
	int			iItem;
	int			iInternalIndex;

	iInternalIndex = LocateFileItemInternalIndex(szFileName);

	if(iInternalIndex != -1)
	{
		lvFind.flags	= LVFI_PARAM;
		lvFind.lParam	= iInternalIndex;
		iItem			= ListView_FindItem(m_hListView,-1,&lvFind);

		return iItem;
	}

	return -1;
}

int CShellBrowser::LocateFileItemInternalIndex(const TCHAR *szFileName) const
{
	LVITEM	lvItem;
	int		i = 0;

	for(i = 0;i < m_nTotalItems;i++)
	{
		lvItem.mask		= LVIF_PARAM;
		lvItem.iItem	= i;
		lvItem.iSubItem	= 0;
		ListView_GetItem(m_hListView,&lvItem);

		if((lstrcmp(m_itemInfoMap.at((int)lvItem.lParam).wfd.cFileName,szFileName) == 0) ||
			(lstrcmp(m_itemInfoMap.at((int)lvItem.lParam).wfd.cAlternateFileName,szFileName) == 0))
		{
			return (int)lvItem.lParam;
			break;
		}
	}

	return -1;
}

boost::optional<int> CShellBrowser::LocateItemByInternalIndex(int internalIndex) const
{
	LVFINDINFO lvfi;
	lvfi.flags = LVFI_PARAM;
	lvfi.lParam = internalIndex;
	int item = ListView_FindItem(m_hListView, -1, &lvfi);

	if (item == -1)
	{
		return boost::none;
	}

	return item;
}

WIN32_FIND_DATA CShellBrowser::GetItemFileFindData(int iItem) const
{
	int internalIndex = GetItemInternalIndex(iItem);
	return m_itemInfoMap.at(internalIndex).wfd;
}

void CShellBrowser::DragStarted(int iFirstItem,POINT *ptCursor)
{
	DraggedFile_t	df;
	int				iSelected = -1;

	if(iFirstItem != -1)
	{
		POINT	ptOrigin;
		POINT	ptItem;

		ListView_GetItemPosition(m_hListView,iFirstItem,&ptItem);

		ListView_GetOrigin(m_hListView,&ptOrigin);

		m_ptDraggedOffset.x = ptOrigin.x + ptCursor->x - ptItem.x;
		m_ptDraggedOffset.y = ptOrigin.y + ptCursor->y - ptItem.y;
	}

	while((iSelected = ListView_GetNextItem(m_hListView,iSelected,LVNI_SELECTED)) != -1)
	{
		GetItemDisplayName(iSelected, SIZEOF_ARRAY(df.szFileName), df.szFileName);

		m_DraggedFilesList.push_back(df);
	}

	m_bDragging = TRUE;
}

void CShellBrowser::DragStopped(void)
{
	m_DraggedFilesList.clear();

	m_bDragging = FALSE;
}

unique_pidl_absolute CShellBrowser::GetItemCompleteIdl(int iItem) const
{
	LVITEM lvItem;
	lvItem.mask = LVIF_PARAM;
	lvItem.iItem = iItem;
	lvItem.iSubItem = 0;
	BOOL bRet = ListView_GetItem(m_hListView, &lvItem);

	if (!bRet)
	{
		return nullptr;
	}

	unique_pidl_absolute pidlComplete(ILCombine(m_directoryState.pidlDirectory.get(), m_itemInfoMap.at((int)lvItem.lParam).pridl.get()));

	return pidlComplete;
}

unique_pidl_child CShellBrowser::GetItemChildIdl(int iItem) const
{
	LVITEM lvItem;
	BOOL bRet;

	lvItem.mask		= LVIF_PARAM;
	lvItem.iItem	= iItem;
	lvItem.iSubItem	= 0;
	bRet = ListView_GetItem(m_hListView,&lvItem);

	if (!bRet)
	{
		return nullptr;
	}

	unique_pidl_child pidlRelative(ILCloneChild(m_itemInfoMap.at((int)lvItem.lParam).pridl.get()));

	return pidlRelative;
}

BOOL CShellBrowser::InVirtualFolder(void) const
{
	return m_bVirtualFolder;
}

/* We can create files in this folder if it is
part of the filesystem, or if it is the root of
the namespace (i.e. the desktop). */
BOOL CShellBrowser::CanCreate(void) const
{
	BOOL bCanCreate = FALSE;
	unique_pidl_absolute pidl;
	HRESULT hr = SHGetFolderLocation(NULL,CSIDL_DESKTOP,NULL,0,wil::out_param(pidl));

	if(SUCCEEDED(hr))
	{
		bCanCreate = !InVirtualFolder() || CompareIdls(m_directoryState.pidlDirectory.get(),pidl.get());
	}

	return bCanCreate;
}

void CShellBrowser::SetDirMonitorId(int iDirMonitorId)
{
	m_iDirMonitorId = iDirMonitorId;
}

int CShellBrowser::GetDirMonitorId(void) const
{
	return m_iDirMonitorId;
}

BOOL CShellBrowser::CompareVirtualFolders(UINT uFolderCSIDL) const
{
	TCHAR	szParsingPath[MAX_PATH];

	GetCsidlDisplayName(uFolderCSIDL,szParsingPath,SIZEOF_ARRAY(szParsingPath),SHGDN_FORPARSING);

	if(StrCmp(m_CurDir,szParsingPath) == 0)
		return TRUE;

	return FALSE;
}

int CShellBrowser::GenerateUniqueItemId(void)
{
	return m_directoryState.itemIDCounter++;
}

void CShellBrowser::PositionDroppedItems(void)
{
	std::list<DroppedFile_t>::iterator	itr;
	BOOL							bDropItemSet = FALSE;
	int								iItem;

	/* LVNI_TOLEFT and LVNI_TORIGHT cause exceptions
	in details view. */
	if(m_folderSettings.viewMode == +ViewMode::Details)
	{
		m_DroppedFileNameList.clear();
		return;
	}

	if(!m_DroppedFileNameList.empty())
	{
		/* The auto arrange style must be off for the items
		to be moved. Therefore, if the style is on, turn it
		off, move the items, and the turn it back on. */
		if(m_folderSettings.autoArrange)
			NListView::ListView_SetAutoArrange(m_hListView,FALSE);

		for(itr = m_DroppedFileNameList.begin();itr != m_DroppedFileNameList.end();)
		{
			iItem = LocateFileItemIndex(itr->szFileName);

			if(iItem != -1)
			{
				if(!bDropItemSet)
					m_iDropped = iItem;

				if(m_folderSettings.autoArrange)
				{
					/* TODO: Merge this code with RepositionLocalFiles(). */
					LVFINDINFO lvfi;
					LVHITTESTINFO lvhti;
					RECT rcItem;
					POINT ptOrigin;
					POINT pt;
					POINT ptNext;
					BOOL bRowEnd = FALSE;
					BOOL bRowStart = FALSE;
					int iNext;
					int iHitItem;
					int nItems;

					pt = itr->DropPoint;

					ListView_GetOrigin(m_hListView,&ptOrigin);
					pt.x -= ptOrigin.x;
					pt.y -= ptOrigin.y;

					lvhti.pt = pt;
					iHitItem = ListView_HitTest(m_hListView,&lvhti);

					/* Based on ListView_HandleInsertionMark() code. */
					if(iHitItem != -1 && lvhti.flags & LVHT_ONITEM)
					{
						ListView_GetItemRect(m_hListView,lvhti.iItem,&rcItem,LVIR_BOUNDS);

						if((pt.x - rcItem.left) >
							((rcItem.right - rcItem.left)/2))
						{
							iNext = iHitItem;
						}
						else
						{
							/* Can just insert the item _after_ the item to the
							left, unless this is the start of a row. */
							iNext = ListView_GetNextItem(m_hListView,iHitItem,LVNI_TOLEFT);

							if(iNext == -1)
								iNext = iHitItem;

							bRowStart = (ListView_GetNextItem(m_hListView,iNext,LVNI_TOLEFT) == -1);
						}
					}
					else
					{
						lvfi.flags			= LVFI_NEARESTXY;
						lvfi.pt				= pt;
						lvfi.vkDirection	= VK_UP;
						iNext = ListView_FindItem(m_hListView,-1,&lvfi);

						if(iNext == -1)
						{
							lvfi.flags			= LVFI_NEARESTXY;
							lvfi.pt				= pt;
							lvfi.vkDirection	= VK_LEFT;
							iNext = ListView_FindItem(m_hListView,-1,&lvfi);
						}

						ListView_GetItemRect(m_hListView,iNext,&rcItem,LVIR_BOUNDS);

						if(pt.x > rcItem.left +
							((rcItem.right - rcItem.left)/2))
						{
							if(pt.y > rcItem.bottom)
							{
								int iBelow;

								iBelow = ListView_GetNextItem(m_hListView,iNext,LVNI_BELOW);

								if(iBelow != -1)
									iNext = iBelow;
							}

							bRowEnd = TRUE;
						}

						nItems = ListView_GetItemCount(m_hListView);

						ListView_GetItemRect(m_hListView,nItems - 1,&rcItem,LVIR_BOUNDS);

						if((pt.x > rcItem.left + ((rcItem.right - rcItem.left)/2)) &&
							pt.x < rcItem.right + ((rcItem.right - rcItem.left)/2) + 2 &&
							pt.y > rcItem.top)
						{
							iNext = nItems - 1;

							bRowEnd = TRUE;
						}

						if(!bRowEnd)
						{
							int iLeft;

							iLeft = ListView_GetNextItem(m_hListView,iNext,LVNI_TOLEFT);

							if(iLeft != -1)
								iNext = iLeft;
							else
								bRowStart = TRUE;
						}
					}

					ListView_GetItemPosition(m_hListView,iNext,&ptNext);

					/* Offset by 1 pixel in the x-direction. This ensures that
					the dropped item will always be placed AFTER iNext. */
					if(bRowStart)
					{
						/* If at the start of a row, simply place at x = 0
						so that dropped item will be placed before first
						item... */
						ListView_SetItemPosition32(m_hListView,
							iItem,0,ptNext.y);
					}
					else
					{
						ListView_SetItemPosition32(m_hListView,
							iItem,ptNext.x + 1,ptNext.y);
					}
				}
				else
				{
					ListView_SetItemPosition32(m_hListView,iItem,itr->DropPoint.x,itr->DropPoint.y);
				}

				NListView::ListView_SelectItem(m_hListView,iItem,TRUE);
				NListView::ListView_FocusItem(m_hListView,iItem,TRUE);

				itr = m_DroppedFileNameList.erase(itr);
			}
			else
			{
				++itr;
			}
		}

		if(m_folderSettings.autoArrange)
			NListView::ListView_SetAutoArrange(m_hListView,TRUE);
	}
}

int CShellBrowser::DetermineItemSortedPosition(LPARAM lParam) const
{
	LVITEM	lvItem;
	BOOL	bItem;
	int		res = 1;
	int		nItems = 0;
	int		i = 0;

	nItems = ListView_GetItemCount(m_hListView);

	while(res > 0 && i < nItems)
	{
		lvItem.mask		= LVIF_PARAM;
		lvItem.iItem	= i;
		lvItem.iSubItem	= 0;
		bItem = ListView_GetItem(m_hListView,&lvItem);

		if(bItem)
			res = Sort(static_cast<int>(lParam),static_cast<int>(lvItem.lParam));
		else
			res = 0;

		i++;
	}

	/* The item will always be inserted BEFORE
	the item at position i that we specify here.
	For example, specifying 0, will place the item
	at 0 (and push 0 to 1), specifying 1 will place
	the item at 1 (and push 1 to 2).
	Therefore, to place in the last position, need
	to place AFTER last item. */
	if((i - 1) == nItems - 1 && res > 0)
	{
		i++;
	}

	return i - 1;
}

void CShellBrowser::RemoveFilteredItems(void)
{
	if (!m_folderSettings.applyFilter)
	{
		return;
	}

	int nItems = ListView_GetItemCount(m_hListView);

	for(int i = nItems - 1;i >= 0;i--)
	{
		int internalIndex = GetItemInternalIndex(i);

		if(!((m_itemInfoMap.at(internalIndex).wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == FILE_ATTRIBUTE_DIRECTORY))
		{
			if(IsFilenameFiltered(m_itemInfoMap.at(internalIndex).szDisplayName))
			{
				RemoveFilteredItem(i, internalIndex);
			}
		}
	}

	SendMessage(m_hOwner,WM_USER_UPDATEWINDOWS,0,0);
}

void CShellBrowser::RemoveFilteredItem(int iItem,int iItemInternal)
{
	ULARGE_INTEGER	ulFileSize;

	if(ListView_GetItemState(m_hListView,iItem,LVIS_SELECTED)
		== LVIS_SELECTED)
	{
		ulFileSize.LowPart = m_itemInfoMap.at(iItemInternal).wfd.nFileSizeLow;
		ulFileSize.HighPart = m_itemInfoMap.at(iItemInternal).wfd.nFileSizeHigh;

		m_ulFileSelectionSize.QuadPart -= ulFileSize.QuadPart;
	}

	/* Take the file size of the removed file away from the total
	directory size. */
	ulFileSize.LowPart = m_itemInfoMap.at(iItemInternal).wfd.nFileSizeLow;
	ulFileSize.HighPart = m_itemInfoMap.at(iItemInternal).wfd.nFileSizeHigh;

	m_ulTotalDirSize.QuadPart -= ulFileSize.QuadPart;

	/* Remove the item from the m_hListView. */
	ListView_DeleteItem(m_hListView,iItem);

	m_nTotalItems--;

	m_FilteredItemsList.push_back(iItemInternal);
}

int CShellBrowser::GetNumItems(void) const
{
	return m_nTotalItems;
}

int CShellBrowser::GetNumSelectedFiles(void) const
{
	return m_NumFilesSelected;
}

int CShellBrowser::GetNumSelectedFolders(void) const
{
	return m_NumFoldersSelected;
}

int CShellBrowser::GetNumSelected(void) const
{
	return m_NumFilesSelected + m_NumFoldersSelected;
}

void CShellBrowser::GetFolderInfo(FolderInfo_t *pFolderInfo)
{
	pFolderInfo->TotalFolderSize.QuadPart		= m_ulTotalDirSize.QuadPart;
	pFolderInfo->TotalSelectionSize.QuadPart	= m_ulFileSelectionSize.QuadPart;
}

void CShellBrowser::DetermineFolderVirtual(PCIDLIST_ABSOLUTE pidlDirectory)
{
	TCHAR szParsingPath[MAX_PATH];

	m_bVirtualFolder = !SHGetPathFromIDList(pidlDirectory,szParsingPath);

	if(m_bVirtualFolder)
	{
		/* Mark the recycle bin and desktop as
		real folders. Shouldn't be able to create
		folders in Recycle Bin. */
		if(CompareVirtualFolders(CSIDL_BITBUCKET))
			m_bVirtualFolder = TRUE;
		else if(CompareVirtualFolders(CSIDL_DESKTOP))
			m_bVirtualFolder = FALSE;
	}
}

std::wstring CShellBrowser::GetFilter() const
{
	return m_folderSettings.filter;
}

void CShellBrowser::SetFilter(std::wstring_view filter)
{
	m_folderSettings.filter = filter;

	if(m_folderSettings.applyFilter)
	{
		UnfilterAllItems();
		UpdateFiltering();
	}
}

void CShellBrowser::SetFilterStatus(BOOL bFilter)
{
	m_folderSettings.applyFilter = bFilter;

	UpdateFiltering();
}

BOOL CShellBrowser::GetFilterStatus(void) const
{
	return m_folderSettings.applyFilter;
}

void CShellBrowser::SetFilterCaseSensitive(BOOL bFilterCaseSensitive)
{
	m_folderSettings.filterCaseSensitive = bFilterCaseSensitive;
}

BOOL CShellBrowser::GetFilterCaseSensitive(void) const
{
	return m_folderSettings.filterCaseSensitive;
}

void CShellBrowser::UpdateFiltering(void)
{
	if(m_folderSettings.applyFilter)
	{
		RemoveFilteredItems();

		ApplyFilteringBackgroundImage(true);
	}
	else
	{
		UnfilterAllItems();

		if(m_nTotalItems == 0)
			ApplyFolderEmptyBackgroundImage(true);
		else
			ApplyFilteringBackgroundImage(false);
	}
}

void CShellBrowser::UnfilterAllItems(void)
{
	std::list<int>::iterator	itr;
	AwaitingAdd_t		AwaitingAdd;

	for(itr = m_FilteredItemsList.begin();itr != m_FilteredItemsList.end();itr++)
	{
		int iSorted = DetermineItemSortedPosition(*itr);

		AwaitingAdd.iItem			= iSorted;
		AwaitingAdd.bPosition		= TRUE;
		AwaitingAdd.iAfter			= iSorted - 1;
		AwaitingAdd.iItemInternal	= *itr;

		m_AwaitingAddList.push_back(AwaitingAdd);
	}

	m_FilteredItemsList.clear();

	InsertAwaitingItems(m_folderSettings.showInGroups);

	SendMessage(m_hOwner,WM_USER_UPDATEWINDOWS,0,0);
}

void CShellBrowser::VerifySortMode()
{
	const std::vector<Column_t> *columns = nullptr;

	if(CompareVirtualFolders(CSIDL_CONTROLS))
	{
		columns = &m_folderColumns.controlPanelColumns;
	}
	else if(CompareVirtualFolders(CSIDL_DRIVES))
	{
		columns = &m_folderColumns.myComputerColumns;
	}
	else if(CompareVirtualFolders(CSIDL_BITBUCKET))
	{
		columns = &m_folderColumns.recycleBinColumns;
	}
	else if(CompareVirtualFolders(CSIDL_PRINTERS))
	{
		columns = &m_folderColumns.printersColumns;
	}
	else if(CompareVirtualFolders(CSIDL_CONNECTIONS))
	{
		columns = &m_folderColumns.networkConnectionsColumns;
	}
	else if (CompareVirtualFolders(CSIDL_NETWORK))
	{
		columns = &m_folderColumns.myNetworkPlacesColumns;
	}
	else
	{
		columns = &m_folderColumns.realFolderColumns;
	}

	auto itr = std::find_if(columns->begin(), columns->end(), [sortMode = m_folderSettings.sortMode] (const Column_t &column) {
		return column.id == static_cast<unsigned int>(sortMode);
	});

	if (itr != columns->end())
	{
		return;
	}

	auto firstChecked = std::find_if(columns->begin(), columns->end(), [] (const Column_t &column) {
		return column.bChecked;
	});

	// There should always be at least one checked column, so firstChecked
	// should always be valid here.
	m_folderSettings.sortMode = DetermineColumnSortMode(firstChecked->id);
}

BOOL CShellBrowser::GetSortAscending(void) const
{
	return m_folderSettings.sortAscending;
}

BOOL CShellBrowser::SetSortAscending(BOOL bAscending)
{
	m_folderSettings.sortAscending = bAscending;

	return m_folderSettings.sortAscending;
}

BOOL CShellBrowser::GetShowHidden(void) const
{
	return m_folderSettings.showHidden;
}

BOOL CShellBrowser::SetShowHidden(BOOL bShowHidden)
{
	m_folderSettings.showHidden = bShowHidden;

	return m_folderSettings.showHidden;
}

BOOL CShellBrowser::IsDragging() const
{
	return m_bPerformingDrag;
}

std::vector<SortMode> CShellBrowser::GetAvailableSortModes() const
{
	std::vector<SortMode> sortModes;

	for (const auto &column : *m_pActiveColumns)
	{
		if(column.bChecked)
		{
			sortModes.push_back(DetermineColumnSortMode(column.id));
		}
	}

	return sortModes;
}

/* Queues an in-place rename for an item.
This method is used when a file is created
using the shell new menu, and the item
may or may not have been inserted into
the listview yet. */
void CShellBrowser::QueueRename(PCIDLIST_ABSOLUTE pidlItem)
{
	/* Items are inserted within the context
	of this thread. Therefore, either pending
	items have already been inserted, or they
	have yet to be inserted.
	First, look for the file using it's display
	name. If the file is not found, set a flag
	indicating that when items are inserted,
	they should be checked against this item.
	Once a match is found, rename the item
	in-place within the listview. */

	TCHAR szItem[MAX_PATH];
	LVITEM lvItem;
	BOOL bItemFound = FALSE;
	int nItems;
	int i = 0;

	GetDisplayName(pidlItem,szItem,SIZEOF_ARRAY(szItem),SHGDN_INFOLDER);

	nItems = ListView_GetItemCount(m_hListView);

	for(i = 0;i < nItems;i++)
	{
		lvItem.mask		= LVIF_PARAM;
		lvItem.iItem	= i;
		lvItem.iSubItem	= 0;
		ListView_GetItem(m_hListView,&lvItem);

		if(CompareIdls(pidlItem, m_itemInfoMap.at((int)lvItem.lParam).pidlComplete.get()))
		{
			bItemFound = TRUE;

			ListView_EditLabel(m_hListView,i);
			break;
		}
	}

	if(!bItemFound)
	{
		m_bNewItemCreated = TRUE;
		m_pidlNewItem = ILCloneFull(pidlItem);
	}
}

void CShellBrowser::SelectItems(const std::list<std::wstring> &PastedFileList)
{
	int i = 0;

	m_FileSelectionList.clear();

	for(const auto &PastedFile : PastedFileList)
	{
		int iIndex = LocateFileItemIndex(PastedFile.c_str());

		if(iIndex != -1)
		{
			NListView::ListView_SelectItem(m_hListView,iIndex,TRUE);

			if(i == 0)
			{
				/* Focus on the first item, and ensure it is visible. */
				NListView::ListView_FocusItem(m_hListView,iIndex,TRUE);
				ListView_EnsureVisible(m_hListView,iIndex,FALSE);

				i++;
			}
		}
		else
		{
			m_FileSelectionList.push_back(PastedFile);
		}
	}
}

void CShellBrowser::OnDeviceChange(WPARAM wParam,LPARAM lParam)
{
	/* Note changes made here may have no effect. Since
	the icon for the cd/dvd/etc. may not have been
	updated by the time this function is called, it's
	possible this may not change anything. */

	/* If we are currently not in my computer, this
	message can be safely ignored (drives are only
	shown in my computer). */
	if(CompareVirtualFolders(CSIDL_DRIVES))
	{
		switch(wParam)
		{
			/* Device has being added/inserted into the system. Update the
			drives toolbar as necessary. */
		case DBT_DEVICEARRIVAL:
			{
				DEV_BROADCAST_HDR *dbh = NULL;

				dbh = (DEV_BROADCAST_HDR *)lParam;

				if(dbh->dbch_devicetype == DBT_DEVTYP_VOLUME)
				{
					DEV_BROADCAST_VOLUME	*pdbv = NULL;
					TCHAR					chDrive;
					TCHAR					szDrive[4];

					pdbv = (DEV_BROADCAST_VOLUME *)dbh;

					/* Build a string that will form the drive name. */
					chDrive = GetDriveLetterFromMask(pdbv->dbcv_unitmask);
					StringCchPrintf(szDrive,SIZEOF_ARRAY(szDrive),
						_T("%c:\\"),chDrive);

					if(pdbv->dbcv_flags & DBTF_MEDIA)
					{
						UpdateDriveIcon(szDrive);
					}
					else
					{
						OnFileActionAdded(szDrive);
					}
				}
			}
			break;

		case DBT_DEVICEREMOVECOMPLETE:
			{
				DEV_BROADCAST_HDR				*dbh = NULL;

				dbh = (DEV_BROADCAST_HDR *)lParam;

				if(dbh->dbch_devicetype == DBT_DEVTYP_VOLUME)
				{
					DEV_BROADCAST_VOLUME	*pdbv = NULL;
					TCHAR					chDrive;
					TCHAR					szDrive[4];

					pdbv = (DEV_BROADCAST_VOLUME *)dbh;

					/* Build a string that will form the drive name. */
					chDrive = GetDriveLetterFromMask(pdbv->dbcv_unitmask);
					StringCchPrintf(szDrive,SIZEOF_ARRAY(szDrive),_T("%c:\\"),chDrive);

					/* The device was removed from the system.
					Remove it from the listview (only if the drive
					was actually removed - the drive may not have
					been removed, for example, if a cd/dvd was
					changed). */
					if(pdbv->dbcv_flags & DBTF_MEDIA)
					{
						UpdateDriveIcon(szDrive);
					}
					else
					{
						/* At this point, the drive has been completely removed
						from the system. Therefore, its display name cannot be
						queried. Need to search for the drive using ONLY its
						drive letter/name. Once its index in the listview has
						been determined, it can simply be removed. */
						RemoveDrive(szDrive);
					}
				}
			}
			break;
		}
	}
}

void CShellBrowser::UpdateDriveIcon(const TCHAR *szDrive)
{
	LVITEM					lvItem;
	SHFILEINFO				shfi;
	TCHAR					szDisplayName[MAX_PATH];
	HRESULT					hr;
	int						iItem = -1;
	int						iItemInternal = -1;
	int						i = 0;

	/* Look for the item using its display name, NOT
	its drive letter/name. */
	GetDisplayName(szDrive,szDisplayName,SIZEOF_ARRAY(szDisplayName),SHGDN_INFOLDER);

	unique_pidl_absolute pidlDrive;
	hr = SHParseDisplayName(szDrive, nullptr, wil::out_param(pidlDrive), 0, nullptr);

	if(SUCCEEDED(hr))
	{
		for(i = 0;i < m_nTotalItems;i++)
		{
			lvItem.mask		= LVIF_PARAM;
			lvItem.iItem	= i;
			lvItem.iSubItem	= 0;
			ListView_GetItem(m_hListView,&lvItem);

			if(CompareIdls(pidlDrive.get(), m_itemInfoMap.at((int)lvItem.lParam).pidlComplete.get()))
			{
				iItem = i;
				iItemInternal = (int)lvItem.lParam;

				break;
			}
		}
	}

	if(iItem != -1)
	{
		SHGetFileInfo(szDrive,0,&shfi,sizeof(shfi),SHGFI_SYSICONINDEX);

		StringCchCopy(m_itemInfoMap.at(iItemInternal).szDisplayName,
			SIZEOF_ARRAY(m_itemInfoMap.at(iItemInternal).szDisplayName),
			szDisplayName);

		/* Update the drives icon and display name. */
		lvItem.mask		= LVIF_TEXT|LVIF_IMAGE;
		lvItem.iImage	= shfi.iIcon;
		lvItem.iItem	= iItem;
		lvItem.iSubItem	= 0;
		lvItem.pszText	= szDisplayName;
		ListView_SetItem(m_hListView,&lvItem);
	}
}

void CShellBrowser::RemoveDrive(const TCHAR *szDrive)
{
	LVITEM lvItem;
	int iItemInternal = -1;
	int i = 0;

	for(i = 0;i < m_nTotalItems;i++)
	{
		lvItem.mask		= LVIF_PARAM;
		lvItem.iItem	= i;
		lvItem.iSubItem	= 0;
		ListView_GetItem(m_hListView,&lvItem);

		if(m_itemInfoMap.at((int)lvItem.lParam).bDrive)
		{
			if(lstrcmp(szDrive,m_itemInfoMap.at((int)lvItem.lParam).szDrive) == 0)
			{
				iItemInternal = (int)lvItem.lParam;
				break;
			}
		}
	}

	if(iItemInternal != -1)
		RemoveItem(iItemInternal);
}

int CShellBrowser::GetUniqueFolderId() const
{
	return m_uniqueFolderId;
}

BasicItemInfo_t CShellBrowser::getBasicItemInfo(int internalIndex) const
{
	const ItemInfo_t &itemInfo = m_itemInfoMap.at(internalIndex);

	BasicItemInfo_t basicItemInfo;
	basicItemInfo.pidlComplete.reset(ILCloneFull(itemInfo.pidlComplete.get()));
	basicItemInfo.pridl.reset(ILCloneChild(itemInfo.pridl.get()));
	basicItemInfo.wfd = itemInfo.wfd;
	StringCchCopy(basicItemInfo.szDisplayName, SIZEOF_ARRAY(basicItemInfo.szDisplayName), itemInfo.szDisplayName);
	basicItemInfo.isRoot = itemInfo.bDrive;

	return basicItemInfo;
}

HWND CShellBrowser::GetListView() const
{
	return m_hListView;
}

IconFetcher *CShellBrowser::GetIconFetcher()
{
	return m_iconFetcher.get();
}

FolderSettings CShellBrowser::GetFolderSettings() const
{
	return m_folderSettings;
}