/******************************************************************
 *
 * Project: ShellBrowser
 * File: BrowsingHandler.cpp
 * License: GPL - See LICENSE in the top level directory
 *
 * Handles the browsing of directories.
 *
 * Written by David Erceg
 * www.explorerplusplus.com
 *
 *****************************************************************/

#include "stdafx.h"
#include <list>
#include "IShellView.h"
#include "iShellBrowser_internal.h"
#include "../Helper/Controls.h"
#include "../Helper/Helper.h"
#include "../Helper/FileOperations.h"
#include "../Helper/FolderSize.h"
#include "../Helper/ShellHelper.h"
#include "../Helper/ListViewHelper.h"
#include "../Helper/Macros.h"


HRESULT CShellBrowser::BrowseFolder(const TCHAR *szPath,UINT wFlags)
{
	LPITEMIDLIST pidlDirectory = NULL;
	HRESULT hr = GetIdlFromParsingName(szPath,&pidlDirectory);

	if(SUCCEEDED(hr))
	{
		hr = BrowseFolder(pidlDirectory,wFlags);

		CoTaskMemFree(pidlDirectory);
	}

	return hr;
}

HRESULT CShellBrowser::BrowseFolder(LPCITEMIDLIST pidlDirectory,UINT wFlags)
{
	SetCursor(LoadCursor(NULL,IDC_WAIT));

	LPITEMIDLIST pidl = ILClone(pidlDirectory);

	if(m_bFolderVisited)
	{
		SaveColumnWidths();
	}

	/* The path may not be absolute, in which case it will
	need to be completed. */
	BOOL StoreHistory = TRUE;
	HRESULT hr = ParsePath(&pidl,wFlags,&StoreHistory);

	if(hr != S_OK)
	{
		SetCursor(LoadCursor(NULL,IDC_ARROW));
		return E_FAIL;
	}

	EmptyIconFinderQueue();
	EmptyThumbnailsQueue();
	EmptyColumnQueue();
	EmptyFolderQueue();

	/* TODO: Wait for any background threads to finish processing. */

	EnterCriticalSection(&m_csDirectoryAltered);
	m_FilesAdded.clear();
	m_FileSelectionList.clear();
	LeaveCriticalSection(&m_csDirectoryAltered);

	TCHAR szParsingPath[MAX_PATH];
	GetDisplayName(pidl,szParsingPath,SIZEOF_ARRAY(szParsingPath),SHGDN_FORPARSING);

	/* TODO: Method callback. */
	SendMessage(m_hOwner,WM_USER_STARTEDBROWSING,m_ID,reinterpret_cast<WPARAM>(szParsingPath));

	StringCchCopy(m_CurDir,SIZEOF_ARRAY(m_CurDir),szParsingPath);

	if(StoreHistory)
	{
		m_pathManager.StoreIdl(pidl);
	}

	/* Stop the list view from redrawing itself each time is inserted.
	Redrawing will be allowed once all items have being inserted.
	(reduces lag when a large number of items are going to be inserted). */
	SendMessage(m_hListView, WM_SETREDRAW, FALSE, NULL);

	ListView_DeleteAllItems(m_hListView);

	if(m_bFolderVisited)
	{
		ResetFolderMemoryAllocations();
	}

	m_nTotalItems = 0;

	BrowseVirtualFolder(pidl);

	CoTaskMemFree(pidl);

	/* Window updates needs these to be set. */
	m_NumFilesSelected		= 0;
	m_NumFoldersSelected	= 0;

	m_ulTotalDirSize.QuadPart = 0;
	m_ulFileSelectionSize.QuadPart = 0;

	SetActiveColumnSet();
	SetCurrentViewModeInternal(m_ViewMode);

	InsertAwaitingItems(FALSE);

	VerifySortMode();
	SortFolder(m_SortMode);

	ListView_EnsureVisible(m_hListView,0,FALSE);

	/* Allow the listview to redraw itself once again. */
	SendMessage(m_hListView,WM_SETREDRAW,TRUE,NULL);

	m_bFolderVisited = TRUE;

	SetCursor(LoadCursor(NULL,IDC_ARROW));

	m_iUniqueFolderIndex++;

	return S_OK;
}

