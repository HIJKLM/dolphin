// Copyright (C) 2003-2009 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include "Common.h"

#include "WII_IPC_HLE_Device_fs.h"

#include "StringUtil.h"
#include "FileSearch.h"
#include "FileUtil.h"

#include "../VolumeHandler.h"

extern std::string HLE_IPC_BuildFilename(const char* _pFilename, int _size);

#define FS_RESULT_OK			(0)
#define FS_DIRFILE_NOT_FOUND      	(-6)
#define FS_INVALID_ARGUMENT		(-101)
#define FS_FILE_EXIST			(-105)
#define FS_FILE_NOT_EXIST		(-106)
#define FS_RESULT_FATAL			(-128)

#define MAX_NAME				(12)


CWII_IPC_HLE_Device_fs::CWII_IPC_HLE_Device_fs(u32 _DeviceID, const std::string& _rDeviceName) 
	: IWII_IPC_HLE_Device(_DeviceID, _rDeviceName)
{}

CWII_IPC_HLE_Device_fs::~CWII_IPC_HLE_Device_fs()
{}

bool CWII_IPC_HLE_Device_fs::Open(u32 _CommandAddress, u32 _Mode)
{
	// clear tmp folder
	{
	    //		std::string WiiTempFolder = File::GetUserDirectory() + FULL_WII_USER_DIR + std::string("tmp");
	    std::string WiiTempFolder = FULL_WII_USER_DIR + std::string("tmp");
	    File::DeleteDirRecursively(WiiTempFolder.c_str());
	    File::CreateDir(WiiTempFolder.c_str());
	}

	// create home directory
    if (VolumeHandler::IsValid())
	{
		char Path[260+1];
		u32 TitleID, GameID;
		VolumeHandler::RAWReadToPtr((u8*)&TitleID, 0x0F8001DC, 4);
		
		TitleID = Common::swap32(TitleID);
		GameID = VolumeHandler::Read32(0);

        _dbg_assert_(WII_IPC_FILEIO, GameID != 0);
		if (GameID == 0) GameID = 0xF00DBEEF;
		if (TitleID == 0) TitleID = 0x00010000;

		sprintf(Path, FULL_WII_USER_DIR "title/%08x/%08x/data/nocopy/", TitleID, GameID);

		File::CreateFullPath(Path);
	}

	Memory::Write_U32(GetDeviceID(), _CommandAddress+4);
	return true;
}

