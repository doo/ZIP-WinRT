
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
using Windows::Storage::Streams::IBuffer;
using Windows::Storage::Streams::IBufferByteAccess;
using Windows::Storage::Streams::IInputStream;
using Windows::Storage::Streams::IRandomAccessStream;

using concurrency::cancellation_token;

// the expected signatures for different parts of a ZIP file
#define ZipArchive_ENTRY_LOCAL_HEADER_SIGNATURE 0x04034b50
#define ZipArchive_CENTRAL_DIRECTORY_RECORD_SIGNATURE 0x02014b50
#define ZipArchive_END_OF_CENTRAL_RECORD_SIGNATURE 0x06054b50

ComPtr<IBufferByteAccess> getByteAccessForBuffer(IBuffer^ buffer) {
  ComPtr<IUnknown> comBuffer(reinterpret_cast<IUnknown*>(buffer));
  ComPtr<IBufferByteAccess> byteBuffer;
  comBuffer.As(&byteBuffer);
  return byteBuffer;
}

// Helper method to comfortably read data from an IDataReader into a memory location
void readBytesFromDataReader(Windows::Storage::Streams::IDataReader^ dataReader, 
                             uint32 length, void* destination, size_t destSize) {
  concurrency::task<uint32> readDataTask(dataReader->LoadAsync(length));
  uint32 bytesRead = readDataTask.get();
  if (bytesRead != length) {
    throw ref new Platform::FailureException(L"Could not read expected amount of data");
  }
  auto buffer = dataReader->ReadBuffer(length);
  auto byteBuffer = getByteAccessForBuffer(buffer);
  byte* data;
  byteBuffer->Buffer(&data);
  memcpy_s(destination, destSize, data, length);
}

String^ charToPlatformString(const char* strData) {
  std::string stdString(strData);
  std::wstring stdWString;
  stdWString.assign(stdString.begin(), stdString.end());
  String^ result = ref new String(stdWString.c_str());
  return result;
}

// interpret the next length bytes from the stream as a string
// since strings in ZIP files aren't null-terminated, add trailing null
String^ readString(IInputStream^ stream, uint16 length) {
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
  // make sure the stream stays usuable for the next record
  dataReader->DetachStream();

  if (centralDirectoryRecord.signature != ZipArchive_CENTRAL_DIRECTORY_RECORD_SIGNATURE) {
    throw ref new Platform::FailureException(L"Invalid ZIP file entry header");
  }

  filename = readString(stream, centralDirectoryRecord.filenameLength);

  IInputStream^ localHeaderInputStream = 
    stream->GetInputStreamAt(centralDirectoryRecord.localHeaderOffset);
  readAndCheckLocalHeader(localHeaderInputStream);
  
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
void ZipArchiveEntry::readAndCheckLocalHeader(IInputStream^ stream) {
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
/************************************************************************/
IBuffer^ ZipArchiveEntry::uncompressedFromStream(IInputStream^ stream, 
                                              cancellation_token cancellationToken) {
  auto dataReader = ref new Windows::Storage::Streams::DataReader(stream);
  concurrency::task<uint32> loadDataTask(
    dataReader->LoadAsync(centralDirectoryRecord.compressedSize));
  uint32 bytesRead = loadDataTask.get();
  if (bytesRead != centralDirectoryRecord.compressedSize) {
    throw ref new Platform::FailureException(L"Could not read file data: " + filename);
  }
  Windows::Storage::Streams::IBuffer^ result = 
    dataReader->ReadBuffer(centralDirectoryRecord.compressedSize);
  return result;
}

/************************************************************************/
/* Decompress a file compressed using the DEFLATE algorithm             */
/************************************************************************/
IBuffer^ ZipArchiveEntry::deflateFromStream(IInputStream^ stream, 
                                            cancellation_token cancellationToken) {
  IBuffer^ compressedBuffer = uncompressedFromStream(stream, cancellationToken);
  
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

IAsyncOperation<IBuffer^>^ ZipArchiveEntry::getUncompressedFileContents(
  IRandomAccessStream^ stream) {
  return concurrency::create_async([=](cancellation_token cancellationToken) {
    IInputStream^ ZipArchiveDataInputStream = stream->GetInputStreamAt(contentStreamStart);
    switch (centralDirectoryRecord.compressionMethod) {
    case 0:  // file is uncompressed
      return uncompressedFromStream(ZipArchiveDataInputStream, cancellationToken);
    case 8: // deflate
      return deflateFromStream(ZipArchiveDataInputStream, cancellationToken);
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
IAsyncOperation<ZipArchive^>^ ZipArchive::createFromStreamReference(
  Windows::Storage::Streams::RandomAccessStreamReference^ reference) {
  return concurrency::create_async([=](cancellation_token cancellationToken) -> ZipArchive^ {
    auto streamOpenTask = 
      concurrency::task<Windows::Storage::Streams::IRandomAccessStreamWithContentType^>(
      reference->OpenReadAsync());
    auto createZipArchivetask = streamOpenTask.then(
      [=](Windows::Storage::Streams::IRandomAccessStreamWithContentType^ stream) -> ZipArchive^ {
      return ref new ZipArchive(stream, cancellationToken);
    }, concurrency::task_continuation_context::use_arbitrary());
    return createZipArchivetask.get();
  });
}

/************************************************************************/
/* Instantiate a ZipArchive object from an IStorageFile                 */
/************************************************************************/
IAsyncOperation<ZipArchive^>^ ZipArchive::createFromFile(Windows::Storage::IStorageFile^ file) {
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
IAsyncOperation<Windows::Storage::Streams::IBuffer^>^ ZipArchive::getFileContentsAsync(String^ filename) {
  return concurrency::create_async([=]() -> Windows::Storage::Streams::IBuffer^ {
    for (unsigned int i = 0; i < archiveEntries->Length; i++) {
      if (wcscmp(archiveEntries[i]->Filename->Data(), filename->Data()) == 0) {
        if (concurrency::is_task_cancellation_requested()) {
          concurrency::cancel_current_task();
        }
        concurrency::task<IBuffer^> uncompressTask(archiveEntries[i]->getUncompressedFileContents(randomAccessStream));
        return uncompressTask.get();
      }
    }
    return nullptr;
  });
}