void inline CShellBrowser::InsertAwaitingItems(BOOL bInsertIntoGroup)
{
	LVITEM lv;
	ULARGE_INTEGER ulFileSize;
	unsigned int nPrevItems;
	int nAdded = 0;
	int iItemIndex;

	nPrevItems = ListView_GetItemCount(m_hListView);

	m_nAwaitingAdd = (int)m_AwaitingAddList.size();

	if((nPrevItems + m_nAwaitingAdd) == 0)
	{
		if(m_bApplyFilter)
			SendMessage(m_hOwner,WM_USER_FILTERINGAPPLIED,m_ID,TRUE);
		else
			SendMessage(m_hOwner,WM_USER_FOLDEREMPTY,m_ID,TRUE);

		m_nTotalItems = 0;

		return;
	}
	else if(!m_bApplyFilter)
	{
		SendMessage(m_hOwner,WM_USER_FOLDEREMPTY,m_ID,FALSE);
	}

	/* Make the listview allocate space (for internal data structures)
	for all the items at once, rather than individually.
	Acts as a speed optimization. */
	ListView_SetItemCount(m_hListView,m_nAwaitingAdd + nPrevItems);

	lv.mask			= LVIF_TEXT|LVIF_IMAGE|LVIF_PARAM;

	if(bInsertIntoGroup)
		lv.mask		|= LVIF_GROUPID;

	/* Constant for each item. */
	lv.iSubItem		= 0;

	if(m_bAutoArrange)
		NListView::ListView_SetAutoArrange(m_hListView,FALSE);

	for(auto itr = m_AwaitingAddList.begin();itr != m_AwaitingAddList.end();itr++)
	{
		if(!IsFileFiltered(itr->iItemInternal))
		{
			std::wstring filename = ProcessItemFileName(itr->iItemInternal);

			TCHAR filenameCopy[MAX_PATH];
			StringCchCopy(filenameCopy, SIZEOF_ARRAY(filenameCopy), filename.c_str());

			lv.iItem	= itr->iItem;
			lv.pszText	= filenameCopy;
			lv.iImage	= I_IMAGECALLBACK;
			lv.lParam	= itr->iItemInternal;

			if(bInsertIntoGroup)
			{
				lv.iGroupId	= DetermineItemGroup(itr->iItemInternal);
			}

			/* Insert the item into the list view control. */
			iItemIndex = ListView_InsertItem(m_hListView,&lv);

			if(itr->bPosition && m_ViewMode != VM_DETAILS)
			{
				POINT ptItem;

				if(itr->iAfter != -1)
				{
					ListView_GetItemPosition(m_hListView,itr->iAfter,&ptItem);
				}
				else
				{
					ptItem.x = 0;
					ptItem.y = 0;
				}

				/* The item will end up in the position AFTER iAfter. */
				ListView_SetItemPosition32(m_hListView,iItemIndex,ptItem.x,ptItem.y);
			}

			if(m_ViewMode == VM_TILES)
			{
				SetTileViewItemInfo(iItemIndex,itr->iItemInternal);
			}

			if(m_bNewItemCreated)
			{
				LPITEMIDLIST pidlComplete = NULL;

				pidlComplete = ILCombine(m_pidlDirectory,m_extraItemInfoMap.at((int)itr->iItemInternal).pridl);

				if(CompareIdls(pidlComplete,m_pidlNewItem))
					m_bNewItemCreated = FALSE;

				m_iIndexNewItem = iItemIndex;

				CoTaskMemFree(pidlComplete);
			}

			/* If the file is marked as hidden, ghost it out. */
			if(m_fileInfoMap.at(itr->iItemInternal).dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)
			{
				ListView_SetItemState(m_hListView,iItemIndex,LVIS_CUT,LVIS_CUT);
			}
			
			/* Add the current file's size to the running size of the current directory. */
			/* A folder may or may not have 0 in its high file size member.
			It should either be zeroed, or never counted. */
			ulFileSize.LowPart = m_fileInfoMap.at(itr->iItemInternal).nFileSizeLow;
			ulFileSize.HighPart = m_fileInfoMap.at(itr->iItemInternal).nFileSizeHigh;

			m_ulTotalDirSize.QuadPart += ulFileSize.QuadPart;

			nAdded++;
		}
		else
		{
			m_FilteredItemsList.push_back(itr->iItemInternal);
		}
	}

	if(m_bAutoArrange)
		NListView::ListView_SetAutoArrange(m_hListView,TRUE);

	m_nTotalItems = nPrevItems + nAdded;

	if(m_ViewMode == VM_DETAILS)
	{
		TCHAR szDrive[MAX_PATH];
		BOOL bNetworkRemovable = FALSE;

		QueueUserAPC(SetAllColumnDataAPC,m_hThread,(ULONG_PTR)this);

		StringCchCopy(szDrive,SIZEOF_ARRAY(szDrive),m_CurDir);
		PathStripToRoot(szDrive);

		if(GetDriveType(szDrive) == DRIVE_REMOVABLE ||
			GetDriveType(szDrive) == DRIVE_REMOTE)
		{
			bNetworkRemovable = TRUE;
		}

		/* If the user has selected to disable folder sizes
		on removable drives or networks, and we are currently
		on such a drive, do not calculate folder sizes. */
		if(m_bShowFolderSizes && !(m_bDisableFolderSizesNetworkRemovable && bNetworkRemovable))
			QueueUserAPC(SetAllFolderSizeColumnDataAPC,m_hFolderSizeThread,(ULONG_PTR)this);
	}

	PositionDroppedItems();

	m_AwaitingAddList.clear();
	m_nAwaitingAdd = 0;
}

