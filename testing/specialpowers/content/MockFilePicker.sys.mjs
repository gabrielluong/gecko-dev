/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  FileUtils: "resource://gre/modules/FileUtils.sys.mjs",
  WrapPrivileged: "resource://testing-common/WrapPrivileged.sys.mjs",
});

const Cm = Components.manager;

const CONTRACT_ID = "@mozilla.org/filepicker;1";

if (import.meta.url.includes("specialpowers")) {
  Cu.crashIfNotInAutomation();
}

var registrar = Cm.QueryInterface(Ci.nsIComponentRegistrar);
var oldClassID;
var newClassID = Services.uuid.generateUUID();
var newFactory = function (window) {
  return {
    createInstance(aIID) {
      return new MockFilePickerInstance(window).QueryInterface(aIID);
    },
    QueryInterface: ChromeUtils.generateQI(["nsIFactory"]),
  };
};

export var MockFilePicker = {
  returnOK: Ci.nsIFilePicker.returnOK,
  returnCancel: Ci.nsIFilePicker.returnCancel,
  returnReplace: Ci.nsIFilePicker.returnReplace,

  filterAll: Ci.nsIFilePicker.filterAll,
  filterHTML: Ci.nsIFilePicker.filterHTML,
  filterText: Ci.nsIFilePicker.filterText,
  filterImages: Ci.nsIFilePicker.filterImages,
  filterXML: Ci.nsIFilePicker.filterXML,
  filterXUL: Ci.nsIFilePicker.filterXUL,
  filterApps: Ci.nsIFilePicker.filterApps,
  filterAllowURLs: Ci.nsIFilePicker.filterAllowURLs,
  filterAudio: Ci.nsIFilePicker.filterAudio,
  filterVideo: Ci.nsIFilePicker.filterVideo,

  window: null,
  pendingPromises: [],

  init(browsingContext) {
    if (registrar.isCIDRegistered(newClassID)) {
      this.cleanup();
    } else {
      this.reset();
    }

    this.window = browsingContext.window;
    this.factory = newFactory(this.window);
    oldClassID = registrar.contractIDToCID(CONTRACT_ID);
    registrar.registerFactory(newClassID, "", CONTRACT_ID, this.factory);
  },

  reset() {
    this.appendFilterCallback = null;
    this.appendFiltersCallback = null;
    this.displayDirectory = null;
    this.displaySpecialDirectory = "";
    this.filterIndex = 0;
    this.mode = null;
    this.returnData = [];
    this.returnValue = null;
    this.returnDataForWebKitDirs = [];
    this.showCallback = null;
    this.afterOpenCallback = null;
    this.shown = false;
    this.showing = false;
  },

  cleanup() {
    var previousFactory = this.factory;
    this.reset();
    this.factory = null;
    this.window = null;
    if (oldClassID) {
      registrar.unregisterFactory(newClassID, previousFactory);
      registrar.registerFactory(oldClassID, "", CONTRACT_ID, null);
    }
  },

  internalFileData(obj) {
    return {
      nsIFile: "nsIFile" in obj ? obj.nsIFile : null,
      domFile: "domFile" in obj ? obj.domFile : null,
      domDirectory: "domDirectory" in obj ? obj.domDirectory : null,
    };
  },

  useAnyFile() {
    var file = lazy.FileUtils.getDir("TmpD", []);
    file.append("testfile");
    file.createUnique(Ci.nsIFile.NORMAL_FILE_TYPE, 0o644);
    let promise = this.window.File.createFromNsIFile(file)
      .then(
        domFile => domFile,
        () => null
      )
      // domFile can be null.
      .then(domFile => {
        this.returnData = [this.internalFileData({ nsIFile: file, domFile })];
      })
      .then(() => file);

    this.pendingPromises = [promise];

    // We return a promise in order to support some existing mochitests.
    return promise;
  },

  useBlobFile() {
    var blob = new this.window.Blob([]);
    var file = new this.window.File([blob], "helloworld.txt", {
      type: "plain/text",
    });
    this.returnData = [this.internalFileData({ domFile: file })];
    this.pendingPromises = [];
  },

  useDirectory(aPath) {
    var directory = new this.window.Directory(aPath);
    var file = new lazy.FileUtils.File(aPath);
    this.returnData = [
      this.internalFileData({ domDirectory: directory, nsIFile: file }),
    ];
    this.pendingPromises = [];
    this.returnDataForWebKitDirs = [];

    if (AppConstants.platform === "android") {
      for (const filename of ["/foo.txt", "/subdir/bar.txt"]) {
        let fileInDir = lazy.FileUtils.File(aPath + filename);
        this.window.File.createFromNsIFile(file, {
          existenceCheck: false,
        }).then(domFile => {
          this.returnDataForWebKitDirs.push(
            this.internalFileData({ nsIFile: fileInDir, domFile })
          );
        });
        // promise might be added into pendinngPromise, but it causes
        // InvalidStateError.
      }
    }
  },

  setFiles(files) {
    this.returnData = [];
    this.pendingPromises = [];

    for (let file of files) {
      if (this.window.File.isInstance(file)) {
        this.returnData.push(this.internalFileData({ domFile: file }));
      } else {
        let promise = this.window.File.createFromNsIFile(file, {
          existenceCheck: false,
        });

        promise.then(domFile => {
          this.returnData.push(
            this.internalFileData({ nsIFile: file, domFile })
          );
        });
        this.pendingPromises.push(promise);
      }
    }
  },

  getNsIFile() {
    if (this.returnData.length >= 1) {
      return this.returnData[0].nsIFile;
    }
    return null;
  },
};

