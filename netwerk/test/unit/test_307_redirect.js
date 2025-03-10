"use strict";

const { HttpServer } = ChromeUtils.importESModule(
  "resource://testing-common/httpd.sys.mjs"
);

ChromeUtils.defineLazyGetter(this, "URL", function () {
  return "http://localhost:" + httpserver.identity.primaryPort;
});

ChromeUtils.defineLazyGetter(this, "uri", function () {
  return URL + "/redirect";
});

ChromeUtils.defineLazyGetter(this, "noRedirectURI", function () {
  return URL + "/content";
});

var httpserver = null;

function make_channel(url) {
  return NetUtil.newChannel({ uri: url, loadUsingSystemPrincipal: true });
}

const requestBody = "request body";

function redirectHandler(metadata, response) {
  response.setStatusLine(metadata.httpVersion, 307, "Moved Temporarily");
  response.setHeader("Location", noRedirectURI, false);
}

function contentHandler(metadata, response) {
  response.setHeader("Content-Type", "text/plain");
  response.bodyOutputStream.writeFrom(
    metadata.bodyInputStream,
    metadata.bodyInputStream.available()
  );
}

function noRedirectStreamObserver(request, buffer) {
  Assert.equal(buffer, requestBody);
  var chan = make_channel(uri);
  var uploadStream = Cc["@mozilla.org/io/string-input-stream;1"].createInstance(
    Ci.nsIStringInputStream
  );
  uploadStream.setByteStringData(requestBody);
  chan
    .QueryInterface(Ci.nsIUploadChannel)
    .setUploadStream(uploadStream, "text/plain", -1);
  chan.asyncOpen(new ChannelListener(noHeaderStreamObserver, null));
}

function noHeaderStreamObserver(request, buffer) {
  Assert.equal(buffer, requestBody);
  var chan = make_channel(uri);
  var uploadStream = Cc["@mozilla.org/io/string-input-stream;1"].createInstance(
    Ci.nsIStringInputStream
  );
  var streamBody =
    "Content-Type: text/plain\r\n" +
    "Content-Length: " +
    requestBody.length +
    "\r\n\r\n" +
    requestBody;
  uploadStream.setByteStringData(streamBody);
  chan
    .QueryInterface(Ci.nsIUploadChannel)
    .setUploadStream(uploadStream, "", -1);
  chan.asyncOpen(new ChannelListener(headerStreamObserver, null));
}

function headerStreamObserver(request, buffer) {
  Assert.equal(buffer, requestBody);
  httpserver.stop(do_test_finished);
}

function run_test() {
  httpserver = new HttpServer();
  httpserver.registerPathHandler("/redirect", redirectHandler);
  httpserver.registerPathHandler("/content", contentHandler);
  httpserver.start(-1);

  Services.prefs.setBoolPref("network.http.prompt-temp-redirect", false);

  var chan = make_channel(noRedirectURI);
  var uploadStream = Cc["@mozilla.org/io/string-input-stream;1"].createInstance(
    Ci.nsIStringInputStream
  );
  uploadStream.setByteStringData(requestBody);
  chan
    .QueryInterface(Ci.nsIUploadChannel)
    .setUploadStream(uploadStream, "text/plain", -1);
  chan.asyncOpen(new ChannelListener(noRedirectStreamObserver, null));
  do_test_pending();
}
