// Client7z.cpp

#include "StdAfx.h"
#include "SevenZipWorker.h"

#include <stdlib.h>  
#include <direct.h> 
#include <windows.h> 
#include <string.h>
#include <string>
#include <io.h>
#include <time.h>
#include <stdio.h >  
#include <iostream>

#include "Common/MyWindows.h"

#include "Common/Defs.h"
#include "Common/MyInitGuid.h"

#include "Common/IntToString.h"
#include "Common/StringConvert.h"

#include "Windows/DLL.h"
#include "Windows/FileDir.h"
#include "Windows/FileFind.h"
#include "Windows/FileName.h"
#include "Windows/NtCheck.h"
#include "Windows/PropVariant.h"
#include "Windows/PropVariantConv.h"

#include "Common/FileStreams.h"

#include "Archive/IArchive.h"

#include "IPassword.h"
#include "C/7zVersion.h"

#define DEBUG_7Z // 调试信息宏
#define LIB_MAIN // main 函数入口宏

#ifdef _WIN32
HINSTANCE g_hInstance = 0;
#endif

// Tou can find the list of all GUIDs in Guid.txt file.
// use another CLSIDs, if you want to support other formats (zip, rar, ...).
// {23170F69-40C1-278A-1000-000110070000}

DEFINE_GUID(CLSID_CFormat7z,
  0x23170F69, 0x40C1, 0x278A, 0x10, 0x00, 0x00, 0x01, 0x10, 0x07, 0x00, 0x00);
DEFINE_GUID(CLSID_CFormatXz,
  0x23170F69, 0x40C1, 0x278A, 0x10, 0x00, 0x00, 0x01, 0x10, 0x0C, 0x00, 0x00);

#define CLSID_Format CLSID_CFormat7z
// #define CLSID_Format CLSID_CFormatXz

using namespace NWindows;
using namespace NFile;
using namespace NDir;

#define kDllName "7z.dll"

static const char *kCopyrightString = "\n7-Zip " MY_VERSION
" ("  kDllName " client) "
MY_COPYRIGHT " " MY_DATE "\n";

static const char *kHelpString =
"Examples:\n"
"  7zlib.exe l d:\\work\\pro\\folder.7z                         : show all files in folder.7z\n"
"  7zlib.exe a d:\\work\\pro\\folder     d:\\work\\pro\\folder.7z  : compress folder into folder.7z\n"
"  7zlib.exe x d:\\work\\pro\\folder.7z  e:\\path                : extract files from folder.7z to path directory\n";

void cleanLine(){
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	// 获取标准输出设备句柄
	CONSOLE_SCREEN_BUFFER_INFO bInfo; // 窗口缓冲区信息
	GetConsoleScreenBufferInfo(hOut, &bInfo);
	COORD size = bInfo.dwSize;
	int width = (int)size.X - 1;
	putchar('\r');
	for (int i = 0; i < width; i++){
		putchar(' ');
	}
	putchar('\r');
}

static AString FStringToConsoleString(const FString &s)
{
  return GetOemString(fs2us(s));
}

static FString CmdStringToFString(const char *s)
{
  return us2fs(GetUnicodeString(s));
}

static void PrintString(const UString &s)
{
  printf("%s", (LPCSTR)GetOemString(s));
}

static void PrintString(const AString &s)
{
  printf("%s", (LPCSTR)s);
}

static void PrintNewLine()
{
  PrintString("\n");
}

static void PrintStringLn(const AString &s)
{
  PrintString(s);
  PrintNewLine();
}

static void PrintError(const char *message, const FString &name)
{
  printf("Error: %s", (LPCSTR)message);
  PrintNewLine();
  PrintString(FStringToConsoleString(name));
  PrintNewLine();
}

static void PrintError(const AString &s)
{
  PrintNewLine();
  PrintString(s);
  PrintNewLine();
}

static HRESULT IsArchiveItemProp(IInArchive *archive, UInt32 index, PROPID propID, bool &result)
{
  NCOM::CPropVariant prop;
  RINOK(archive->GetProperty(index, propID, &prop));
  if (prop.vt == VT_BOOL)
    result = VARIANT_BOOLToBool(prop.boolVal);
  else if (prop.vt == VT_EMPTY)
    result = false;
  else
    return E_FAIL;
  return S_OK;
}

static HRESULT IsArchiveItemFolder(IInArchive *archive, UInt32 index, bool &result)
{
  return IsArchiveItemProp(archive, index, kpidIsDir, result);
}


static const wchar_t *kEmptyFileAlias = L"[Content]";


//////////////////////////////////////////////////////////////
// Archive Open callback class


class CArchiveOpenCallback:
  public IArchiveOpenCallback,
  public ICryptoGetTextPassword,
  public CMyUnknownImp
{
public:
  MY_UNKNOWN_IMP1(ICryptoGetTextPassword)

  STDMETHOD(SetTotal)(const UInt64 *files, const UInt64 *bytes);
  STDMETHOD(SetCompleted)(const UInt64 *files, const UInt64 *bytes);

  STDMETHOD(CryptoGetTextPassword)(BSTR *password);

  bool PasswordIsDefined;
  UString Password;

  CArchiveOpenCallback() : PasswordIsDefined(false) {}
};

STDMETHODIMP CArchiveOpenCallback::SetTotal(const UInt64 * /* files */, const UInt64 * /* bytes */)
{
  return S_OK;
}

