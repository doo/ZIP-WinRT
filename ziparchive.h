#pragma once

#include <collection.h>
#include <ppltasks.h>

namespace runtime {
  namespace doo {
    namespace zip {
      typedef Windows::Foundation::IAsyncOperation<Windows::Storage::Streams::IBuffer^>^ 
        AsyncBufferOperation;

      public ref class ZipArchiveEntry sealed {
        friend ref class ZipArchive;
      public:
        property Platform::String^ Filename {
          Platform::String^ get() {
            return filename;
          }
        }

        property uint32 CompressedSize {
          uint32 get() {
            return localHeader.compressedSize;
          }
        }

        property uint32 UncompressedSize {
          uint32 get() {
            return localHeader.uncompressedSize;
          }
        }

        property boolean IsDirectory {
          boolean get() {
            return filename->Data()[filename->Length()-1] == '/';
          }
        }

      private:
        ZipArchiveEntry(
          Windows::Storage::Streams::IRandomAccessStream^ centralDirectoryRecordStream
          );

        AsyncBufferOperation GetUncompressedFileContents(
          Windows::Storage::Streams::IRandomAccessStream^ stream
          );

        Windows::Foundation::IAsyncAction^ ExtractAsync(
          Windows::Storage::Streams::IRandomAccessStream^ stream,
          Windows::Storage::IStorageFile^ destination
          );

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

        Platform::String^ filename;
        Platform::String^ extraField;
        DWORD64 contentStreamStart;

        void ReadAndCheckLocalHeader(Windows::Storage::Streams::IInputStream^ stream);
        Windows::Storage::Streams::IBuffer^ DeflateFromStream(
          Windows::Storage::Streams::IInputStream^ stream, 
          const concurrency::cancellation_token& cancellationToken
          );
        void DeflateFromStreamToFile(
          Windows::Storage::Streams::IInputStream^ stream,
          FILE* out,
          const concurrency::cancellation_token& cancellationToken
          );
        void CopyFromStreamToFile(
          Windows::Storage::Streams::IInputStream^ stream,
          FILE* out,
          const concurrency::cancellation_token& cancellationToken
          );
        Windows::Storage::Streams::IBuffer^ UncompressedFromStream(
          Windows::Storage::Streams::IInputStream^ stream, 
          unsigned int maxBufSize,
          const concurrency::cancellation_token& cancellationToken
          );
      };

      // the main archive class
      public ref class ZipArchive sealed {
        typedef Windows::Foundation::IAsyncOperation<ZipArchive^>^ AsyncZipArchiveOperation;
      public:
        static AsyncZipArchiveOperation CreateFromFileAsync(
          Windows::Storage::IStorageFile^ file
          );
        static AsyncZipArchiveOperation CreateFromStreamReferenceAsync(
          Windows::Storage::Streams::RandomAccessStreamReference^ reference
          );

        AsyncBufferOperation GetFileContentsAsync(Platform::String^ filename);
        Windows::Foundation::IAsyncAction^ ExtractFileAsync(
          Platform::String^ filename, 
          Windows::Storage::IStorageFile^ destination);
        Windows::Foundation::IAsyncAction^ ExtractFileToFolderAsync(
          Platform::String^ filename,
          Windows::Storage::IStorageFolder^ destinationFolder
          );
        Windows::Foundation::IAsyncAction^ ExtractAllAsync(
          Windows::Storage::IStorageFolder^ destination);

        property Platform::Array<ZipArchiveEntry^>^ Files {
          Platform::Array<ZipArchiveEntry^>^ get() {
            return archiveEntries;
          };
        }

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
        Windows::Storage::Streams::IRandomAccessStream^ randomAccessStream;
        concurrency::task<Windows::Storage::IStorageFile^> 
          CreateFileInFolderAsync(
            Windows::Storage::IStorageFolder^ parent, 
            const std::wstring& filename);

        ZipArchive(
          Windows::Storage::Streams::IRandomAccessStream^ stream, 
          concurrency::cancellation_token cancellationToken
          );
      };
    }
  }
}