function MockFilePickerInstance(window) {
  this.window = window;
  this.showCallback = null;
  this.showCallbackWrapped = null;
}
MockFilePickerInstance.prototype = {
  QueryInterface: ChromeUtils.generateQI(["nsIFilePicker"]),
  init(aParent, aTitle, aMode) {
    this.mode = aMode;
    this.filterIndex = MockFilePicker.filterIndex;
    this.parent = aParent;
  },
  appendFilter(aTitle, aFilter) {
    if (typeof MockFilePicker.appendFilterCallback == "function") {
      MockFilePicker.appendFilterCallback(this, aTitle, aFilter);
    }
  },
  appendFilters(aFilterMask) {
    if (typeof MockFilePicker.appendFiltersCallback == "function") {
      MockFilePicker.appendFiltersCallback(this, aFilterMask);
    }
  },
  defaultString: "",
  defaultExtension: "",
  parent: null,
  filterIndex: 0,
  displayDirectory: null,
  displaySpecialDirectory: "",
  get file() {
    if (MockFilePicker.returnData.length >= 1) {
      return MockFilePicker.returnData[0].nsIFile;
    }

    return null;
  },

  // We don't support directories here.
  get domFileOrDirectory() {
    if (MockFilePicker.returnData.length < 1) {
      return null;
    }

    if (MockFilePicker.returnData[0].domFile) {
      return MockFilePicker.returnData[0].domFile;
    }

    if (MockFilePicker.returnData[0].domDirectory) {
      return MockFilePicker.returnData[0].domDirectory;
    }

    return null;
  },
  get fileURL() {
    if (
      MockFilePicker.returnData.length >= 1 &&
      MockFilePicker.returnData[0].nsIFile
    ) {
      return Services.io.newFileURI(MockFilePicker.returnData[0].nsIFile);
    }

    return null;
  },
  *getFiles(asDOM) {
    for (let d of MockFilePicker.returnData) {
      if (asDOM) {
        yield d.domFile || d.domDirectory;
      } else if (d.nsIFile) {
        yield d.nsIFile;
      } else {
        throw Components.Exception("", Cr.NS_ERROR_FAILURE);
      }
    }
  },
  get files() {
    return this.getFiles(false);
  },
  get domFileOrDirectoryEnumerator() {
    return this.getFiles(true);
  },
  *getDomFilesInWebKitDirectory() {
    for (let d of MockFilePicker.returnDataForWebKitDirs) {
      yield d.domFile;
    }
  },
  get domFilesInWebKitDirectory() {
    if (AppConstants.platform !== "android") {
      throw Components.Exception("", Cr.NS_ERROR_FAILURE);
    }

    return this.getDomFilesInWebKitDirectory();
  },
  open(aFilePickerShownCallback) {
    MockFilePicker.showing = true;
    Services.tm.dispatchToMainThread(() => {
      // Maybe all the pending promises are already resolved, but we want to be sure.
      Promise.all(MockFilePicker.pendingPromises)
        .then(
          () => {
            return Ci.nsIFilePicker.returnOK;
          },
          () => {
            return Ci.nsIFilePicker.returnCancel;
          }
        )
        .then(result => {
          // Nothing else has to be done.
          MockFilePicker.pendingPromises = [];

          if (result == Ci.nsIFilePicker.returnCancel) {
            return result;
          }

          MockFilePicker.displayDirectory = this.displayDirectory;
          MockFilePicker.displaySpecialDirectory = this.displaySpecialDirectory;
          MockFilePicker.shown = true;
          if (typeof MockFilePicker.showCallback == "function") {
            if (MockFilePicker.showCallback != this.showCallback) {
              this.showCallback = MockFilePicker.showCallback;
              if (Cu.isXrayWrapper(this.window)) {
                this.showCallbackWrapped = lazy.WrapPrivileged.wrapCallback(
                  MockFilePicker.showCallback,
                  this.window
                );
              } else {
                this.showCallbackWrapped = this.showCallback;
              }
            }
            try {
              var returnValue = this.showCallbackWrapped(this);
              if (typeof returnValue != "undefined") {
                return returnValue;
              }
            } catch (ex) {
              return Ci.nsIFilePicker.returnCancel;
            }
          }

          return MockFilePicker.returnValue;
        })
        .then(result => {
          // Some additional result file can be set by the callback. Let's
          // resolve the pending promises again.
          return Promise.all(MockFilePicker.pendingPromises).then(
            () => {
              return result;
            },
            () => {
              return Ci.nsIFilePicker.returnCancel;
            }
          );
        })
        .then(result => {
          MockFilePicker.pendingPromises = [];

          if (aFilePickerShownCallback) {
            aFilePickerShownCallback.done(result);
          }

          if (typeof MockFilePicker.afterOpenCallback == "function") {
            Services.tm.dispatchToMainThread(() => {
              MockFilePicker.afterOpenCallback(this);
            });
          }
        });
    });
  },
};