STDMETHODIMP CArchiveOpenCallback::SetCompleted(const UInt64 * /* files */, const UInt64 * /* bytes */)
{
  return S_OK;
}
  
STDMETHODIMP CArchiveOpenCallback::CryptoGetTextPassword(BSTR *password)
{
  if (!PasswordIsDefined)
  {
    // You can ask real password here from user
    // Password = GetPassword(OutStream);
    // PasswordIsDefined = true;
    PrintError("Password is not defined");
    return E_ABORT;
  }
  return StringToBstr(Password, password);
}


//////////////////////////////////////////////////////////////
// Archive Extracting callback class

static const char *kTestingString    =  "Testing     ";
static const char *kExtractingString =  "Extracting  ";
static const char *kSkippingString   =  "Skipping    ";

static const char *kUnsupportedMethod = "Unsupported Method";
static const char *kCRCFailed = "CRC Failed";
static const char *kDataError = "Data Error";
static const char *kUnavailableData = "Unavailable data";
static const char *kUnexpectedEnd = "Unexpected end of data";
static const char *kDataAfterEnd = "There are some data after the end of the payload data";
static const char *kIsNotArc = "Is not archive";
static const char *kHeadersError = "Headers Error";

class CArchiveExtractCallback:
  public IArchiveExtractCallback,
  public ICryptoGetTextPassword,
  public CMyUnknownImp
{
public:
  MY_UNKNOWN_IMP1(ICryptoGetTextPassword)

  // IProgress
  STDMETHOD(SetTotal)(UInt64 size);
  STDMETHOD(SetCompleted)(const UInt64 *completeValue);

  // IArchiveExtractCallback
  STDMETHOD(GetStream)(UInt32 index, ISequentialOutStream **outStream, Int32 askExtractMode);
  STDMETHOD(PrepareOperation)(Int32 askExtractMode);
  STDMETHOD(SetOperationResult)(Int32 resultEOperationResult);

  // ICryptoGetTextPassword
  STDMETHOD(CryptoGetTextPassword)(BSTR *aPassword);

private:
  CMyComPtr<IInArchive> _archiveHandler;
  FString _directoryPath;  // Output directory
  UString _filePath;       // name inside arcvhive
  FString _diskFilePath;   // full path to file on disk
  bool _extractMode;
  struct CProcessedFileInfo
  {
    FILETIME MTime;
    UInt32 Attrib;
    bool isDir;
    bool AttribDefined;
    bool MTimeDefined;
  } _processedFileInfo;

  COutFileStream *_outFileStreamSpec;
  CMyComPtr<ISequentialOutStream> _outFileStream;

public:
  void Init(IInArchive *archiveHandler, const FString &directoryPath);

  UInt64 NumErrors;
  bool PasswordIsDefined;
  UString Password;

  CArchiveExtractCallback() : PasswordIsDefined(false) {}
};

void CArchiveExtractCallback::Init(IInArchive *archiveHandler, const FString &directoryPath)
{
  NumErrors = 0;
  _archiveHandler = archiveHandler;
  _directoryPath = directoryPath;
  NName::NormalizeDirPathPrefix(_directoryPath);
}

STDMETHODIMP CArchiveExtractCallback::SetTotal(UInt64 /* size */)
{
  return S_OK;
}

STDMETHODIMP CArchiveExtractCallback::SetCompleted(const UInt64 * /* completeValue */)
{
  return S_OK;
}

STDMETHODIMP CArchiveExtractCallback::GetStream(UInt32 index,
    ISequentialOutStream **outStream, Int32 askExtractMode)
{
  *outStream = 0;
  _outFileStream.Release();

  {
    // Get Name
    NCOM::CPropVariant prop;
    RINOK(_archiveHandler->GetProperty(index, kpidPath, &prop));
    
    UString fullPath;
    if (prop.vt == VT_EMPTY)
      fullPath = kEmptyFileAlias;
    else
    {
      if (prop.vt != VT_BSTR)
        return E_FAIL;
      fullPath = prop.bstrVal;
    }
    _filePath = fullPath;
  }

  if (askExtractMode != NArchive::NExtract::NAskMode::kExtract)
    return S_OK;

  {
    // Get Attrib
    NCOM::CPropVariant prop;
    RINOK(_archiveHandler->GetProperty(index, kpidAttrib, &prop));
    if (prop.vt == VT_EMPTY)
    {
      _processedFileInfo.Attrib = 0;
      _processedFileInfo.AttribDefined = false;
    }
    else
    {
      if (prop.vt != VT_UI4)
        return E_FAIL;
      _processedFileInfo.Attrib = prop.ulVal;
      _processedFileInfo.AttribDefined = true;
    }
  }

  RINOK(IsArchiveItemFolder(_archiveHandler, index, _processedFileInfo.isDir));

  {
    // Get Modified Time
    NCOM::CPropVariant prop;
    RINOK(_archiveHandler->GetProperty(index, kpidMTime, &prop));
    _processedFileInfo.MTimeDefined = false;
    switch (prop.vt)
    {
      case VT_EMPTY:
        // _processedFileInfo.MTime = _utcMTimeDefault;
        break;
      case VT_FILETIME:
        _processedFileInfo.MTime = prop.filetime;
        _processedFileInfo.MTimeDefined = true;
        break;
      default:
        return E_FAIL;
    }

  }
  {
    // Get Size
    NCOM::CPropVariant prop;
    RINOK(_archiveHandler->GetProperty(index, kpidSize, &prop));
    UInt64 newFileSize;
    /* bool newFileSizeDefined = */ ConvertPropVariantToUInt64(prop, newFileSize);
  }

  
  {
    // Create folders for file
    int slashPos = _filePath.ReverseFind_PathSepar();
    if (slashPos >= 0)
      CreateComplexDir(_directoryPath + us2fs(_filePath.Left(slashPos)));
  }

  FString fullProcessedPath = _directoryPath + us2fs(_filePath);
  _diskFilePath = fullProcessedPath;

  if (_processedFileInfo.isDir)
  {
    CreateComplexDir(fullProcessedPath);
  }
  else
  {
    NFind::CFileInfo fi;
    if (fi.Find(fullProcessedPath))
    {
      if (!DeleteFileAlways(fullProcessedPath))
      {
        PrintError("Can not delete output file", fullProcessedPath);
        return E_ABORT;
      }
    }
    
    _outFileStreamSpec = new COutFileStream;
    CMyComPtr<ISequentialOutStream> outStreamLoc(_outFileStreamSpec);
    if (!_outFileStreamSpec->Open(fullProcessedPath, CREATE_ALWAYS))
    {
      PrintError("Can not open output file", fullProcessedPath);
      return E_ABORT;
    }
    _outFileStream = outStreamLoc;
    *outStream = outStreamLoc.Detach();
  }
  return S_OK;
}