bool CWII_IPC_HLE_Device_fs::IOCtlV(u32 _CommandAddress) 
{ 
	u32 ReturnValue = FS_RESULT_OK;
	SIOCtlVBuffer CommandBuffer(_CommandAddress);
	
	// Prepare the out buffer(s) with zeroes as a safety precaution
	// to avoid returning bad values
	for(u32 i = 0; i < CommandBuffer.NumberPayloadBuffer; i++)
	{
		Memory::Memset(CommandBuffer.PayloadBuffer[i].m_Address, 0,
			CommandBuffer.PayloadBuffer[i].m_Size);
	}

	switch(CommandBuffer.Parameter)
	{
	case IOCTLV_READ_DIR:
		{
			// the wii uses this function to define the type (dir or file)
			std::string Filename(HLE_IPC_BuildFilename((const char*)Memory::GetPointer(
				CommandBuffer.InBuffer[0].m_Address), CommandBuffer.InBuffer[0].m_Size));

			INFO_LOG(WII_IPC_FILEIO, "FS: IOCTL_READ_DIR %s", Filename.c_str());

			/* Check if this is really a directory. Or a file, because it seems like Mario Kart
			   did a IOCTL_READ_DIR on the save file to check if it existed before deleting it,
			   and if I didn't returned a -something it never deleted the file presumably because
			   it thought it didn't exist. So this solution worked for Mario Kart. 
			   
			   F|RES: i dont have mkart but -6 is a wrong return value if you try to read from a 
			   directory which doesnt exist
			   
			   JP: Okay, but Mario Kart calls this for files and if I return 0 here it never
			   creates a new file in any event, it just calls a DELETE_FILE and never close
			   the handle, so perhaps this is better
			   */

			if (!File::Exists(Filename.c_str()))
			{
				WARN_LOG(WII_IPC_FILEIO, "    directory does not exist - return FS_DIRFILE_NOT_FOUND", Filename.c_str());
				ReturnValue = FS_DIRFILE_NOT_FOUND;
				break;
			}
			/* Okay, maybe it is a file but not a directory, then we should return -101?
			   I have not seen any example of this. */
			else if (!File::IsDirectory(Filename.c_str()))
			{
				WARN_LOG(WII_IPC_FILEIO, "    Not a directory - return FS_INVALID_ARGUMENT", Filename.c_str());
				ReturnValue = FS_INVALID_ARGUMENT;
				break;
			}

			// make a file search
			CFileSearch::XStringVector Directories;
			Directories.push_back(Filename);

			CFileSearch::XStringVector Extensions;
			Extensions.push_back("*.*");

			CFileSearch FileSearch(Extensions, Directories);

			// it is one
			if ((CommandBuffer.InBuffer.size() == 1) && (CommandBuffer.PayloadBuffer.size() == 1))
			{
				size_t numFile = FileSearch.GetFileNames().size();
				INFO_LOG(WII_IPC_FILEIO, "    Files in directory: %i", numFile);

				Memory::Write_U32((u32)numFile, CommandBuffer.PayloadBuffer[0].m_Address);
			}
			else
			{
				u32 MaxEntries = Memory::Read_U32(CommandBuffer.InBuffer[0].m_Address);

				memset(Memory::GetPointer(CommandBuffer.PayloadBuffer[0].m_Address), 0, CommandBuffer.PayloadBuffer[0].m_Size);

				size_t numFiles = 0;
				char* pFilename = (char*)Memory::GetPointer((u32)(CommandBuffer.PayloadBuffer[0].m_Address));

				for (size_t i=0; i<FileSearch.GetFileNames().size(); i++)
				{
					if (i >= MaxEntries)
						break;

					std::string filename, ext;
					SplitPath(FileSearch.GetFileNames()[i], NULL, &filename, &ext);
					std::string CompleteFilename = filename + ext;

					strcpy(pFilename, CompleteFilename.c_str());
					pFilename += CompleteFilename.length();
					*pFilename++ = 0x00;  // termination
					numFiles++;

					INFO_LOG(WII_IPC_FILEIO, "    %s", CompleteFilename.c_str());					
				}

				Memory::Write_U32((u32)numFiles, CommandBuffer.PayloadBuffer[1].m_Address);
			}

			ReturnValue = FS_RESULT_OK;
		}
		break;

	case IOCTLV_GETUSAGE:
		{
			// check buffer sizes
			_dbg_assert_(WII_IPC_FILEIO, CommandBuffer.PayloadBuffer.size() == 2);
			_dbg_assert_(WII_IPC_FILEIO, CommandBuffer.PayloadBuffer[0].m_Size == 4);
			_dbg_assert_(WII_IPC_FILEIO, CommandBuffer.PayloadBuffer[1].m_Size == 4);

			// this command sucks because it asks of the number of used 
			// fsBlocks and inodes
			// we answer nothing is used, but if a program uses it to check
			// how much memory has been used we are doomed...
			std::string Filename(HLE_IPC_BuildFilename((const char*)Memory::GetPointer(CommandBuffer.InBuffer[0].m_Address), CommandBuffer.InBuffer[0].m_Size));
			u32 fsBlock = 0;
			u32 iNodes = 0;

			WARN_LOG(WII_IPC_FILEIO, "FS: IOCTL_GETUSAGE %s", Filename.c_str());
			if (File::IsDirectory(Filename.c_str()))
			{
				// make a file search
				CFileSearch::XStringVector Directories;
				Directories.push_back(Filename);

				CFileSearch::XStringVector Extensions;
				Extensions.push_back("*.*");

				CFileSearch FileSearch(Extensions, Directories);
			
				u64 overAllSize = 0;
				for (size_t i=0; i<FileSearch.GetFileNames().size(); i++)
				{
					overAllSize += File::GetSize(FileSearch.GetFileNames()[i].c_str());
				}

				fsBlock = (u32)(overAllSize / (16 * 1024));  // one bock is 16kb
				iNodes = (u32)(FileSearch.GetFileNames().size());

				ReturnValue = FS_RESULT_OK;

				WARN_LOG(WII_IPC_FILEIO, "    fsBlock: %i, iNodes: %i", fsBlock, iNodes);
			}
			else
			{
				fsBlock = 0;
				iNodes = 0;
				ReturnValue = FS_RESULT_OK;

				// PanicAlert("IOCTL_GETUSAGE - unk dir %s", Filename.c_str());
				WARN_LOG(WII_IPC_FILEIO, "    error: not executed on a valid directoy: %s", Filename.c_str());
			}
			
			Memory::Write_U32(fsBlock, CommandBuffer.PayloadBuffer[0].m_Address);
			Memory::Write_U32(iNodes, CommandBuffer.PayloadBuffer[1].m_Address);
		}
		break;


	default:
		PanicAlert("CWII_IPC_HLE_Device_fs::IOCtlV: %i", CommandBuffer.Parameter);
		break;
	}

	Memory::Write_U32(ReturnValue, _CommandAddress+4);
	
	return true; 
}

