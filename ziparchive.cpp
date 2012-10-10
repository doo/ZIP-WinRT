
#include <wrl/client.h>
#include <collection.h>
#include <robuffer.h>

#include <ppl.h>
#include <ppltasks.h>

#include "tinfl.c"

#include "ziparchive.h"

using namespace runtime::doo::zip;

using Platform::String;
using Platform::Array;

using Microsoft::WRL::ComPtr;

using Windows::Foundation::IAsyncOperation;
using Windows::Foundation::IAsyncAction;
using Windows::Storage::Streams::IBuffer;
using Windows::Storage::Streams::IBufferByteAccess;
using Windows::Storage::Streams::IInputStream;
using Windows::Storage::Streams::IRandomAccessStream;
using Windows::Storage::IStorageFile;
using Windows::Storage::IStorageFolder;

using concurrency::cancellation_token;

// the expected signatures for different parts of a ZIP file
#define ZipArchive_ENTRY_LOCAL_HEADER_SIGNATURE 0x04034b50
#define ZipArchive_CENTRAL_DIRECTORY_RECORD_SIGNATURE 0x02014b50
#define ZipArchive_END_OF_CENTRAL_RECORD_SIGNATURE 0x06054b50

static ComPtr<IBufferByteAccess> getByteAccessForBuffer(IBuffer^ buffer) {
  ComPtr<IUnknown> comBuffer(reinterpret_cast<IUnknown*>(buffer));
  ComPtr<IBufferByteAccess> byteBuffer;
  comBuffer.As(&byteBuffer);
  return byteBuffer;
}

// Helper method to comfortably read data from an IDataReader into a memory location
static void readBytesFromDataReader(Windows::Storage::Streams::IDataReader^ dataReader, 
                             uint32 length, void* destination, size_t destSize) {
  uint32 read = 0;
  while (read < length) {
    concurrency::task<uint32> readDataTask(dataReader->LoadAsync(length-read));
    read += readDataTask.get();
  }
  auto buffer = dataReader->ReadBuffer(length);
  auto byteBuffer = getByteAccessForBuffer(buffer);
  byte* data;
  byteBuffer->Buffer(&data);
  memcpy_s(destination, destSize, data, length);
}

static String^ charToPlatformString(const char* strData) {
  std::string stdString(strData);
  std::wstring stdWString;
  stdWString.assign(stdString.begin(), stdString.end());
  String^ result = ref new String(stdWString.c_str());
  return result;
}

// interpret the next length bytes from the stream as a string
// since strings in ZIP files aren't null-terminated, add trailing null
static String^ readString(IInputStream^ stream, uint16 length) {
  char* buffer = new char[length+1];
  auto dataReader = ref new Windows::Storage::Streams::DataReader(stream);
  readBytesFromDataReader(dataReader, length, buffer, length+1);
  dataReader->DetachStream();
  buffer[length] = 0x0;
  String^ result = charToPlatformString(buffer);
  delete[] buffer;
  return result;
}

/************************************************************************/
/* Instantiate a ZipArchiveEntry from a stream positioned at the central   */
/* directory record for a file. Will leave the stream positioned at     */
/* the beginning of the next record.                                    */
/************************************************************************/
ZipArchiveEntry::ZipArchiveEntry(IRandomAccessStream^ stream) {
  auto dataReader = ref new Windows::Storage::Streams::DataReader(stream);
  
  memset(&centralDirectoryRecord, 0, sizeof(centralDirectoryRecord));
  memset(&localHeader, 0, sizeof(localHeader));

  readBytesFromDataReader(
    dataReader, 
    sizeof(ZipArchiveEntry::CentralDirectoryRecord), 
    &centralDirectoryRecord, 
    sizeof(centralDirectoryRecord));
  // make sure the stream stays usable for the next record
  dataReader->DetachStream();

  if (centralDirectoryRecord.signature != ZipArchive_CENTRAL_DIRECTORY_RECORD_SIGNATURE) {
    throw ref new Platform::FailureException(L"Invalid ZIP file entry header");
  }

  filename = readString(stream, centralDirectoryRecord.filenameLength);

  IInputStream^ localHeaderInputStream = 
    stream->GetInputStreamAt(centralDirectoryRecord.localHeaderOffset);
  ReadAndCheckLocalHeader(localHeaderInputStream);
  
  contentStreamStart = centralDirectoryRecord.localHeaderOffset +
    + sizeof(LocalFileHeader) 
    + localHeader.filenameLength 
    + localHeader.extraFieldLength;

  // make sure the stream is ready to read the next header
  if (centralDirectoryRecord.extraFieldLength > 0) {
    stream->Seek(stream->Position + centralDirectoryRecord.extraFieldLength);
  }
}