STDMETHODIMP CArchiveExtractCallback::PrepareOperation(Int32 askExtractMode)
{
  _extractMode = false;
  switch (askExtractMode)
  {
    case NArchive::NExtract::NAskMode::kExtract:  _extractMode = true; break;
  };
  switch (askExtractMode)
  {
#ifdef DEBUG_7Z
	case NArchive::NExtract::NAskMode::kExtract:
		cleanLine();
		printf("%3.0lf%% ", SevenZipWorker::getUnZipCompletedPersent() * 100);
		SevenZipWorker::completedFiles += 1;
		PrintString(kExtractingString);
		break;
#endif
    case NArchive::NExtract::NAskMode::kTest:  PrintString(kTestingString); break;
    case NArchive::NExtract::NAskMode::kSkip:  PrintString(kSkippingString); break;
  };

#ifdef DEBUG_7Z
  PrintString(_filePath);
#endif

  return S_OK;
}

STDMETHODIMP CArchiveExtractCallback::SetOperationResult(Int32 operationResult)
{
  switch (operationResult)
  {
    case NArchive::NExtract::NOperationResult::kOK:
      break;
    default:
    {
      NumErrors++;
      PrintString("  :  ");
      const char *s = NULL;
      switch (operationResult)
      {
        case NArchive::NExtract::NOperationResult::kUnsupportedMethod:
          s = kUnsupportedMethod;
          break;
        case NArchive::NExtract::NOperationResult::kCRCError:
          s = kCRCFailed;
          break;
        case NArchive::NExtract::NOperationResult::kDataError:
          s = kDataError;
          break;
        case NArchive::NExtract::NOperationResult::kUnavailable:
          s = kUnavailableData;
          break;
        case NArchive::NExtract::NOperationResult::kUnexpectedEnd:
          s = kUnexpectedEnd;
          break;
        case NArchive::NExtract::NOperationResult::kDataAfterEnd:
          s = kDataAfterEnd;
          break;
        case NArchive::NExtract::NOperationResult::kIsNotArc:
          s = kIsNotArc;
          break;
        case NArchive::NExtract::NOperationResult::kHeadersError:
          s = kHeadersError;
          break;
      }
      if (s)
      {
        PrintString("Error : ");
        PrintString(s);
      }
      else
      {
        char temp[16];
        ConvertUInt32ToString(operationResult, temp);
        PrintString("Error #");
        PrintString(temp);
      }
    }
  }

  if (_outFileStream)
  {
    if (_processedFileInfo.MTimeDefined)
      _outFileStreamSpec->SetMTime(&_processedFileInfo.MTime);
    RINOK(_outFileStreamSpec->Close());
  }
  _outFileStream.Release();
  if (_extractMode && _processedFileInfo.AttribDefined)
    SetFileAttrib(_diskFilePath, _processedFileInfo.Attrib);
#ifdef DEBUG_7Z
  //PrintNewLine();
#endif
  return S_OK;
}


STDMETHODIMP CArchiveExtractCallback::CryptoGetTextPassword(BSTR *password)
{
  if (!PasswordIsDefined)
  {
    // You can ask real password here from user
    // Password = GetPassword(OutStream);
    // PasswordIsDefined = true;
    PrintError("Password is not defined");
    return E_ABORT;
  }
  return StringToBstr(Password, password);
}



//////////////////////////////////////////////////////////////
// Archive Creating callback class

struct CDirItem
{
  UInt64 Size;
  FILETIME CTime;
  FILETIME ATime;
  FILETIME MTime;
  UString Name;
  FString FullPath;
  UInt32 Attrib;

  bool isDir() const { return (Attrib & FILE_ATTRIBUTE_DIRECTORY) != 0 ; }
};