bool CWII_IPC_HLE_Device_fs::IOCtl(u32 _CommandAddress) 
{ 
	//u32 DeviceID = Memory::Read_U32(_CommandAddress + 8);
	//LOG(WII_IPC_FILEIO, "FS: IOCtl (Device=%s, DeviceID=%08x)", GetDeviceName().c_str(), DeviceID);

	u32 Parameter =  Memory::Read_U32(_CommandAddress + 0xC);
	u32 BufferIn =  Memory::Read_U32(_CommandAddress + 0x10);
	u32 BufferInSize =  Memory::Read_U32(_CommandAddress + 0x14);
	u32 BufferOut = Memory::Read_U32(_CommandAddress + 0x18);
	u32 BufferOutSize = Memory::Read_U32(_CommandAddress + 0x1C);

	/* Prepare the out buffer(s) with zeroes as a safety precaution
	   to avoid returning bad values. */
	//LOG(WII_IPC_FILEIO, "Cleared %u bytes of the out buffer", _BufferOutSize);
	Memory::Memset(BufferOut, 0, BufferOutSize);

	u32 ReturnValue = ExecuteCommand(Parameter, BufferIn, BufferInSize, BufferOut, BufferOutSize);	
	Memory::Write_U32(ReturnValue, _CommandAddress + 4);

	return true; 
}