/************************************************************************/
/* Read the local header and check it against the central directory     */
/************************************************************************/
void ZipArchiveEntry::ReadAndCheckLocalHeader(IInputStream^ stream) {
  auto dataReader = ref new Windows::Storage::Streams::DataReader(stream);
  readBytesFromDataReader(dataReader, sizeof(LocalFileHeader), &localHeader, sizeof(localHeader));
  if (localHeader.signature != ZipArchive_ENTRY_LOCAL_HEADER_SIGNATURE) {
    throw ref new Platform::FailureException(L"Invalid local header: " + filename);
  }
  String^ localFilename = readString(stream, localHeader.filenameLength);
  if (wcscmp(localFilename->Data(), filename->Data()) != 0) {
    throw ref new Platform::FailureException(
      L"Filename in local header does not match: " + filename + L" : " + localFilename);
  }
}

/************************************************************************/
/* The file isn't compressed, just pass it through from the stream      */
/* If maxBufSize is larger 0, the buffer size will be limitied          */
/************************************************************************/
IBuffer^ ZipArchiveEntry::UncompressedFromStream(IInputStream^ stream, 
                                              unsigned int maxBufSize,
                                              const cancellation_token& cancellationToken) {
  auto dataReader = ref new Windows::Storage::Streams::DataReader(stream);
  uint32 bytesToRead = centralDirectoryRecord.compressedSize;
  if (maxBufSize > 0 && maxBufSize < bytesToRead) {
    bytesToRead = maxBufSize;
  }

  uint32 bytesRead = 0;
  while (bytesRead < bytesToRead) {
    concurrency::task<uint32> loadDataTask(dataReader->LoadAsync(bytesToRead-bytesRead));
    bytesRead += loadDataTask.get();
  }

  Windows::Storage::Streams::IBuffer^ result = dataReader->ReadBuffer(bytesToRead);
  return result;
}

/************************************************************************/
/* Decompress a file compressed using the DEFLATE algorithm             */
/************************************************************************/
IBuffer^ ZipArchiveEntry::DeflateFromStream(IInputStream^ stream, 
                                            const cancellation_token& cancellationToken) {
  IBuffer^ compressedBuffer = UncompressedFromStream(stream, 0, cancellationToken);
  
  if (cancellationToken.is_canceled()) { return nullptr; }

  auto compressedBufferByteAccess = getByteAccessForBuffer(compressedBuffer);
  byte* data;
  compressedBufferByteAccess->Buffer(&data);
  // allocate buffer for decompression
  Platform::Array<byte>^ decompressedData = 
    ref new Platform::Array<byte>(centralDirectoryRecord.uncompressedSize);

  auto decompressionResult = tinfl_decompress_mem_to_mem(
    decompressedData->Data,
    centralDirectoryRecord.uncompressedSize, 
    data, 
    centralDirectoryRecord.compressedSize, 
    0);

  if (decompressionResult != centralDirectoryRecord.uncompressedSize) {
    throw ref new Platform::FailureException(L"Could not extract data for file " + filename);
  }

  Windows::Storage::Streams::DataWriter^ writer = ref new Windows::Storage::Streams::DataWriter();
  writer->WriteBytes(decompressedData);
  return writer->DetachBuffer();
}

int __cdecl decompressCallback(const void *buf, int len, void *pUser) {
  auto outFile = reinterpret_cast<FILE*>(pUser);
  if (fwrite(buf, 1, len, outFile) == len) {
    return 1;
  } else {
    return 0;
  }
}

void ZipArchiveEntry::DeflateFromStreamToFile( 
  Windows::Storage::Streams::IInputStream^ in, 
  FILE* out, 
  const concurrency::cancellation_token& cancellationToken ) {
    // for now just read the whole uncompressed buffer into memory
    // this can probably be optimized later on
    IBuffer^ compressedBuffer = UncompressedFromStream(in, 0, cancellationToken);

    if (cancellationToken.is_canceled()) { return; }

    auto compressedBufferByteAccess = getByteAccessForBuffer(compressedBuffer);
    byte* data;
    compressedBufferByteAccess->Buffer(&data);
    size_t compressedBufferSize = compressedBuffer->Length;
    auto decompressionResult = tinfl_decompress_mem_to_callback(
      data,
      &compressedBufferSize, 
      decompressCallback, 
      reinterpret_cast<void*>(out), 
      0);

    if (decompressionResult != 1) {
      throw ref new Platform::FailureException(L"Could not extract data for file " + filename);
    }
}