class CArchiveUpdateCallback:
  public IArchiveUpdateCallback2,
  public ICryptoGetTextPassword2,
  public CMyUnknownImp
{
public:
  MY_UNKNOWN_IMP2(IArchiveUpdateCallback2, ICryptoGetTextPassword2)

  // IProgress
  STDMETHOD(SetTotal)(UInt64 size);
  STDMETHOD(SetCompleted)(const UInt64 *completeValue);

  // IUpdateCallback2
  STDMETHOD(GetUpdateItemInfo)(UInt32 index,
      Int32 *newData, Int32 *newProperties, UInt32 *indexInArchive);
  STDMETHOD(GetProperty)(UInt32 index, PROPID propID, PROPVARIANT *value);
  STDMETHOD(GetStream)(UInt32 index, ISequentialInStream **inStream);
  STDMETHOD(SetOperationResult)(Int32 operationResult);
  STDMETHOD(GetVolumeSize)(UInt32 index, UInt64 *size);
  STDMETHOD(GetVolumeStream)(UInt32 index, ISequentialOutStream **volumeStream);

  STDMETHOD(CryptoGetTextPassword2)(Int32 *passwordIsDefined, BSTR *password);

public:
  CRecordVector<UInt64> VolumesSizes;
  UString VolName;
  UString VolExt;

  FString DirPrefix;
  const CObjectVector<CDirItem> *DirItems;

  bool PasswordIsDefined;
  UString Password;
  bool AskPassword;

  bool m_NeedBeClosed;

  FStringVector FailedFiles;
  CRecordVector<HRESULT> FailedCodes;

  CArchiveUpdateCallback(): PasswordIsDefined(false), AskPassword(false), DirItems(0) {};

  ~CArchiveUpdateCallback() { Finilize(); }
  HRESULT Finilize();

  void Init(const CObjectVector<CDirItem> *dirItems)
  {
    DirItems = dirItems;
    m_NeedBeClosed = false;
    FailedFiles.Clear();
    FailedCodes.Clear();
  }
};

STDMETHODIMP CArchiveUpdateCallback::SetTotal(UInt64 /* size */)
{
  return S_OK;
}

STDMETHODIMP CArchiveUpdateCallback::SetCompleted(const UInt64 * /* completeValue */)
{
  return S_OK;
}

STDMETHODIMP CArchiveUpdateCallback::GetUpdateItemInfo(UInt32 /* index */,
      Int32 *newData, Int32 *newProperties, UInt32 *indexInArchive)
{
  if (newData)
    *newData = BoolToInt(true);
  if (newProperties)
    *newProperties = BoolToInt(true);
  if (indexInArchive)
    *indexInArchive = (UInt32)(Int32)-1;
  return S_OK;
}

STDMETHODIMP CArchiveUpdateCallback::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value)
{
  NCOM::CPropVariant prop;
  
  if (propID == kpidIsAnti)
  {
    prop = false;
    prop.Detach(value);
    return S_OK;
  }

  {
    const CDirItem &dirItem = (*DirItems)[index];
    switch (propID)
    {
      case kpidPath:  prop = dirItem.Name; break;
      case kpidIsDir:  prop = dirItem.isDir(); break;
      case kpidSize:  prop = dirItem.Size; break;
      case kpidAttrib:  prop = dirItem.Attrib; break;
      case kpidCTime:  prop = dirItem.CTime; break;
      case kpidATime:  prop = dirItem.ATime; break;
      case kpidMTime:  prop = dirItem.MTime; break;
    }
  }
  prop.Detach(value);
  return S_OK;
}

HRESULT CArchiveUpdateCallback::Finilize()
{
  if (m_NeedBeClosed)
  {
#ifdef DEBUG_7Z
    //PrintNewLine();
#endif
    m_NeedBeClosed = false;
  }
  return S_OK;
}

#ifdef DEBUG_7Z
static void GetStream2(const wchar_t *name)
{
	cleanLine();
	printf("%3.0lf%% ", SevenZipWorker::getZipCompletedPersent() * 100);
	SevenZipWorker::zipFileNumber += 1;
  PrintString("Compressing  ");
  if (name[0] == 0)
    name = kEmptyFileAlias;
  PrintString(name);
}
#endif

STDMETHODIMP CArchiveUpdateCallback::GetStream(UInt32 index, ISequentialInStream **inStream)
{
  RINOK(Finilize());

  const CDirItem &dirItem = (*DirItems)[index];
#ifdef DEBUG_7Z
  GetStream2(dirItem.Name);
#endif
 
  if (dirItem.isDir())
    return S_OK;

  {
    CInFileStream *inStreamSpec = new CInFileStream;
    CMyComPtr<ISequentialInStream> inStreamLoc(inStreamSpec);
    FString path = DirPrefix + dirItem.FullPath;
    if (!inStreamSpec->Open(path))
    {
      DWORD sysError = ::GetLastError();
      FailedCodes.Add(sysError);
      FailedFiles.Add(path);
      // if (systemError == ERROR_SHARING_VIOLATION)
      {
        PrintNewLine();
        PrintError("WARNING: can't open file");
        // PrintString(NError::MyFormatMessageW(systemError));
        return S_FALSE;
      }
      // return sysError;
    }
    *inStream = inStreamLoc.Detach();
  }
  return S_OK;
}

STDMETHODIMP CArchiveUpdateCallback::SetOperationResult(Int32 /* operationResult */)
{
  m_NeedBeClosed = true;
  return S_OK;
}

STDMETHODIMP CArchiveUpdateCallback::GetVolumeSize(UInt32 index, UInt64 *size)
{
  if (VolumesSizes.Size() == 0)
    return S_FALSE;
  if (index >= (UInt32)VolumesSizes.Size())
    index = VolumesSizes.Size() - 1;
  *size = VolumesSizes[index];
  return S_OK;
}