s32 CWII_IPC_HLE_Device_fs::ExecuteCommand(u32 _Parameter, u32 _BufferIn, u32 _BufferInSize, u32 _BufferOut, u32 _BufferOutSize)
{
	switch(_Parameter)
	{
	case IOCTL_GET_STATS:
		{
			_dbg_assert_(WII_IPC_FILEIO, _BufferOutSize == 28);

			WARN_LOG(WII_IPC_FILEIO, "FS: GET STATS - no idea what we have to return here, prolly the free memory etc:)");
			WARN_LOG(WII_IPC_FILEIO, "    InBufferSize: %i OutBufferSize: %i", _BufferInSize, _BufferOutSize);

			// This happens in Tatsonuko vs Capcom., Transformers
            // The buffer out values are ripped form a real WII and i dont know the meaning
            // of them. Prolly it is some kind of small statistic like number of iblocks, free iblocks etc
            u32 Addr = _BufferOut;
            Memory::Write_U32(0x00004000, Addr); Addr += 4;
            Memory::Write_U32(0x00005717, Addr); Addr += 4;
            Memory::Write_U32(0x000024a9, Addr); Addr += 4;
            Memory::Write_U32(0x00000000, Addr); Addr += 4;
            Memory::Write_U32(0x00000300, Addr); Addr += 4;
            Memory::Write_U32(0x0000163e, Addr); Addr += 4;
            Memory::Write_U32(0x000001c1, Addr);

			return FS_RESULT_OK;
		}
		break;

	case IOCTL_CREATE_DIR:
		{
			_dbg_assert_(WII_IPC_FILEIO, _BufferOutSize == 0);
			u32 Addr = _BufferIn;

			u32 OwnerID = Memory::Read_U32(Addr); Addr += 4;
			u16 GroupID = Memory::Read_U16(Addr); Addr += 2;
			std::string DirName(HLE_IPC_BuildFilename((const char*)Memory::GetPointer(Addr), 64)); Addr += 64;
			Addr += 9; // owner attribs, permission
			u8 Attribs = Memory::Read_U8(Addr);

			INFO_LOG(WII_IPC_FILEIO, "FS: CREATE_DIR %s", DirName.c_str());

			DirName += DIR_SEP;
			File::CreateFullPath(DirName.c_str());
			_dbg_assert_msg_(WII_IPC_FILEIO, File::IsDirectory(DirName.c_str()), "FS: CREATE_DIR %s failed", DirName.c_str());

			return FS_RESULT_OK;
		}
		break;

	case IOCTL_SET_ATTR:
		{
			u32 Addr = _BufferIn;
		
			u32 OwnerID = Memory::Read_U32(Addr); Addr += 4;
			u16 GroupID = Memory::Read_U16(Addr); Addr += 2;
			std::string Filename = HLE_IPC_BuildFilename((const char*)Memory::GetPointer(_BufferIn), 64); Addr += 64;
			u8 OwnerPerm = Memory::Read_U8(Addr); Addr += 1;
			u8 GroupPerm = Memory::Read_U8(Addr); Addr += 1;
			u8 OtherPerm = Memory::Read_U8(Addr); Addr += 1;
			u8 Attributes = Memory::Read_U8(Addr); Addr += 1;

			INFO_LOG(WII_IPC_FILEIO, "FS: SetAttrib %s", Filename.c_str());
			DEBUG_LOG(WII_IPC_FILEIO, "    OwnerID: 0x%08x", OwnerID);
			DEBUG_LOG(WII_IPC_FILEIO, "    GroupID: 0x%04x", GroupID);
			DEBUG_LOG(WII_IPC_FILEIO, "    OwnerPerm: 0x%02x", OwnerPerm);
			DEBUG_LOG(WII_IPC_FILEIO, "    GroupPerm: 0x%02x", GroupPerm);
			DEBUG_LOG(WII_IPC_FILEIO, "    OtherPerm: 0x%02x", OtherPerm);
			DEBUG_LOG(WII_IPC_FILEIO, "    Attributes: 0x%02x", Attributes);

			return FS_RESULT_OK;
		}
		break;

	case IOCTL_GET_ATTR:
		{		
			_dbg_assert_msg_(WII_IPC_FILEIO, _BufferOutSize == 76,
				"    GET_ATTR needs an 76 bytes large output buffer but it is %i bytes large",
				_BufferOutSize);

			u32 OwnerID = 0;
			u16 GroupID = 0;
			std::string Filename = HLE_IPC_BuildFilename((const char*)Memory::GetPointer(_BufferIn), 64);
			u8 OwnerPerm = 0x3;		// read/write
			u8 GroupPerm = 0x3;		// read/write
			u8 OtherPerm = 0x3;		// read/write		
			u8 Attributes = 0x00;	// no attributes
			if (File::IsDirectory(Filename.c_str()))
			{
				INFO_LOG(WII_IPC_FILEIO, "FS: GET_ATTR Directory %s - all permission flags are set", Filename.c_str());
			}
			else
			{
				if (File::Exists(Filename.c_str()))
				{
					INFO_LOG(WII_IPC_FILEIO, "FS: GET_ATTR %s - all permission flags are set", Filename.c_str());
				}
				else
				{
					INFO_LOG(WII_IPC_FILEIO, "FS: GET_ATTR unknown %s", Filename.c_str());
					return FS_FILE_NOT_EXIST;
				}
			}

			// write answer to buffer
			if (_BufferOutSize == 76)
			{
				u32 Addr = _BufferOut;
				Memory::Write_U32(OwnerID, Addr);										Addr += 4;
				Memory::Write_U16(GroupID, Addr);										Addr += 2;
				memcpy(Memory::GetPointer(Addr), Filename.c_str(), Filename.size());	Addr += 64;
				Memory::Write_U8(OwnerPerm, Addr);										Addr += 1;
				Memory::Write_U8(GroupPerm, Addr);										Addr += 1;
				Memory::Write_U8(OtherPerm, Addr);										Addr += 1;
				Memory::Write_U8(Attributes, Addr);										Addr += 1;
			}

			return FS_RESULT_OK;
		}
		break;


	case IOCTL_DELETE_FILE:
		{
			_dbg_assert_(WII_IPC_FILEIO, _BufferOutSize == 0);
			int Offset = 0;

			std::string Filename = HLE_IPC_BuildFilename((const char*)Memory::GetPointer(_BufferIn+Offset), 64);
			Offset += 64;
			if (File::Delete(Filename.c_str()))
			{
				INFO_LOG(WII_IPC_FILEIO, "FS: DeleteFile %s", Filename.c_str());
			}
			else if (File::DeleteDir(Filename.c_str()))
			{
				INFO_LOG(WII_IPC_FILEIO, "FS: DeleteDir %s", Filename.c_str());
			}
			else
			{
				WARN_LOG(WII_IPC_FILEIO, "FS: DeleteFile %s - failed!!!", Filename.c_str());
			}

			return FS_RESULT_OK;
		}
		break;

	case IOCTL_RENAME_FILE:
		{
			_dbg_assert_(WII_IPC_FILEIO, _BufferOutSize == 0);
			int Offset = 0;

			std::string Filename = HLE_IPC_BuildFilename((const char*)Memory::GetPointer(_BufferIn+Offset), 64);
			Offset += 64;

			std::string FilenameRename = HLE_IPC_BuildFilename((const char*)Memory::GetPointer(_BufferIn+Offset), 64);
			Offset += 64;

			// try to make the basis directory
			File::CreateFullPath(FilenameRename.c_str());

			// if there is already a filedelete it
			if (File::Exists(FilenameRename.c_str()))
			{
				File::Delete(FilenameRename.c_str());
			}

			// finally try to rename the file
			if (File::Rename(Filename.c_str(), FilenameRename.c_str()))
			{
				INFO_LOG(WII_IPC_FILEIO, "FS: Rename %s to %s", Filename.c_str(), FilenameRename.c_str());
			}
			else
			{
				ERROR_LOG(WII_IPC_FILEIO, "FS: Rename %s to %s - failed", Filename.c_str(), FilenameRename.c_str());
				return FS_FILE_NOT_EXIST;
			}

			return FS_RESULT_OK;
		}
		break;

	case IOCTL_CREATE_FILE:
		{
			_dbg_assert_(WII_IPC_FILEIO, _BufferOutSize == 0);

			u32 Addr = _BufferIn;
			u32 OwnerID = Memory::Read_U32(Addr); Addr += 4;
			u16 GroupID = Memory::Read_U16(Addr); Addr += 2;
			std::string Filename(HLE_IPC_BuildFilename((const char*)Memory::GetPointer(Addr), 64)); Addr += 64;
			u8 OwnerPerm = Memory::Read_U8(Addr); Addr++;
			u8 GroupPerm = Memory::Read_U8(Addr); Addr++;
			u8 OtherPerm = Memory::Read_U8(Addr); Addr++;
			u8 Attributes = Memory::Read_U8(Addr); Addr++;

			INFO_LOG(WII_IPC_FILEIO, "FS: CreateFile %s", Filename.c_str());
			DEBUG_LOG(WII_IPC_FILEIO, "    OwnerID: 0x%08x", OwnerID);
			DEBUG_LOG(WII_IPC_FILEIO, "    GroupID: 0x%04x", GroupID);
			DEBUG_LOG(WII_IPC_FILEIO, "    OwnerPerm: 0x%02x", OwnerPerm);
			DEBUG_LOG(WII_IPC_FILEIO, "    GroupPerm: 0x%02x", GroupPerm);
			DEBUG_LOG(WII_IPC_FILEIO, "    OtherPerm: 0x%02x", OtherPerm);
			DEBUG_LOG(WII_IPC_FILEIO, "    Attributes: 0x%02x", Attributes);

			// check if the file already exist
			if (File::Exists(Filename.c_str()))
			{
				WARN_LOG(WII_IPC_FILEIO, "    result = FS_RESULT_EXISTS", Filename.c_str());
				return FS_FILE_EXIST;
			}

			// create the file
			File::CreateFullPath(Filename.c_str());  // just to be sure
			bool Result = File::CreateEmptyFile(Filename.c_str());
			if (!Result)
			{
				ERROR_LOG(WII_IPC_FILEIO, "CWII_IPC_HLE_Device_fs: couldn't create new file");
				PanicAlert("CWII_IPC_HLE_Device_fs: couldn't create new file");
				return FS_RESULT_FATAL;
			}

			INFO_LOG(WII_IPC_FILEIO, "    result = FS_RESULT_OK", Filename.c_str());
			return FS_RESULT_OK;
		}
		break;

	default:
		ERROR_LOG(WII_IPC_FILEIO, "CWII_IPC_HLE_Device_fs::IOCtl: ni  0x%x", _Parameter);
		PanicAlert("CWII_IPC_HLE_Device_fs::IOCtl: ni  0x%x", _Parameter);
		break;
	}

	return FS_RESULT_FATAL;
}
