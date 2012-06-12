#pragma once

#include <collection.h>
#include <ppltasks.h>

using Windows::Foundation::IAsyncOperation;
using Windows::Storage::Streams::IInputStream;
using Windows::Storage::Streams::IRandomAccessStream;
using Windows::Storage::Streams::IBuffer;

using Platform::String;
using Platform::Array;

using concurrency::cancellation_token;

namespace runtime {
  namespace doo {
    namespace zip {

      private ref class ZipArchiveEntry sealed {
#pragma pack(1)
        struct LocalFileHeader {
          uint32 signature;
          uint16 version;
          uint16 flags;
          uint16 compressionMethod;
          uint16 lastModifiedTime;
          uint16 lastModifiedDate;
          uint32 crc32;
          uint32 compressedSize;
          uint32 uncompressedSize;
          uint16 filenameLength;
          uint16 extraFieldLength;
        } localHeader;

        struct CentralDirectoryRecord {
          uint32 signature;
          uint16 versionCreated;
          uint16 versionNeeded;
          uint16 flags;
          uint16 compressionMethod;
          uint16 lastModifiedTime;
          uint16 lastModifiedDate;
          uint32 crc32;
          uint32 compressedSize;
          uint32 uncompressedSize;
          uint16 filenameLength;
          uint16 extraFieldLength;
          uint16 fileCommentLength;
          uint16 diskNumberStart;
          uint16 internalFileAttributes;
          uint32 externalFileAttributes;
          uint32 localHeaderOffset;
        } centralDirectoryRecord;
#pragma pack()

        String^ filename;
        String^ extraField;
        DWORD64 contentStreamStart;

        void readAndCheckLocalHeader(IInputStream^ stream);
        IBuffer^ deflateFromStream(IInputStream^ stream, cancellation_token cancellationToken);
        IBuffer^ uncompressedFromStream(IInputStream^ stream, cancellation_token cancellationToken);

      public:
        ZipArchiveEntry(IRandomAccessStream^ centralDirectoryRecordStream);

        IAsyncOperation<IBuffer^>^ getUncompressedFileContents(IRandomAccessStream^ stream);

        property String^ Filename {
          String^ get() {
            return filename;
          }
        }

        property boolean isDirectory {
          boolean get() {
            return filename->Data()[filename->Length()-1] == '/';
          }
        }
      };

      public ref class ZipArchive sealed {
        private:
#pragma pack(1)
          struct EndOfCentralDirectoryRecord {
            uint32 signature;
            uint16 diskNumber;
            uint16 directoryDiskNumber;
            uint16 entryCountThisDisk;
            uint16 entryCountTotal;
            uint32 centralDirectorySize;
            uint32 centralDirectoryOffset;
            uint16 zipFileCommentLength;
          } endOfCentralDirectoryRecord;
#pragma pack()

          Platform::Array<ZipArchiveEntry^>^ archiveEntries;
          IRandomAccessStream^ randomAccessStream;

          ZipArchive(IRandomAccessStream^ stream, cancellation_token cancellationToken);

        public:
         static IAsyncOperation<ZipArchive^>^ createFromFile(Windows::Storage::IStorageFile^ file);
         static IAsyncOperation<ZipArchive^>^ createFromStreamReference(Windows::Storage::Streams::RandomAccessStreamReference^ reference);

         IAsyncOperation<IBuffer^>^ getFileContentsAsync(String^ filename);

         property Platform::Array<String^>^ files {
           Platform::Array<String^>^ get() {
             std::vector<String^> fileList;
             for (ZipArchiveEntry^* zipFileEntry = archiveEntries->begin(); zipFileEntry != archiveEntries->end(); zipFileEntry++) {
               if (!(*zipFileEntry)->isDirectory) {
                fileList.push_back((*zipFileEntry)->Filename);
               }
             }
             unsigned int arraySize = static_cast<unsigned int>(fileList.size());
             Platform::Array<String^>^ result = ref new Platform::Array<String^>(arraySize);
             for (unsigned int i = 0; i < fileList.size(); i++) {
               result[i] = fileList.at(i);
             }
             return result;
           }
         }
      };
    }
  }
}