STDMETHODIMP CArchiveUpdateCallback::GetVolumeStream(UInt32 index, ISequentialOutStream **volumeStream)
{
  wchar_t temp[16];
  ConvertUInt32ToString(index + 1, temp);
  UString res = temp;
  while (res.Len() < 2)
    res.InsertAtFront(L'0');
  UString fileName = VolName;
  fileName += L'.';
  fileName += res;
  fileName += VolExt;
  COutFileStream *streamSpec = new COutFileStream;
  CMyComPtr<ISequentialOutStream> streamLoc(streamSpec);
  if (!streamSpec->Create(us2fs(fileName), false))
    return ::GetLastError();
  *volumeStream = streamLoc.Detach();
  return S_OK;
}

STDMETHODIMP CArchiveUpdateCallback::CryptoGetTextPassword2(Int32 *passwordIsDefined, BSTR *password)
{
  if (!PasswordIsDefined)
  {
    if (AskPassword)
    {
      // You can ask real password here from user
      // Password = GetPassword(OutStream);
      // PasswordIsDefined = true;
      PrintError("Password is not defined");
      return E_ABORT;
    }
  }
  *passwordIsDefined = BoolToInt(PasswordIsDefined);
  return StringToBstr(Password, password);
}


// Main function

#define NT_CHECK_FAIL_ACTION PrintError("Unsupported Windows version"); return 1;

std::vector<std::string> SevenZipWorker::m_allFilename;
char SevenZipWorker::m_szInitDir[];
char SevenZipWorker::m_folderDir[];
unsigned int SevenZipWorker::zipFileNumber = 0; // 文件总数
unsigned int SevenZipWorker::fileNumber = 0; // 文件总数
unsigned int SevenZipWorker::folderNumber = 0; // 子文件夹总数
unsigned int SevenZipWorker::unZipFiles = 0;
unsigned int SevenZipWorker::completedFiles = 0;

double SevenZipWorker::getZipCompletedPersent(){
	if (fileNumber){
		return zipFileNumber * 1.0 / fileNumber;
	}
	else{
		return 0.0;
	}
}

double SevenZipWorker::getUnZipCompletedPersent(){
	if (unZipFiles){
		return completedFiles * 1.0 / unZipFiles;
	}
	else{
		return 0.0;
	}
}

SevenZipWorker::FileType SevenZipWorker::initDir(const char *dir){
	//先把dir转换为绝对路径  
	if (_fullpath(m_szInitDir, dir, _MAX_PATH) == NULL)
		return NOTHING;

	//判断目录是否存在  
	if (_chdir(m_szInitDir) != 0){
		_finddata_t FileInfo;
		long Handle = _findfirst(m_szInitDir, &FileInfo);
		if (Handle != -1L){
			if (FileInfo.attrib & _A_ARCH){ // 判断是否为文件
				std::string filePath(m_szInitDir);
				int index = filePath.rfind("\\");
				std::string folderPath = filePath.substr(0, index+1);
				strncpy(m_folderDir, folderPath.c_str(), _MAX_PATH);
				fileNumber++;
				return ARCH;
			}
		}
		return NOTHING;
	}

	//如果目录的最后一个字母不是'\',则在最后加上一个'\'  
	int len = strlen(m_szInitDir);
	if (m_szInitDir[len - 1] != '\\'){
		strcat(m_szInitDir, "\\");
	}
	std::string filePath(m_szInitDir);
	//filePath.pop_back();
	int index = filePath.rfind("\\");
	std::string folderPath = filePath.substr(0, index+1);
	strncpy(m_folderDir, folderPath.c_str(), _MAX_PATH);

	return FOLDER;
}

bool SevenZipWorker::realTraveseDir(const char *dir, const char *filespec){
	_finddata_t fileInfo;
	//std::string strfind = std::string(dir) + std::string(filespec);
	char strfind[_MAX_PATH] = {0};
	strncpy(strfind, dir, _MAX_PATH);
	strcat(strfind, filespec);
	long handle = _findfirst(strfind, &fileInfo);
	if (handle != -1L){
		do{
			char filePath[_MAX_PATH] = {0};
			strncpy(filePath, dir, _MAX_PATH);
			strcat(filePath, fileInfo.name);
			//检查是不是目录  
			if (fileInfo.attrib & _A_SUBDIR){
				if ((strcmp(fileInfo.name, ".") != 0) && (strcmp(fileInfo.name, "..") != 0)){
					strcat(filePath, "\\");
					realTraveseDir(filePath, filespec);
					folderNumber++;
				}
			}
			else{
				m_allFilename.push_back(std::string(filePath));
				fileNumber++;
			}
		} while (_findnext(handle, &fileInfo) == 0);
		_findclose(handle);
	}
	return true;
}

bool SevenZipWorker::traveseDir(const char *dir){
	FileType type = SevenZipWorker::initDir(dir);
	if (type == ARCH){
		m_allFilename.push_back(m_szInitDir);
		return true;
	}
	else if (type == FOLDER){
		realTraveseDir(m_szInitDir, "*");
		return true;
	}
	return false;
}

void SevenZipWorker::displayFilenames(){
	for (auto item : m_allFilename){
		std::cout << item << std::endl;
	}
}