#define BUFSIZE 1024*1024
void ZipArchiveEntry::CopyFromStreamToFile(Windows::Storage::Streams::IInputStream^ stream, 
  FILE* out, 
  const concurrency::cancellation_token& cancellationToken ) {
    unsigned int written = 0;
    while (written < centralDirectoryRecord.uncompressedSize) {
      if (cancellationToken.is_canceled()) {
        concurrency::cancel_current_task();
      }
      unsigned int bytesToRead = min(BUFSIZE, centralDirectoryRecord.uncompressedSize-written);
      IBuffer^ buf = UncompressedFromStream(stream, bytesToRead, cancellationToken);
      auto compressedBufferByteAccess = getByteAccessForBuffer(buf);
      byte* data;
      compressedBufferByteAccess->Buffer(&data);
      fwrite(data, sizeof(byte), buf->Length, out);
      written += buf->Length;
    }
}

IAsyncAction^ ZipArchiveEntry::ExtractAsync(IRandomAccessStream^ stream, 
  Windows::Storage::IStorageFile^ destination) {
  return concurrency::create_async([=](cancellation_token cancellationToken) {
    FILE* fileHandle;
    auto openResult = _wfopen_s(&fileHandle, destination->Path->Data(), L"wb");
    if (openResult != 0) {
      throw ref new Platform::AccessDeniedException("Could not write to file " + destination->Path);
    }
    auto outFile = std::shared_ptr<FILE>(fileHandle, [](FILE* ptr) {
      fclose(ptr);
    });
    IInputStream^ zipArchiveDataInputStream = stream->GetInputStreamAt(contentStreamStart);
    switch (centralDirectoryRecord.compressionMethod) {
      case 0: // file is uncompressed, read it in chunks
        CopyFromStreamToFile(zipArchiveDataInputStream, outFile.get(), cancellationToken);
        break;
      case 8: // deflate
        DeflateFromStreamToFile(zipArchiveDataInputStream, outFile.get(), cancellationToken);
        break;
    }
  });
}

IAsyncOperation<IBuffer^>^ ZipArchiveEntry::GetUncompressedFileContents(
  IRandomAccessStream^ stream) {
  return concurrency::create_async([=](cancellation_token cancellationToken) {
    IInputStream^ zipArchiveDataInputStream = stream->GetInputStreamAt(contentStreamStart);
    switch (centralDirectoryRecord.compressionMethod) {
    case 0:  // file is uncompressed
      return UncompressedFromStream(zipArchiveDataInputStream, 0, cancellationToken);
    case 8: // deflate
      return DeflateFromStream(zipArchiveDataInputStream, cancellationToken);
    default:
      throw ref new Platform::FailureException(L"Compression algorithm not supported: " + 
        centralDirectoryRecord.compressionMethod);
    }
  });
}

/************************************************************************/
/* Instantiate the ZipArchive and read its directory of contents        */
/************************************************************************/
ZipArchive::ZipArchive(IRandomAccessStream^ stream, cancellation_token cancellationToken) {
  randomAccessStream = stream;

  // the central directory record is located at the end of the file
  randomAccessStream->Seek(randomAccessStream->Size - sizeof(EndOfCentralDirectoryRecord));
  auto dataReader = ref new Windows::Storage::Streams::DataReader(randomAccessStream);
  readBytesFromDataReader(
    dataReader, 
    sizeof(ZipArchive::EndOfCentralDirectoryRecord), 
    &endOfCentralDirectoryRecord, 
    sizeof(endOfCentralDirectoryRecord));
  dataReader->DetachStream(); // done reading but keep the stream usuable

  if (endOfCentralDirectoryRecord.signature != ZipArchive_END_OF_CENTRAL_RECORD_SIGNATURE) {
    throw ref new Platform::FailureException("Could not read ZIP file");
  }
  if (cancellationToken.is_canceled()) {
    return;
  }
  archiveEntries = ref new Array<ZipArchiveEntry^>(endOfCentralDirectoryRecord.entryCountThisDisk);
  stream->Seek(endOfCentralDirectoryRecord.centralDirectoryOffset);
  for (int i = 0; i < endOfCentralDirectoryRecord.entryCountThisDisk; i++) {
    archiveEntries[i] = ref new ZipArchiveEntry(randomAccessStream);
    if (cancellationToken.is_canceled()) {
      return;
    }
  }
}

/************************************************************************/
/* Constructor function to create a zip archive from a stream reference */
/************************************************************************/
IAsyncOperation<ZipArchive^>^ ZipArchive::CreateFromStreamReferenceAsync(
  Windows::Storage::Streams::RandomAccessStreamReference^ reference) {
  return concurrency::create_async([=](cancellation_token cancellationToken) -> ZipArchive^ {
    auto streamOpenTask = 
      concurrency::task<Windows::Storage::Streams::IRandomAccessStreamWithContentType^>(
      reference->OpenReadAsync());
    auto createZipArchiveTask = streamOpenTask.then(
      [=](Windows::Storage::Streams::IRandomAccessStreamWithContentType^ stream) -> ZipArchive^ {
      return ref new ZipArchive(stream, cancellationToken);
    }, concurrency::task_continuation_context::use_arbitrary());
    return createZipArchiveTask.get();
  });
}

