(function () {
  "use strict";

  var CreationCollisionOption = Windows.Storage.CreationCollisionOption,
      RandomAccessStreamReference = Windows.Storage.Streams.RandomAccessStreamReference,
      ZipArchive = runtime.doo.zip.ZipArchive;

  describe('Zip component', function() {

    it('should handle OpenDocument containers', function () {
      return spec.async(function() {
        var stream, uri;
        uri = "resource/test1.odt".toAppPackageUri();
        stream = RandomAccessStreamReference.createFromUri(uri);
        return ZipArchive.createFromStreamReferenceAsync(stream).then(function(archive) {
          expect(archive.files.length).toEqual(17);
          return archive.getFileContentsAsync('meta.xml').then(function(buffer) {
            return expect(buffer).toBeTruthy();
          }).then(function() {
            return archive.getFileContentsAsync('settings.xml');
          }).then(function(buffer) {
            return expect(buffer).toBeTruthy();
          });
        });
      });
    });

    it('should handle Office Open XML containers', function() {
      return spec.async(function() {
        var stream, uri;
        uri = "resource/test1.docx".toAppPackageUri();
        stream = RandomAccessStreamReference.createFromUri(uri);
        return ZipArchive.createFromStreamReferenceAsync(stream).then(function(archive) {
          expect(archive.files.length).toEqual(9);
          return archive.getFileContentsAsync('docProps/core.xml');
        }).then(function(buffer) {
          return expect(buffer).toBeTruthy();
        });
      });
    });

    it('should extract single files to disk', function () {
      var tempFolder;
      tempFolder = Windows.Storage.ApplicationData.current.temporaryFolder;
      return spec.async(function() {
        var stream, uri;
        uri = "resource/test1.odt".toAppPackageUri();
        stream = RandomAccessStreamReference.createFromUri(uri);
        return tempFolder.createFileAsync('temp_meta.xml', CreationCollisionOption.replaceExisting).then(function(destinationFile) {
          return ZipArchive.createFromStreamReferenceAsync(stream).then(function(archive) {
            return archive.extractFileAsync('meta.xml', destinationFile);
          }).then(function() {
            return destinationFile.getBasicPropertiesAsync();
          }).then(function(fileProperties) {
            expect(fileProperties.size).toBeGreaterThan(0);
            return tempFolder.createFileAsync('temp_content.xml', CreationCollisionOption.replaceExisting);
          });
        }).then(function(destinationFile) {
          return ZipArchive.createFromStreamReferenceAsync(stream).then(function(archive) {
            return archive.extractFileAsync('content.xml', destinationFile);
          }).then(function() {
            return destinationFile.getBasicPropertiesAsync();
          }).then(function(fileProperties) {
            return expect(fileProperties.size).toBeGreaterThan(0);
          });
        });
      });
    });

    it('should extract whole archives to disk', function () {
      var tempFolder;
      tempFolder = Windows.Storage.ApplicationData.current.temporaryFolder;
      return spec.async(function() {
        return tempFolder.createFolderAsync('unzipped', CreationCollisionOption.replaceExisting).then(function(folder) {
          var stream, uri;
          uri = "resource/test1.odt".toAppPackageUri();
          stream = RandomAccessStreamReference.createFromUri(uri);
          return ZipArchive.createFromStreamReferenceAsync(stream).then(function(archive) {
            return archive.extractAllAsync(folder);
          }).then(function() {
            var fileQueryResults, queryOptions;
            queryOptions = new Windows.Storage.Search.QueryOptions();
            queryOptions.folderDepth = Windows.Storage.Search.FolderDepth.deep;
            fileQueryResults = folder.createFileQueryWithOptions(queryOptions);
            return fileQueryResults.getFilesAsync();
          });
        }).then(function(extractedFiles) {
          return expect(extractedFiles.length).toBeGreaterThan(0);
        });
      });
    });

    return it('should throw invalid argument exception for non-existing files', function () {
      var tempFolder;
      tempFolder = Windows.Storage.ApplicationData.current.temporaryFolder;
      return spec.async(function() {
        var stream, uri;
        uri = "resource/test1.odt".toAppPackageUri();
        stream = RandomAccessStreamReference.createFromUri(uri);
        return tempFolder.createFileAsync('temp.xml', CreationCollisionOption.replaceExisting).then(function(destinationFile) {
          return ZipArchive.createFromStreamReferenceAsync(stream).then(function(archive) {
            return archive.extractFileAsync('foobar.dat', destinationFile);
          }).then(function() {
            return jasmine.getEnv().currentSpec.fail("This should have failed");
          }, function(error) {
            return expect(error.number).toEqual(-2147024809);
          });
        });
      });
    });
  });

}());