bool SevenZipWorker::compress(const char *in_path, const char *out_file_name){
#ifdef DEBUG_7Z
	clock_t beg_time, end_time;
	double use_time = 0.0;
	beg_time = clock();
#endif
	if (!traveseDir(in_path)){
		return false;
	}
	_chdir(m_folderDir);

	NT_CHECK

#ifdef DEBUG_7Z
	PrintStringLn(kCopyrightString);
#endif

	NDLL::CLibrary lib;
	if (!lib.Load(NDLL::GetModuleDirPrefix() + FTEXT(kDllName)))
	{
		PrintError("Can not load 7-zip library");
		return false;
	}

	Func_CreateObject createObjectFunc = (Func_CreateObject)lib.GetProc("CreateObject");
	if (!createObjectFunc)
	{
		PrintError("Can not get CreateObject");
		return false;
	}

	FString archiveName = CmdStringToFString(out_file_name);

	CObjectVector<CDirItem> dirItems;
	{
		for (auto &item : m_allFilename)
		{
			CDirItem di;
			std::string fileName = item.substr(strlen(m_folderDir));
			FString name = CmdStringToFString(fileName.c_str());

			NFind::CFileInfo fi;
			if (!fi.Find(name))
			{
				PrintError("Can't find file", name);
				return false;
			}

			di.Attrib = fi.Attrib;
			di.Size = fi.Size;
			di.CTime = fi.CTime;
			di.ATime = fi.ATime;
			di.MTime = fi.MTime;
			di.Name = fs2us(name);
			di.FullPath = name;
			dirItems.Add(di);
		}
	}

	COutFileStream *outFileStreamSpec = new COutFileStream;
	CMyComPtr<IOutStream> outFileStream = outFileStreamSpec;
	if (!outFileStreamSpec->Create(archiveName, false))
	{
		PrintError("can't create archive file");
		return false;
	}

	CMyComPtr<IOutArchive> outArchive;
	if (createObjectFunc(&CLSID_Format, &IID_IOutArchive, (void **)&outArchive) != S_OK)
	{
		PrintError("Can not get class object");
		return false;
	}

	CArchiveUpdateCallback *updateCallbackSpec = new CArchiveUpdateCallback;
	CMyComPtr<IArchiveUpdateCallback2> updateCallback(updateCallbackSpec);
	updateCallbackSpec->Init(&dirItems);

	HRESULT result = outArchive->UpdateItems(outFileStream, dirItems.Size(), updateCallback);

	updateCallbackSpec->Finilize();

	if (result != S_OK)
	{
		PrintError("Update Error");
		return false;
	}

	FOR_VECTOR(i, updateCallbackSpec->FailedFiles)
	{
		PrintNewLine();
		PrintError("Error for file", updateCallbackSpec->FailedFiles[i]);
	}

	if (updateCallbackSpec->FailedFiles.Size() != 0)
		return false;

#ifdef DEBUG_7Z
	end_time = clock();
	use_time = (double)(end_time - beg_time) / CLOCKS_PER_SEC;

	cleanLine();
	printf("finish!\n");
	printf("folders: %d\n", SevenZipWorker::folderNumber);
	printf("files: %d\n", SevenZipWorker::fileNumber);
	printf("use time: %d s\n", (int)use_time);
#endif
	return true;
}

bool SevenZipWorker::uncompress(const char *in_file_name, const char *out_path){
	NT_CHECK
#ifdef DEBUG_7Z
	clock_t beg_time, end_time;
	double use_time = 0.0;
	beg_time = clock();

	PrintStringLn(kCopyrightString);
#endif

	NDLL::CLibrary lib;
	if (!lib.Load(NDLL::GetModuleDirPrefix() + FTEXT(kDllName)))
	{
		PrintError("Can not load 7-zip library");
		return false;
	}

	Func_CreateObject createObjectFunc = (Func_CreateObject)lib.GetProc("CreateObject");
	if (!createObjectFunc)
	{
		PrintError("Can not get CreateObject");
		return false;
	}

	CMyComPtr<IInArchive> archive;
	if (createObjectFunc(&CLSID_Format, &IID_IInArchive, (void **)&archive) != S_OK)
	{
		PrintError("Can not get class object");
		return false;
	}

	CInFileStream *fileSpec = new CInFileStream;
	CMyComPtr<IInStream> file = fileSpec;

	FString archiveName = CmdStringToFString(in_file_name);
	if (!fileSpec->Open(archiveName))
	{
		PrintError("Can not open archive file", archiveName);
		return false;
	}

	{
		CArchiveOpenCallback *openCallbackSpec = new CArchiveOpenCallback;
		CMyComPtr<IArchiveOpenCallback> openCallback(openCallbackSpec);
		openCallbackSpec->PasswordIsDefined = false;

		const UInt64 scanSize = 1 << 23;
		if (archive->Open(file, &scanSize, openCallback) != S_OK)
		{
			PrintError("Can not open file as archive", archiveName);
			return false;
		}
	}

	UInt32 numItems = 0;
	archive->GetNumberOfItems(&numItems);
	SevenZipWorker::unZipFiles = numItems;

	{
		// Extract command
		CArchiveExtractCallback *extractCallbackSpec = new CArchiveExtractCallback;
		CMyComPtr<IArchiveExtractCallback> extractCallback(extractCallbackSpec);
		FString outPathName = CmdStringToFString(out_path);
		extractCallbackSpec->Init(archive, outPathName); // second parameter is output folder path
		extractCallbackSpec->PasswordIsDefined = false;

		HRESULT result = archive->Extract(NULL, (UInt32)(Int32)(-1), false, extractCallback);

		if (result != S_OK)
		{
			PrintError("Extract Error");
			return false;
		}
	}
#ifdef DEBUG_7Z
	end_time = clock();
	use_time = (double)(end_time - beg_time) / CLOCKS_PER_SEC;
	cleanLine();
	printf("finish!\n");
	printf("\nuse time: %d s\n", (int)use_time);
#endif
	return true;
}