BOOL CShellBrowser::IsFileFiltered(int iItemInternal) const
{
	BOOL bHideSystemFile	= FALSE;
	BOOL bFilenameFiltered	= FALSE;

	if(m_bApplyFilter &&
		((m_fileInfoMap.at(iItemInternal).dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != FILE_ATTRIBUTE_DIRECTORY))
	{
		bFilenameFiltered = IsFilenameFiltered(m_extraItemInfoMap.at(iItemInternal).szDisplayName);
	}

	if(m_bHideSystemFiles)
	{
		bHideSystemFile = (m_fileInfoMap.at(iItemInternal).dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)
			== FILE_ATTRIBUTE_SYSTEM;
	}

	return bFilenameFiltered || bHideSystemFile;
}

/* Processes an items filename. Essentially checks
if the extension (if any) needs to be removed, and
removes it if it does. */
std::wstring CShellBrowser::ProcessItemFileName(int iItemInternal) const
{
	BOOL bHideExtension = FALSE;
	TCHAR *pExt = NULL;

	if(m_bHideLinkExtension &&
		((m_fileInfoMap.at(iItemInternal).dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != FILE_ATTRIBUTE_DIRECTORY))
	{
		pExt = PathFindExtension(m_extraItemInfoMap.at(iItemInternal).szDisplayName);

		if(*pExt != '\0')
		{
			if(lstrcmpi(pExt,_T(".lnk")) == 0)
				bHideExtension = TRUE;
		}
	}

	/* We'll hide the extension, provided it is meant
	to be hidden, and the filename does not begin with
	a period, and the item is not a directory. */
	if((!m_bShowExtensions || bHideExtension) &&
		m_extraItemInfoMap.at(iItemInternal).szDisplayName[0] != '.' &&
		(m_fileInfoMap.at(iItemInternal).dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != FILE_ATTRIBUTE_DIRECTORY)
	{
		static TCHAR szDisplayName[MAX_PATH];

		StringCchCopy(szDisplayName,SIZEOF_ARRAY(szDisplayName),
			m_extraItemInfoMap.at(iItemInternal).szDisplayName);

		/* Strip the extension. */
		PathRemoveExtension(szDisplayName);

		return szDisplayName;
	}
	else
	{
		return m_extraItemInfoMap.at(iItemInternal).szDisplayName;
	}
}

void CShellBrowser::RemoveItem(int iItemInternal)
{
	ULARGE_INTEGER	ulFileSize;
	LVFINDINFO		lvfi;
	BOOL			bFolder;
	int				iItem;
	int				nItems;

	if(iItemInternal == -1)
		return;

	CoTaskMemFree(m_extraItemInfoMap.at(iItemInternal).pridl);

	/* Is this item a folder? */
	bFolder = (m_fileInfoMap.at(iItemInternal).dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ==
	FILE_ATTRIBUTE_DIRECTORY;

	/* Take the file size of the removed file away from the total
	directory size. */
	ulFileSize.LowPart = m_fileInfoMap.at(iItemInternal).nFileSizeLow;
	ulFileSize.HighPart = m_fileInfoMap.at(iItemInternal).nFileSizeHigh;

	m_ulTotalDirSize.QuadPart -= ulFileSize.QuadPart;

	/* Locate the item within the listview.
	Could use filename, providing removed
	items are always deleted before new
	items are inserted. */
	lvfi.flags	= LVFI_PARAM;
	lvfi.lParam	= iItemInternal;
	iItem = ListView_FindItem(m_hListView,-1,&lvfi);
	
	if(iItem != -1)
	{
		/* Remove the item from the listview. */
		ListView_DeleteItem(m_hListView,iItem);
	}

	m_fileInfoMap.erase(iItemInternal);
	m_extraItemInfoMap.erase(iItemInternal);

	nItems = ListView_GetItemCount(m_hListView);

	m_nTotalItems--;

	if(nItems == 0 && !m_bApplyFilter)
	{
		SendMessage(m_hOwner,WM_USER_FOLDEREMPTY,m_ID,TRUE);
	}
}

HRESULT CShellBrowser::ParsePath(LPITEMIDLIST *pidlDirectory,UINT uFlags,
BOOL *bStoreHistory)
{
	if((uFlags & SBSP_RELATIVE) == SBSP_RELATIVE)
	{
		LPITEMIDLIST	pidlComplete;

		if(pidlDirectory == NULL)
			return E_INVALIDARG;

		/* This is a relative path. Add it on to the end of the current directory
		name to get a fully qualified path. */
		pidlComplete = ILCombine(m_pidlDirectory,*pidlDirectory);

		*pidlDirectory = ILClone(pidlComplete);

		CoTaskMemFree(pidlComplete);
	}
	else if((uFlags & SBSP_PARENT) == SBSP_PARENT)
	{
		HRESULT hr;

		hr = GetVirtualParentPath(m_pidlDirectory,pidlDirectory);
	}
	else if((uFlags & SBSP_NAVIGATEBACK) == SBSP_NAVIGATEBACK)
	{
		if(m_pathManager.GetNumBackPathsStored() == 0)
		{
			SetFocus(m_hListView);
			return E_FAIL;
		}

		/*Gets the path of the folder that was last visited.
		Ignores the supplied Path argument.*/
		*bStoreHistory		= FALSE;

		*pidlDirectory = m_pathManager.RetrieveAndValidateIdl(-1);
	}
	else if((uFlags & SBSP_NAVIGATEFORWARD) == SBSP_NAVIGATEFORWARD)
	{
		if(m_pathManager.GetNumForwardPathsStored() == 0)
		{
			SetFocus(m_hListView);
			return E_FAIL;
		}

		/*Gets the path of the folder that is 'forward' of
		this one. Ignores the supplied Path argument.*/
		*bStoreHistory		= FALSE;

		*pidlDirectory = m_pathManager.RetrieveAndValidateIdl(1);
	}
	else
	{
		/* Assume that SBSP_ABSOLUTE was passed. */
		if(pidlDirectory == NULL)
			return E_INVALIDARG;
	}
	
	if((uFlags & SBSP_WRITENOHISTORY) == SBSP_WRITENOHISTORY)
	{
		/* Client has requested that the folder to be browsed to will have
		no history item associated with it. */
		*bStoreHistory		= FALSE;
	}

	if(!CheckIdl(*pidlDirectory))
		return E_FAIL;

	return S_OK;
}

void CShellBrowser::BrowseVirtualFolder(LPITEMIDLIST pidlDirectory)
{
	IShellFolder	*pShellFolder = NULL;
	IEnumIDList		*pEnumIDList = NULL;
	LPITEMIDLIST	rgelt = NULL;
	STRRET			str;
	SHCONTF			EnumFlags;
	TCHAR			szFileName[MAX_PATH];
	ULONG			uFetched;
	HRESULT			hr;

	DetermineFolderVirtual(pidlDirectory);

	hr = BindToIdl(pidlDirectory, IID_PPV_ARGS(&pShellFolder));

	if(SUCCEEDED(hr))
	{
		m_pidlDirectory = ILClone(pidlDirectory);

		EnumFlags = SHCONTF_FOLDERS|SHCONTF_NONFOLDERS;

		if(m_bShowHidden)
			EnumFlags |= SHCONTF_INCLUDEHIDDEN;

		hr = pShellFolder->EnumObjects(m_hOwner,EnumFlags,&pEnumIDList);

		if(SUCCEEDED(hr) && pEnumIDList != NULL)
		{
			uFetched = 1;
			while(pEnumIDList->Next(1,&rgelt,&uFetched) == S_OK && (uFetched == 1))
			{
				ULONG uAttributes = SFGAO_FOLDER;

				pShellFolder->GetAttributesOf(1,(LPCITEMIDLIST *)&rgelt,&uAttributes);

				/* If this is a virtual folder, only use SHGDN_INFOLDER. If this is
				a real folder, combine SHGDN_INFOLDER with SHGDN_FORPARSING. This is
				so that items in real folders can still be shown with extensions, even
				if the global, Explorer option is disabled.
				Also use only SHGDN_INFOLDER if this item is a folder. This is to ensure
				that specific folders in Windows 7 (those under C:\Users\Username) appear
				correctly. */
				if(m_bVirtualFolder || (uAttributes & SFGAO_FOLDER))
					hr = pShellFolder->GetDisplayNameOf(rgelt,SHGDN_INFOLDER,&str);
				else
					hr = pShellFolder->GetDisplayNameOf(rgelt,SHGDN_INFOLDER|SHGDN_FORPARSING,&str);

				if(SUCCEEDED(hr))
				{
					StrRetToBuf(&str, rgelt, szFileName, SIZEOF_ARRAY(szFileName));

					AddItemInternal(pidlDirectory,rgelt,szFileName,-1,FALSE);
				}

				CoTaskMemFree((LPVOID)rgelt);
			}

			pEnumIDList->Release();
		}

		pShellFolder->Release();
	}
}

HRESULT inline CShellBrowser::AddItemInternal(LPITEMIDLIST pidlDirectory,
LPITEMIDLIST pidlRelative,const TCHAR *szFileName,int iItemIndex,BOOL bPosition)
{
	int uItemId;

	uItemId = SetItemInformation(pidlDirectory,pidlRelative,szFileName);

	return AddItemInternal(iItemIndex,uItemId,bPosition);
}

HRESULT inline CShellBrowser::AddItemInternal(int iItemIndex,int iItemId,BOOL bPosition)
{
	AwaitingAdd_t	AwaitingAdd;

	if(iItemIndex == -1)
		AwaitingAdd.iItem = m_nTotalItems + m_nAwaitingAdd - 1;
	else
		AwaitingAdd.iItem = iItemIndex;

	AwaitingAdd.iItemInternal = iItemId;
	AwaitingAdd.bPosition = bPosition;
	AwaitingAdd.iAfter = iItemIndex - 1;

	m_AwaitingAddList.push_back(AwaitingAdd);

	AddToColumnQueue(AwaitingAdd.iItem);
	AddToFolderQueue(AwaitingAdd.iItem);

	return S_OK;
}

int inline CShellBrowser::SetItemInformation(LPITEMIDLIST pidlDirectory,
LPITEMIDLIST pidlRelative,const TCHAR *szFileName)
{
	LPITEMIDLIST	pidlItem = NULL;
	HANDLE			hFirstFile;
	TCHAR			szPath[MAX_PATH];
	int				uItemId;

	m_nAwaitingAdd++;

	uItemId = GenerateUniqueItemId();

	m_extraItemInfoMap[uItemId].pridl					= ILClone(pidlRelative);
	m_extraItemInfoMap[uItemId].bIconRetrieved		= FALSE;
	m_extraItemInfoMap[uItemId].bThumbnailRetreived	= FALSE;
	m_extraItemInfoMap[uItemId].bFolderSizeRetrieved	= FALSE;
	StringCchCopy(m_extraItemInfoMap[uItemId].szDisplayName,
		SIZEOF_ARRAY(m_extraItemInfoMap[uItemId].szDisplayName), szFileName);

	pidlItem = ILCombine(pidlDirectory,pidlRelative);

	SHGetPathFromIDList(pidlItem,szPath);

	CoTaskMemFree(pidlItem);

	/* DO NOT call FindFirstFile() on root drives (especially
	floppy drives). Doing so may cause a delay of up to a
	few seconds. */
	if(!PathIsRoot(szPath))
	{
		m_extraItemInfoMap[uItemId].bDrive = FALSE;

		WIN32_FIND_DATA wfd;
		hFirstFile = FindFirstFile(szPath,&wfd);

		m_fileInfoMap.insert({uItemId, wfd});
	}
	else
	{
		m_extraItemInfoMap[uItemId].bDrive = TRUE;
		StringCchCopy(m_extraItemInfoMap[uItemId].szDrive,
			SIZEOF_ARRAY(m_extraItemInfoMap[uItemId].szDrive),
			szPath);

		hFirstFile = INVALID_HANDLE_VALUE;
	}

	/* Need to use this, since may be in a virtual folder
	(such as the recycle bin), but items still exist. */
	if(hFirstFile != INVALID_HANDLE_VALUE)
	{
		m_extraItemInfoMap[uItemId].bReal = TRUE;
		FindClose(hFirstFile);
	}
	else
	{
		WIN32_FIND_DATA wfd;

		StringCchCopy(wfd.cFileName, SIZEOF_ARRAY(wfd.cFileName), szFileName);
		wfd.nFileSizeLow			= 0;
		wfd.nFileSizeHigh			= 0;
		wfd.dwFileAttributes		= FILE_ATTRIBUTE_DIRECTORY;

		m_fileInfoMap.insert({uItemId, wfd});

		m_extraItemInfoMap[uItemId].bReal = FALSE;
	}

	return uItemId;
}