/************************************************************************/
/* Instantiate a ZipArchive object from an IStorageFile                 */
/************************************************************************/
IAsyncOperation<ZipArchive^>^ ZipArchive::CreateFromFileAsync(IStorageFile^ file) {
  return concurrency::create_async([=](cancellation_token cancellationToken) -> ZipArchive^ {
    auto fileOpenTask = concurrency::task<IRandomAccessStream^>(
      file->OpenAsync(Windows::Storage::FileAccessMode::Read));
    auto createZipArchiveTask = fileOpenTask.then([=](IRandomAccessStream^ stream) -> ZipArchive^ {
      return ref new ZipArchive(stream, cancellationToken);
    } , concurrency::task_continuation_context::use_arbitrary());
    return createZipArchiveTask.get();
    });
}

/************************************************************************/
/* Get the uncompressed file contents as an IBuffer                     */
/************************************************************************/
IAsyncOperation<IBuffer^>^ ZipArchive::GetFileContentsAsync(String^ filename) {
  return concurrency::create_async([=]() -> IBuffer^ {
    for (unsigned int i = 0; i < archiveEntries->Length; i++) {
      if (wcscmp(archiveEntries[i]->Filename->Data(), filename->Data()) == 0) {
        if (concurrency::is_task_cancellation_requested()) {
          concurrency::cancel_current_task();
        }
        concurrency::task<IBuffer^> uncompressTask(archiveEntries[i]->GetUncompressedFileContents(randomAccessStream));
        return uncompressTask.get();
      }
    }
    return nullptr;
  });
}

concurrency::task<IStorageFile^> ZipArchive::CreateFileInFolderAsync(
  IStorageFolder^ parent, const std::wstring& filename) {

  std::wstring currentFilename = filename;
  concurrency::task<IStorageFolder^> antecedent = concurrency::create_task([parent]() {
    return parent;
  });

  while (true) {
    auto directorySeperatorPos = currentFilename.find(L"/");

    if (directorySeperatorPos == std::wstring::npos) {
      return antecedent.then([currentFilename](IStorageFolder^ parent) {
        return reinterpret_cast<IAsyncOperation<IStorageFile^>^>(parent->CreateFileAsync(ref new Platform::String(currentFilename.c_str()),
          Windows::Storage::CreationCollisionOption::ReplaceExisting));
      }, concurrency::task_continuation_context::use_arbitrary());
    } else {
      auto dirname = currentFilename.substr(0, directorySeperatorPos);
      currentFilename = filename.substr(directorySeperatorPos+1, std::wstring::npos);
      antecedent = antecedent.then([dirname](IStorageFolder^ parent) {
        return reinterpret_cast<IAsyncOperation<IStorageFolder^>^>(parent->CreateFolderAsync(ref new Platform::String(dirname.c_str()), 
          Windows::Storage::CreationCollisionOption::OpenIfExists));
      }, concurrency::task_continuation_context::use_arbitrary());
      
    }
  }
}
IAsyncAction^ ZipArchive::ExtractAllAsync(IStorageFolder^ destination) {
  return concurrency::create_async([this, destination]() -> void {
    std::vector<concurrency::task<void>> copyOperations;
    for (unsigned int i = 0; i < archiveEntries->Length; i++) {
      std::wstring filename = archiveEntries[i]->Filename->Data();
      if (filename[filename.length()-1] != '/') {
        auto extractTask = concurrency::task<void>(CreateFileInFolderAsync(destination, filename).then([this, i](IStorageFile^ file) {
          return archiveEntries[i]->ExtractAsync(randomAccessStream, file);
        }));
        copyOperations.push_back(extractTask);
      }
    }
    concurrency::when_all(copyOperations.begin(), copyOperations.end()).wait();
  });
}

IAsyncAction^ ZipArchive::ExtractFileAsync(Platform::String^ filename, IStorageFile^ destination) {
  const wchar_t* fileToExtract = filename->Data();
  for (unsigned int i = 0; i < archiveEntries->Length; i++) {
    const wchar_t* currentFilename = archiveEntries[i]->Filename->Data();
    if (wcscmp(fileToExtract, currentFilename) == 0) {
      return archiveEntries[i]->ExtractAsync(randomAccessStream, destination);
    }
  }
  return concurrency::create_async([filename]() {
    Platform::String^ errorMessage = ref new Platform::String(L"File not found: ") + filename;
    throw ref new Platform::InvalidArgumentException(errorMessage);
  });
}

IAsyncAction^ ZipArchive::ExtractFileToFolderAsync(Platform::String^ filename, IStorageFolder^ destination) {
  return concurrency::create_async([=]() {
    return CreateFileInFolderAsync(destination, filename->Data()).then([this, &filename](IStorageFile^ file) {
      return ExtractFileAsync(filename, file);
    });
  });
}