bool SevenZipWorker::displayUnZipFiles(const char *in_file_name){
	NT_CHECK
#ifdef DEBUG_7Z
	PrintStringLn(kCopyrightString);
#endif

	NDLL::CLibrary lib;
	if (!lib.Load(NDLL::GetModuleDirPrefix() + FTEXT(kDllName)))
	{
		PrintError("Can not load 7-zip library");
		return false;
	}

	Func_CreateObject createObjectFunc = (Func_CreateObject)lib.GetProc("CreateObject");
	if (!createObjectFunc)
	{
		PrintError("Can not get CreateObject");
		return false;
	}

	CMyComPtr<IInArchive> archive;
	if (createObjectFunc(&CLSID_Format, &IID_IInArchive, (void **)&archive) != S_OK)
	{
		PrintError("Can not get class object");
		return false;
	}

	CInFileStream *fileSpec = new CInFileStream;
	CMyComPtr<IInStream> file = fileSpec;

	FString archiveName = CmdStringToFString(in_file_name);
	if (!fileSpec->Open(archiveName))
	{
		PrintError("Can not open archive file", archiveName);
		return false;
	}
	{
		CArchiveOpenCallback *openCallbackSpec = new CArchiveOpenCallback;
		CMyComPtr<IArchiveOpenCallback> openCallback(openCallbackSpec);
		openCallbackSpec->PasswordIsDefined = false;

		const UInt64 scanSize = 1 << 23;
		if (archive->Open(file, &scanSize, openCallback) != S_OK)
		{
			PrintError("Can not open file as archive", archiveName);
			return false;
		}
	}
	{
		// List command
		UInt32 numItems = 0;
		archive->GetNumberOfItems(&numItems);
		for (UInt32 i = 0; i < numItems; i++)
		{
			{
				// Get uncompressed size of file
				NCOM::CPropVariant prop;
				archive->GetProperty(i, kpidSize, &prop);
				char s[32];
				ConvertPropVariantToShortString(prop, s);
				PrintString(s);
				PrintString("  ");
			}
		{
			// Get name of file
			NCOM::CPropVariant prop;
			archive->GetProperty(i, kpidPath, &prop);
			if (prop.vt == VT_BSTR)
				PrintString(prop.bstrVal);
			else if (prop.vt != VT_EMPTY)
				PrintString("ERROR!");
		}
		PrintNewLine();
		}
	}
	return true;
}

#ifdef LIB_MAIN
int main(int args, char *uargs[]){
	if (args < 3){
		PrintStringLn(kHelpString);
		return 1;
	}

	std::string op(uargs[1]);
	if (op.length() != 1){
		PrintError("incorrect command");
		return 1;
	}

	if (op == "l"){
		SevenZipWorker::displayUnZipFiles(uargs[2]);
	}else if (op == "a"){
		SevenZipWorker::compress(uargs[2], uargs[3]);
	}
	else if(op == "x"){
		SevenZipWorker::uncompress(uargs[2], uargs[3]);
	}

	return 0;
}
#endif // LIB_MAIN

#if 0
int MY_CDECL main(int numArgs, const char *args[])
{
  NT_CHECK

  PrintStringLn(kCopyrightString);

  if (numArgs < 3)
  {
    PrintStringLn(kHelpString);
    return 1;
  }
  
  NDLL::CLibrary lib;
  if (!lib.Load(NDLL::GetModuleDirPrefix() + FTEXT(kDllName)))
  {
    PrintError("Can not load 7-zip library");
    return 1;
  }

  Func_CreateObject createObjectFunc = (Func_CreateObject)lib.GetProc("CreateObject");
  if (!createObjectFunc)
  {
    PrintError("Can not get CreateObject");
    return 1;
  }

  char c;
  {
    AString command = args[1];
    if (command.Len() != 1)
    {
      PrintError("incorrect command");
      return 1;
    }
    c = (char)MyCharLower_Ascii(command[0]);
  }

  FString archiveName = CmdStringToFString(args[2]);
  
  if (c == 'a')
  {
    // create archive command
    if (numArgs < 4)
    {
      PrintStringLn(kHelpString);
      return 1;
    }
    CObjectVector<CDirItem> dirItems;
    {
      int i;
      for (i = 3; i < numArgs; i++)
      {
        CDirItem di;
        FString name = CmdStringToFString(args[i]);
        
        NFind::CFileInfo fi;
        if (!fi.Find(name))
        {
          PrintError("Can't find file", name);
          return 1;
        }
        
        di.Attrib = fi.Attrib;
        di.Size = fi.Size;
        di.CTime = fi.CTime;
        di.ATime = fi.ATime;
        di.MTime = fi.MTime;
        di.Name = fs2us(name);
        di.FullPath = name;
        dirItems.Add(di);
      }
    }

    COutFileStream *outFileStreamSpec = new COutFileStream;
    CMyComPtr<IOutStream> outFileStream = outFileStreamSpec;
    if (!outFileStreamSpec->Create(archiveName, false))
    {
      PrintError("can't create archive file");
      return 1;
    }

    CMyComPtr<IOutArchive> outArchive;
    if (createObjectFunc(&CLSID_Format, &IID_IOutArchive, (void **)&outArchive) != S_OK)
    {
      PrintError("Can not get class object");
      return 1;
    }

    CArchiveUpdateCallback *updateCallbackSpec = new CArchiveUpdateCallback;
    CMyComPtr<IArchiveUpdateCallback2> updateCallback(updateCallbackSpec);
    updateCallbackSpec->Init(&dirItems);
    // updateCallbackSpec->PasswordIsDefined = true;
    // updateCallbackSpec->Password = L"1";

    /*
    {
      const wchar_t *names[] =
      {
        L"s",
        L"x"
      };
      const unsigned kNumProps = ARRAY_SIZE(names);
      NCOM::CPropVariant values[kNumProps] =
      {
        false,    // solid mode OFF
        (UInt32)9 // compression level = 9 - ultra
      };
      CMyComPtr<ISetProperties> setProperties;
      outArchive->QueryInterface(IID_ISetProperties, (void **)&setProperties);
      if (!setProperties)
      {
        PrintError("ISetProperties unsupported");
        return 1;
      }
      RINOK(setProperties->SetProperties(names, values, kNumProps));
    }
    */
    
    HRESULT result = outArchive->UpdateItems(outFileStream, dirItems.Size(), updateCallback);
    
    updateCallbackSpec->Finilize();
    
    if (result != S_OK)
    {
      PrintError("Update Error");
      return 1;
    }
    
    FOR_VECTOR (i, updateCallbackSpec->FailedFiles)
    {
      PrintNewLine();
      PrintError("Error for file", updateCallbackSpec->FailedFiles[i]);
    }
    
    if (updateCallbackSpec->FailedFiles.Size() != 0)
      return 1;
  }
  else
  {
    if (numArgs != 3)
    {
      PrintStringLn(kHelpString);
      return 1;
    }

    bool listCommand;
    
    if (c == 'l')
      listCommand = true;
    else if (c == 'x')
      listCommand = false;
    else
    {
      PrintError("incorrect command");
      return 1;
    }
  
    CMyComPtr<IInArchive> archive;
    if (createObjectFunc(&CLSID_Format, &IID_IInArchive, (void **)&archive) != S_OK)
    {
      PrintError("Can not get class object");
      return 1;
    }
    
    CInFileStream *fileSpec = new CInFileStream;
    CMyComPtr<IInStream> file = fileSpec;
    
    if (!fileSpec->Open(archiveName))
    {
      PrintError("Can not open archive file", archiveName);
      return 1;
    }

    {
      CArchiveOpenCallback *openCallbackSpec = new CArchiveOpenCallback;
      CMyComPtr<IArchiveOpenCallback> openCallback(openCallbackSpec);
      openCallbackSpec->PasswordIsDefined = false;
      // openCallbackSpec->PasswordIsDefined = true;
      // openCallbackSpec->Password = L"1";
      
      const UInt64 scanSize = 1 << 23;
      if (archive->Open(file, &scanSize, openCallback) != S_OK)
      {
        PrintError("Can not open file as archive", archiveName);
        return 1;
      }
    }
    
    if (listCommand)
    {
      // List command
      UInt32 numItems = 0;
      archive->GetNumberOfItems(&numItems);
      for (UInt32 i = 0; i < numItems; i++)
      {
        {
          // Get uncompressed size of file
          NCOM::CPropVariant prop;
          archive->GetProperty(i, kpidSize, &prop);
          char s[32];
          ConvertPropVariantToShortString(prop, s);
          PrintString(s);
          PrintString("  ");
        }
        {
          // Get name of file
          NCOM::CPropVariant prop;
          archive->GetProperty(i, kpidPath, &prop);
          if (prop.vt == VT_BSTR)
            PrintString(prop.bstrVal);
          else if (prop.vt != VT_EMPTY)
            PrintString("ERROR!");
        }
        PrintNewLine();
      }
    }
    else
    {
      // Extract command
      CArchiveExtractCallback *extractCallbackSpec = new CArchiveExtractCallback;
      CMyComPtr<IArchiveExtractCallback> extractCallback(extractCallbackSpec);
      extractCallbackSpec->Init(archive, FTEXT("")); // second parameter is output folder path
      extractCallbackSpec->PasswordIsDefined = false;
      // extractCallbackSpec->PasswordIsDefined = true;
      // extractCallbackSpec->Password = L"1";

      /*
      const wchar_t *names[] =
      {
        L"mt",
        L"mtf"
      };
      const unsigned kNumProps = sizeof(names) / sizeof(names[0]);
      NCOM::CPropVariant values[kNumProps] =
      {
        (UInt32)1,
        false
      };
      CMyComPtr<ISetProperties> setProperties;
      archive->QueryInterface(IID_ISetProperties, (void **)&setProperties);
      if (setProperties)
        setProperties->SetProperties(names, values, kNumProps);
      */

      HRESULT result = archive->Extract(NULL, (UInt32)(Int32)(-1), false, extractCallback);
  
      if (result != S_OK)
      {
        PrintError("Extract Error");
        return 1;
      }
    }
  }

  return 0;
}
#endif // end it's main
